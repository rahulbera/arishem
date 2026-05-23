/*
 * champsim_tracer_mt_roi_v2.cpp
 *
 * ROI-marker-aware, optimized, multi-threaded, periodically-sampled
 * PIN-based trace generator for the EXTENDED ChampSim trace format
 * (input_instr_v2, 512 bytes per record), with online Zstandard
 * compression.
 *
 * This tool is derived from champsim_tracer_mt_roi.cpp (64-byte v1
 * records). All runtime machinery (per-thread state machine, ROI
 * markers, TRACE-granularity skip phases, PIN_RemoveInstrumentation()
 * phase flushes, per-thread streaming zstd) is preserved. The new
 * behaviour concerns ONLY the record layout and the fields it captures:
 *
 *   - Virtual addresses of loads (up to 4) and stores (up to 2)         [v1]
 *   - Physical addresses                              ZERO under PIN   [v2]
 *   - Per-operand access sizes in bytes                                 [v2]
 *   - Privilege bit                                   ZERO under PIN   [v2]
 *   - Instruction type (INT / FP / SIMD), classified at instrumentation [v2]
 *   - Load values   (PIN_SafeCopy at IPOINT_BEFORE)                     [v2]
 *   - Store values  (PIN_SafeCopy at IPOINT_AFTER)                      [v2]
 *
 * Rationale for capture points:
 *   Loads  -> BEFORE is correct: the memory bytes at the effective
 *             address are already what the load will fetch; PIN has no
 *             race with the application (single thread executing this
 *             instruction at this time).
 *   Stores -> AFTER is correct: the value has landed at the effective
 *             address. IPOINT_AFTER is only legal when the instruction
 *             has a fall-through. For the (rare) no-fall-through store
 *             we leave the value zeroed and bump a global counter that
 *             is printed in Fini.
 *
 * OUTPUT FILES
 * ------------
 *   <base>_t<os_tid>_master_s<sid>.champsim2.zst  (master thread -- discard)
 *   <base>_t<os_tid>_s<sid>.champsim2.zst         (worker threads -- keep)
 *
 *   The ".champsim2.zst" suffix distinguishes 512-byte v2 records from
 *   the v1 ".champsim.zst" output. Do not mix the two in a ChampSim run.
 *
 * KNOB SUMMARY (additions over v1)
 * --------------------------------
 *   -values <0|1>  : Enable value capture for loads/stores. Default 1.
 *                    Set 0 to emit zero-filled value slots for speed or
 *                    backward-compatibility testing. Addresses, sizes,
 *                    and all v1 fields are always captured.
 *
 * All v1 knobs (-use_markers, -i, -s, -t, -n, -main_only, -zstd_level,
 * -o) behave identically to the v1 tracer.
 *
 * BUILD
 * -----
 *   Add to make_tracer.sh:
 *     make obj-intel64/champsim_tracer_mt_roi_v2.so
 *   Links against libzstd (same as v1).
 *   Requires PIN 3.17+ on Linux x86-64.
 *
 * USAGE
 * -----
 *   pin -t obj-intel64/champsim_tracer_mt_roi_v2.so \
 *       -use_markers 1                               \
 *       -o traces/faiss_hnsw                         \
 *       -t 10000000                                  \
 *       -n 1                                         \
 *       [-values 0]                                  \
 *       [-zstd_level 1]                              \
 *       -- ./faiss_driver ...
 */

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <pin.H>
#include <sstream>
#include <string>
#include <zstd.h>

/* =========================================================================
 * Extended ChampSim trace record (v2).
 *
 * Layout is BYTE-FOR-BYTE identical to input_instr_v2 declared in
 * champsim/inc/instruction.h. We redefine it here locally to avoid
 * pulling the ChampSim simulator headers into the PIN tool build.
 * The static_assert below guards the 512-byte invariant.
 * ========================================================================= */

#define NUM_INSTR_DESTINATIONS 2
#define NUM_INSTR_SOURCES      4
#define MAX_MEM_VALUE_SIZE     64  // AVX-512: 512 bits

#define INSTR_TYPE_INT  0
#define INSTR_TYPE_FP   1
#define INSTR_TYPE_SIMD 2

struct __attribute__((packed)) trace_instr_v2_t {
  // --- Block 1: vanilla 64-byte layout ---
  uint64_t ip;
  uint8_t  is_branch;
  uint8_t  branch_taken;
  uint8_t  destination_registers[NUM_INSTR_DESTINATIONS];
  uint8_t  source_registers[NUM_INSTR_SOURCES];
  uint64_t destination_memory[NUM_INSTR_DESTINATIONS];       // VA
  uint64_t source_memory[NUM_INSTR_SOURCES];                 // VA

  // --- Block 2: PA + metadata (64 bytes) ---
  uint64_t destination_memory_pa[NUM_INSTR_DESTINATIONS];    // PA
  uint64_t source_memory_pa[NUM_INSTR_SOURCES];              // PA
  uint8_t  source_memory_size[NUM_INSTR_SOURCES];
  uint8_t  destination_memory_size[NUM_INSTR_DESTINATIONS];
  uint8_t  privilege;
  uint8_t  instr_type;
  uint8_t  reserved[8];

  // --- Block 3: memory values (384 bytes) ---
  uint8_t  source_memory_value[NUM_INSTR_SOURCES][MAX_MEM_VALUE_SIZE];
  uint8_t  destination_memory_value[NUM_INSTR_DESTINATIONS][MAX_MEM_VALUE_SIZE];
};

static_assert(sizeof(trace_instr_v2_t) == 512,
              "trace_instr_v2_t must be 512 bytes (matches input_instr_v2)");

/* =========================================================================
 * ROI marker constants -- must match champsim_markers.h
 * ========================================================================= */

#define CHAMPSIM_ROI_BEGIN ((ADDRINT)1)
#define CHAMPSIM_ROI_END   ((ADDRINT)2)

/* =========================================================================
 * Output buffer size
 *
 * 128 KB per thread. With 512-byte records the zstd output rate is
 * lower per record than in v1 (most of the new bytes are zeros for
 * scalar workloads and compress extremely well), so the same buffer
 * amortizes far more records per flush.
 * ========================================================================= */

static constexpr size_t OUT_BUF_SIZE = 128 * 1024;

/* =========================================================================
 * Knobs
 * ========================================================================= */

KNOB<std::string> KnobOutputBase(
  KNOB_MODE_WRITEONCE,
  "pintool",
  "o",
  "champsim_mt",
  "Base name for output trace files. "
  "Files: <base>_t<os_tid>[_master]_s<sid>.champsim2.zst");

KNOB<BOOL> KnobUseMarkers(
  KNOB_MODE_WRITEONCE,
  "pintool",
  "use_markers",
  "0",
  "If 1, use champsim_roi_begin/end markers to gate tracing "
  "(see champsim_markers.h). The -i knob is ignored in this mode. "
  "If 0, use -i for initial skip (legacy mode). Default: 0.");

KNOB<UINT64> KnobInitialSkip(
  KNOB_MODE_WRITEONCE,
  "pintool",
  "i",
  "0",
  "Initial skip (skip-based mode only, ignored when -use_markers 1): "
  "instructions to skip at thread start before tracing. Default: 0.");

KNOB<UINT64> KnobInterSampleSkip(
  KNOB_MODE_WRITEONCE,
  "pintool",
  "s",
  "0",
  "Inter-sample skip: instructions to skip between sample windows. "
  "Applies in both modes. Default: 0.");

KNOB<UINT64> KnobTraceInstructions(
  KNOB_MODE_WRITEONCE,
  "pintool",
  "t",
  "1000000",
  "Instructions to trace per sample window. Default: 1,000,000.");

KNOB<UINT64> KnobNumSamples(
  KNOB_MODE_WRITEONCE,
  "pintool",
  "n",
  "1",
  "Max sample windows per thread. 0 = unlimited. Default: 1.");

KNOB<BOOL> KnobMainThreadOnly(
  KNOB_MODE_WRITEONCE,
  "pintool",
  "main_only",
  "0",
  "If 1, trace only the main (root) thread. Default: 0.");

KNOB<INT32> KnobZstdLevel(
  KNOB_MODE_WRITEONCE,
  "pintool",
  "zstd_level",
  "1",
  "Zstandard compression level (1-22). Level 1 is strongly recommended "
  "to avoid bottlenecking PIN (~500-800 MB/s). Level 3 is acceptable if "
  "you observe headroom (~300 MB/s). Never exceed 3 during tracing. "
  "Default: 1.");

KNOB<BOOL> KnobCaptureValues(
  KNOB_MODE_WRITEONCE,
  "pintool",
  "values",
  "1",
  "If 1 (default), capture memory load/store values via PIN_SafeCopy. "
  "If 0, leave value slots zero-filled (faster, produces a v2-shaped "
  "record with addresses and sizes but no values).");

KNOB<BOOL> KnobExitOnDone(
  KNOB_MODE_WRITEONCE,
  "pintool",
  "exit_on_done",
  "0",
  "If 1, call PIN_ExitApplication(0) once every thread that ever entered "
  "TRACING has reached DONE (quota met or ROI-end). Terminates the driver "
  "cleanly after the last sample so post-trace work does not run under PIN. "
  "Default: 0.");

/* =========================================================================
 * Per-thread phase state machine
 * ========================================================================= */

enum class Phase {
  WAITING_FOR_ROI,
  INITIAL_SKIP,
  TRACING,
  INTER_SKIP,
  DONE
};

static const char *phase_name(Phase p)
{
  switch (p) {
  case Phase::WAITING_FOR_ROI:
    return "WAITING_FOR_ROI";
  case Phase::INITIAL_SKIP:
    return "INITIAL_SKIP";
  case Phase::TRACING:
    return "TRACING";
  case Phase::INTER_SKIP:
    return "INTER_SKIP";
  case Phase::DONE:
    return "DONE";
  default:
    return "UNKNOWN";
  }
}

/* =========================================================================
 * Global warning counters -- printed in Fini.
 *
 * These track pathological cases that are handled gracefully (we never
 * crash or corrupt a record) but that the user should know about when
 * analyzing traces:
 *
 *   g_overflowed_src_memops:  Executed memory-read operands that could
 *                             not fit in the 4 src slots. Address and
 *                             value for the overflow are dropped.
 *   g_overflowed_dst_memops:  Same, for write operands past the 2 dst
 *                             slots.
 *   g_missed_store_values:    Store operands that could not be captured
 *                             at IPOINT_AFTER because the carrying
 *                             instruction has no architectural fall-
 *                             through. Calls are excluded -- their
 *                             return-address push is synthesised at
 *                             pre-call time (see RecordCallRetAddrValue).
 *   g_call_store_values_filled: CALL return-address store values filled
 *                             synthetically from INS_NextAddress + RSP.
 *   g_truncated_values:       Memory operands wider than
 *                             MAX_MEM_VALUE_SIZE (64 B). Shouldn't
 *                             happen on x86 prior to AVX-1024.
 *   g_safecopy_short_reads:   PIN_SafeCopy returned fewer bytes than
 *                             requested (unmapped page, guard page,
 *                             etc.). The captured prefix is kept,
 *                             remaining bytes left zero.
 *   g_scattered_instrs_seen:  Dynamic count of vgather/vscatter (and any
 *                             other INS_HasScatteredMemoryAccess) instrs
 *                             executed under tracing. Their per-lane
 *                             addresses are drained via
 *                             IARG_MULTI_MEMORYACCESS_EA into the normal
 *                             src/dst slots (overflow lanes fall into
 *                             g_overflowed_src_memops/g_overflowed_dst_memops).
 *   g_scatter_missed_store_values:
 *                             Scatter (store) lanes whose post-write value
 *                             we deliberately did not capture. IPOINT_AFTER
 *                             value capture is not viable per-lane for
 *                             scattered stores, so each masked-on store
 *                             lane bumps both g_missed_store_values and
 *                             this scatter-specific sub-counter.
 * ========================================================================= */

static std::atomic<uint64_t> g_overflowed_src_memops{0};
static std::atomic<uint64_t> g_overflowed_dst_memops{0};
static std::atomic<uint64_t> g_missed_store_values{0};
static std::atomic<uint64_t> g_call_store_values_filled{0};
static std::atomic<uint64_t> g_truncated_values{0};
static std::atomic<uint64_t> g_safecopy_short_reads{0};
static std::atomic<uint64_t> g_scattered_instrs_seen{0};
static std::atomic<uint64_t> g_scatter_missed_store_values{0};

// --- exit_on_done bookkeeping ---
// Counts threads that ever entered TRACING and that ever reached DONE,
// plus a one-shot flag that guards the PIN_ExitApplication() call so it
// runs at most once across all threads.
static std::atomic<uint64_t> g_threads_started_tracing{0};
static std::atomic<uint64_t> g_threads_reached_done{0};
static std::atomic<bool>     g_exit_triggered{false};

/* =========================================================================
 * Per-thread state
 * ========================================================================= */

struct ThreadState {
  // --- State machine ---
  Phase  phase;
  UINT64 counter;

  // --- Sample tracking ---
  UINT64 samples_collected;
  UINT64 sample_limit;

  // --- Compressed output ---
  FILE      *fp;
  ZSTD_CCtx *zstd_ctx;
  uint8_t   *out_buf;
  size_t     out_buf_pos;

  // --- Identification ---
  std::string  base_name;
  OS_THREAD_ID os_tid;
  bool         is_master;
  int          zstd_level;
  bool         capture_values;

  // --- Instruction record being built ---
  trace_instr_v2_t curr_instr;

  // --- Cached knob values ---
  UINT64 inter_sample_skip;
  UINT64 trace_per_sample;

  // --- Store-value post-call plumbing ---
  // When a write pre-call fills destination_memory[k] at runtime, it
  // remembers (op_idx -> k) so the matching IPOINT_AFTER post-call can
  // drop the captured value into destination_memory_value[k] without
  // rescanning the slot array. Reset by RecordInstr at end-of-record.
  // Sized by INS_MemoryOperandCount upper bound: x86 instructions
  // rarely have more than a couple of write operands, but PIN allows
  // up to 8. Using 8 for safety.
  static constexpr int MAX_WRITE_OPS = 8;
  int                  pending_store_slot[MAX_WRITE_OPS];
  ADDRINT              pending_store_addr[MAX_WRITE_OPS];
  UINT32               pending_store_size[MAX_WRITE_OPS];

  // --- One-shot lifecycle flags (for -exit_on_done bookkeeping) ---
  bool ever_started_tracing;
  bool ever_reached_done;

  ThreadState() = delete;

  explicit ThreadState(OS_THREAD_ID       tid,
                       const std::string &base,
                       Phase              starting_phase,
                       UINT64             initial_counter,
                       UINT64             inter_skip,
                       UINT64             trace_count,
                       UINT64             num_samples,
                       bool               master,
                       int                level,
                       bool               values)
      : phase(starting_phase),
        counter(initial_counter),
        samples_collected(0),
        sample_limit(num_samples),
        fp(nullptr),
        zstd_ctx(nullptr),
        out_buf(new uint8_t[OUT_BUF_SIZE]),
        out_buf_pos(0),
        base_name(base),
        os_tid(tid),
        is_master(master),
        zstd_level(level),
        capture_values(values),
        inter_sample_skip(inter_skip),
        trace_per_sample(trace_count),
        ever_started_tracing(false),
        ever_reached_done(false)
  {
    reset_instr();
    reset_pending_stores();
    if (phase == Phase::TRACING)
      open_next_sample();
  }

  ~ThreadState()
  {
    delete[] out_buf;
  }

  std::string sample_filename() const
  {
    std::ostringstream ss;
    ss << base_name << "_t" << os_tid;
    if (is_master)
      ss << "_master";
    ss << "_s" << samples_collected << ".champsim2.zst";
    return ss.str();
  }

  void open_next_sample();
  void close_sample();
  void compress_write(const void *data, size_t size);
  bool finish_sample();

  void force_close()
  {
    if (fp && zstd_ctx) {
      close_sample();
    } else if (fp) {
      std::fclose(fp);
      fp = nullptr;
    }
  }

  void reset_instr()
  {
    std::memset(&curr_instr, 0, sizeof(curr_instr));
  }

  void reset_pending_stores()
  {
    for (int i = 0; i < MAX_WRITE_OPS; i++) {
      pending_store_slot[i] = -1;
      pending_store_addr[i] = 0;
      pending_store_size[i] = 0;
    }
  }
};

/* =========================================================================
 * Thread-safe logging
 * ========================================================================= */

PIN_MUTEX cerr_lock;

struct LogGuard {
  __attribute__((always_inline)) LogGuard()
  {
    PIN_MutexLock(&cerr_lock);
  }

  __attribute__((always_inline)) ~LogGuard()
  {
    PIN_MutexUnlock(&cerr_lock);
  }

  LogGuard(const LogGuard &)            = delete;
  LogGuard &operator=(const LogGuard &) = delete;
};

/* =========================================================================
 * Global state
 * ========================================================================= */

static ThreadState *thread_states[PIN_MAX_THREADS];
static PIN_RWMUTEX  registry_lock;
static THREADID     main_thread_id = INVALID_THREADID;

static std::atomic<int>  active_tracing_threads{0};
static std::atomic<bool> roi_started{false};
static std::atomic<bool> roi_ended{false};

static inline ThreadState *get_state(THREADID tid)
{
  return thread_states[tid];
}

/* =========================================================================
 * exit_on_done helpers
 *
 * mark_started_tracing / mark_reached_done are one-shot per thread; each
 * flips the per-thread flag and bumps a global counter exactly once. Once
 * every thread that ever entered TRACING has reached DONE, and exit_on_done
 * is enabled, maybe_exit_on_done() calls PIN_ExitApplication(0) exactly
 * once (guarded by g_exit_triggered). PIN runs Fini on exit.
 * ========================================================================= */

static void maybe_exit_on_done();

static inline void mark_started_tracing(ThreadState *ts)
{
  if (!ts || ts->ever_started_tracing)
    return;
  ts->ever_started_tracing = true;
  g_threads_started_tracing.fetch_add(1, std::memory_order_acq_rel);
}

static inline void mark_reached_done(ThreadState *ts)
{
  if (!ts || ts->ever_reached_done)
    return;
  ts->ever_reached_done = true;
  g_threads_reached_done.fetch_add(1, std::memory_order_acq_rel);
  maybe_exit_on_done();
}

static void maybe_exit_on_done()
{
  if (!KnobExitOnDone.Value())
    return;

  uint64_t started = g_threads_started_tracing.load(std::memory_order_acquire);
  uint64_t done    = g_threads_reached_done.load(std::memory_order_acquire);
  if (started == 0 || done < started)
    return;

  bool expected = false;
  if (!g_exit_triggered.compare_exchange_strong(expected,
                                                true,
                                                std::memory_order_acq_rel))
    return;

  {
    LogGuard _lg;
    std::cerr << "[tracer_roi_v2] exit_on_done: all " << started
              << " traced thread(s) reached DONE. "
              << "Calling PIN_ExitApplication(0)." << std::endl;
  }
  PIN_ExitApplication(0);
}

/* =========================================================================
 * ThreadState methods that need access to LogGuard / globals
 * ========================================================================= */

void ThreadState::open_next_sample()
{
  std::string fname = sample_filename();

  fp = std::fopen(fname.c_str(), "wb");
  if (!fp) {
    {
      LogGuard _lg;
      std::cerr << "[tracer_roi_v2] ERROR: cannot open: " << fname << std::endl;
    }
    return;
  }

  zstd_ctx = ZSTD_createCCtx();
  if (!zstd_ctx) {
    {
      LogGuard _lg;
      std::cerr << "[tracer_roi_v2] ERROR: ZSTD_createCCtx failed: " << fname
                << std::endl;
    }
    std::fclose(fp);
    fp = nullptr;
    return;
  }

  size_t rc = ZSTD_CCtx_setParameter(zstd_ctx,
                                     ZSTD_c_compressionLevel,
                                     zstd_level);
  if (ZSTD_isError(rc)) {
    LogGuard _lg;
    std::cerr << "[tracer_roi_v2] WARNING: cannot set zstd level "
              << zstd_level << ": " << ZSTD_getErrorName(rc) << std::endl;
  }

  out_buf_pos = 0;

  {
    LogGuard _lg;
    std::cerr << "[tracer_roi_v2] Thread " << os_tid
              << (is_master ? " (master)" : "") << " sample "
              << samples_collected << " -> " << fname << " (zstd level "
              << zstd_level << ", values=" << (capture_values ? 1 : 0) << ")"
              << std::endl;
  }
}

void ThreadState::close_sample()
{
  if (!fp || !zstd_ctx)
    return;

  ZSTD_inBuffer empty_in = {nullptr, 0, 0};
  size_t        remaining;
  do {
    ZSTD_outBuffer out = {out_buf, OUT_BUF_SIZE, out_buf_pos};
    remaining = ZSTD_compressStream2(zstd_ctx, &out, &empty_in, ZSTD_e_end);

    if (ZSTD_isError(remaining)) {
      LogGuard _lg;
      std::cerr << "[tracer_roi_v2] ERROR: zstd finalization: "
                << ZSTD_getErrorName(remaining) << std::endl;
      break;
    }

    out_buf_pos = out.pos;
    if (out_buf_pos > 0) {
      std::fwrite(out_buf, 1, out_buf_pos, fp);
      out_buf_pos = 0;
    }
  } while (remaining > 0);

  ZSTD_freeCCtx(zstd_ctx);
  zstd_ctx = nullptr;
  std::fclose(fp);
  fp = nullptr;
}

bool ThreadState::finish_sample()
{
  close_sample();
  samples_collected++;

  {
    LogGuard _lg;
    std::cerr << "[tracer_roi_v2] Thread " << os_tid
              << (is_master ? " (master)" : "") << " sample "
              << (samples_collected - 1) << " complete." << std::endl;
  }

  if (sample_limit > 0 && samples_collected >= sample_limit) {
    phase = Phase::DONE;
    {
      LogGuard _lg;
      std::cerr << "[tracer_roi_v2] Thread " << os_tid
                << " quota reached. Done." << std::endl;
    }
    mark_reached_done(this);
    return false;
  }

  if (inter_sample_skip > 0) {
    phase   = Phase::INTER_SKIP;
    counter = inter_sample_skip;
    return false;
  }

  phase   = Phase::TRACING;
  counter = trace_per_sample;
  open_next_sample();
  return true;
}

void ThreadState::compress_write(const void *data, size_t size)
{
  if (!fp || !zstd_ctx)
    return;

  ZSTD_inBuffer in = {data, size, 0};

  while (in.pos < in.size) {
    ZSTD_outBuffer out = {out_buf, OUT_BUF_SIZE, out_buf_pos};

    size_t rc = ZSTD_compressStream2(zstd_ctx, &out, &in, ZSTD_e_continue);

    if (ZSTD_isError(rc)) {
      LogGuard _lg;
      std::cerr << "[tracer_roi_v2] ERROR: ZSTD_compressStream2: "
                << ZSTD_getErrorName(rc) << std::endl;
      return;
    }

    out_buf_pos = out.pos;

    if (out_buf_pos == OUT_BUF_SIZE) {
      std::fwrite(out_buf, 1, OUT_BUF_SIZE, fp);
      out_buf_pos = 0;
    }
  }
}

/* =========================================================================
 * ROI marker detection
 * ========================================================================= */

static bool is_roi_marker(INS ins)
{
  if (INS_Opcode(ins) != XED_ICLASS_XCHG)
    return false;
  if (INS_OperandCount(ins) < 2)
    return false;
  if (!INS_OperandIsReg(ins, 0) || !INS_OperandIsReg(ins, 1))
    return false;
  REG r0 = REG_FullRegName(INS_OperandReg(ins, 0));
  REG r1 = REG_FullRegName(INS_OperandReg(ins, 1));
  return (r0 == LEVEL_BASE::REG_RCX && r1 == LEVEL_BASE::REG_RCX);
}

/* =========================================================================
 * Instruction type classification (INT / FP / SIMD)
 *
 * Executed once per instruction at TRACE instrumentation time; the
 * resulting tag is baked into the INS_InsertCall argument list and
 * reaches RecordInstr() as an immediate. No per-execution overhead.
 *
 * Classification rule (per project instructions):
 *   X87_ALU                              -> FP
 *   SSE* / AVX* / MMX / AES / PCLMULQDQ /
 *   FMA* / GFNI / VAES / VPCLMULQDQ /
 *   SHA / 3DNOW / XOP                    -> SIMD
 *   everything else                      -> INT
 *
 * Scalar SSE/AVX FP instructions (addss, mulsd, etc.) fall under the
 * SSE/AVX category and are therefore tagged SIMD. This matches the
 * category-level PIN/XED taxonomy; post-hoc re-classification into
 * scalar-FP is possible offline using Zydis or XED if needed.
 * ========================================================================= */

static uint8_t classify_instr(INS ins)
{
  std::string cat = CATEGORY_StringShort(INS_Category(ins));

  if (cat == "X87_ALU")
    return INSTR_TYPE_FP;

  // Fast prefix check for SSE*/AVX* families.
  if (cat.compare(0, 3, "SSE") == 0 || cat.compare(0, 3, "AVX") == 0)
    return INSTR_TYPE_SIMD;

  if (cat == "MMX" || cat == "AES" || cat == "PCLMULQDQ" ||
      cat == "FMA4" || cat == "VFMA" || cat == "GFNI" ||
      cat == "VAES" || cat == "VPCLMULQDQ" || cat == "SHA" ||
      cat == "3DNOW" || cat == "XOP")
    return INSTR_TYPE_SIMD;

  return INSTR_TYPE_INT;
}

/* =========================================================================
 * Phase transition helpers
 * ========================================================================= */

static void enter_tracing(ThreadState *ts)
{
  ts->open_next_sample();
  active_tracing_threads.fetch_add(1, std::memory_order_acq_rel);
  mark_started_tracing(ts);
  PIN_RemoveInstrumentation();
}

static bool leave_tracing(ThreadState *ts)
{
  bool re_entered = ts->finish_sample();
  if (!re_entered) {
    active_tracing_threads.fetch_sub(1, std::memory_order_acq_rel);
    PIN_RemoveInstrumentation();
  }
  return re_entered;
}

/* =========================================================================
 * Analysis callbacks -- marker handling
 * ========================================================================= */

VOID HandleMarker(THREADID tid, ADDRINT rcx_val)
{
  ThreadState *ts = get_state(tid);

  if (rcx_val == CHAMPSIM_ROI_BEGIN) {
    bool expected = false;
    if (!roi_started.compare_exchange_strong(expected,
                                             true,
                                             std::memory_order_acq_rel))
      return;

    {
      LogGuard _lg;
      std::cerr << "[tracer_roi_v2] ROI begin detected on thread "
                << (ts ? ts->os_tid : (OS_THREAD_ID)-1) << std::endl;
    }

    if (!ts)
      return;

    ts->is_master = true;

    if (ts->phase == Phase::WAITING_FOR_ROI ||
        ts->phase == Phase::INITIAL_SKIP) {
      ts->phase   = Phase::TRACING;
      ts->counter = ts->trace_per_sample;
      enter_tracing(ts);
    }

  } else if (rcx_val == CHAMPSIM_ROI_END) {
    bool expected = false;
    if (!roi_ended.compare_exchange_strong(expected,
                                           true,
                                           std::memory_order_acq_rel))
      return;

    {
      LogGuard _lg;
      std::cerr << "[tracer_roi_v2] ROI end detected on thread "
                << (ts ? ts->os_tid : (OS_THREAD_ID)-1) << std::endl;
    }

    if (!ts)
      return;

    if (ts->phase == Phase::TRACING) {
      active_tracing_threads.fetch_sub(1, std::memory_order_acq_rel);
      ts->force_close();
      {
        LogGuard _lg;
        std::cerr << "[tracer_roi_v2] Thread " << ts->os_tid
                  << " stopped at ROI end (partial sample "
                  << ts->samples_collected << " kept)." << std::endl;
      }
    }
    ts->phase = Phase::DONE;
    mark_reached_done(ts);

    PIN_RemoveInstrumentation();
  }
}

/* =========================================================================
 * Analysis callbacks -- ROI transition check (TRACE granularity)
 * ========================================================================= */

VOID CheckROITransition(THREADID tid)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase == Phase::DONE)
    return;

  if (roi_ended.load(std::memory_order_acquire)) {
    if (ts->phase == Phase::TRACING) {
      active_tracing_threads.fetch_sub(1, std::memory_order_acq_rel);
      ts->force_close();
      {
        LogGuard _lg;
        std::cerr << "[tracer_roi_v2] Thread " << ts->os_tid
                  << " -> DONE via CheckROITransition (roi_ended)."
                  << std::endl;
      }
    }
    ts->phase = Phase::DONE;
    mark_reached_done(ts);
    return;
  }

  if (roi_started.load(std::memory_order_acquire) &&
      ts->phase == Phase::WAITING_FOR_ROI) {
    ts->phase   = Phase::TRACING;
    ts->counter = ts->trace_per_sample;
    ts->open_next_sample();
    active_tracing_threads.fetch_add(1, std::memory_order_acq_rel);
    mark_started_tracing(ts);
    {
      LogGuard _lg;
      std::cerr << "[tracer_roi_v2] Thread " << ts->os_tid
                << " -> TRACING via CheckROITransition." << std::endl;
    }
  }
}

/* =========================================================================
 * Analysis callbacks -- skip phase (TRACE granularity, near-native)
 * ========================================================================= */

VOID FastForwardInitial(THREADID tid, UINT32 trace_icount)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase != Phase::INITIAL_SKIP)
    return;

  if (ts->counter > (UINT64)trace_icount) {
    ts->counter -= trace_icount;
  } else {
    ts->counter = 0;
    ts->phase   = Phase::TRACING;
    ts->counter = ts->trace_per_sample;
    enter_tracing(ts);
  }
}

VOID FastForwardInter(THREADID tid, UINT32 trace_icount)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase != Phase::INTER_SKIP)
    return;

  if (ts->counter > (UINT64)trace_icount) {
    ts->counter -= trace_icount;
  } else {
    ts->counter = 0;
    ts->phase   = Phase::TRACING;
    ts->counter = ts->trace_per_sample;
    enter_tracing(ts);
  }
}

/* =========================================================================
 * Analysis callbacks -- memory operand recording (hot path)
 * ========================================================================= */

// Clamp a requested memory-value width to MAX_MEM_VALUE_SIZE and bump
// the truncation counter when the requested width exceeds the cap.
static inline UINT32 clamp_value_size(UINT32 size)
{
  if (size > MAX_MEM_VALUE_SIZE) {
    g_truncated_values.fetch_add(1, std::memory_order_relaxed);
    return MAX_MEM_VALUE_SIZE;
  }
  return size;
}

// SafeCopy wrapper: copies up to `size` bytes from `addr` into `dst`.
// Returns silently on failure, counting short reads. The destination
// slot was pre-zeroed by reset_instr(), so a short copy naturally
// leaves the unread tail zero-filled.
static inline void safe_copy_value(void *dst, const void *addr, UINT32 size)
{
  size_t got = PIN_SafeCopy(dst, addr, size);
  if (got < size)
    g_safecopy_short_reads.fetch_add(1, std::memory_order_relaxed);
}

VOID RecordMemRead(THREADID tid, ADDRINT addr, UINT32 size, UINT32 want_value)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase != Phase::TRACING)
    return;

  for (int i = 0; i < NUM_INSTR_SOURCES; i++) {
    if (ts->curr_instr.source_memory[i] == 0) {
      ts->curr_instr.source_memory[i]      = (uint64_t)addr;
      ts->curr_instr.source_memory_size[i] =
        (uint8_t)std::min<UINT32>(size, 255);

      if (want_value) {
        UINT32 csz = clamp_value_size(size);
        safe_copy_value(&ts->curr_instr.source_memory_value[i][0],
                        (const void *)addr,
                        csz);
      }
      return;
    }
  }

  // Overflow: more than NUM_INSTR_SOURCES read operands executed.
  g_overflowed_src_memops.fetch_add(1, std::memory_order_relaxed);
}

// Pre-call for stores: assign an address/size slot and, if the carrying
// instruction has a fall-through, remember (op_idx -> slot) so the
// matching RecordMemWriteValue post-call can drop the value in after
// the store retires. `will_capture_value` is a compile-time decision
// made at instrumentation, so the branch is well-predicted.
VOID RecordMemWrite(THREADID tid,
                    ADDRINT  addr,
                    UINT32   size,
                    UINT32   op_idx,
                    UINT32   will_capture_value)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase != Phase::TRACING)
    return;

  for (int k = 0; k < NUM_INSTR_DESTINATIONS; k++) {
    if (ts->curr_instr.destination_memory[k] == 0) {
      ts->curr_instr.destination_memory[k]      = (uint64_t)addr;
      ts->curr_instr.destination_memory_size[k] =
        (uint8_t)std::min<UINT32>(size, 255);

      if (will_capture_value && op_idx < ThreadState::MAX_WRITE_OPS) {
        ts->pending_store_slot[op_idx] = k;
        ts->pending_store_addr[op_idx] = addr;
        ts->pending_store_size[op_idx] = size;
      }
      return;
    }
  }

  g_overflowed_dst_memops.fetch_add(1, std::memory_order_relaxed);
}

// Pre-call for VSIB gather/scatter instructions (anything where
// INS_HasScatteredMemoryAccess(ins) is true). PIN forbids calling
// INS_MemoryOperandSize() on those operands at instrumentation time
// because each lane carries its own (address, size, mask) tuple --
// trying anyway aborts the tool with
// "Instruction memory operand does not have a size". Instead we ask PIN
// for the per-lane access info via IARG_MULTI_MEMORYACCESS_EA and dispatch
// each masked-on lane into the normal src/dst slot machinery. Load lanes
// reuse RecordMemRead so value capture and overflow accounting are shared
// with non-scattered loads; store lanes inline a slot fill plus a value
// miss bump (IPOINT_AFTER is not viable per-lane for scattered stores, so
// we punt on the value, matching the no-fall-through non-call store path).
VOID RecordScatteredMemAccess(THREADID                   tid,
                              PIN_MULTI_MEM_ACCESS_INFO *info,
                              UINT32                     want_load_value)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase != Phase::TRACING)
    return;
  if (!info)
    return;

  g_scattered_instrs_seen.fetch_add(1, std::memory_order_relaxed);

  for (UINT32 lane_idx = 0; lane_idx < info->numberOfMemops; lane_idx++) {
    const PIN_MEM_ACCESS_INFO &lane = info->memop[lane_idx];
    if (!lane.maskOn)
      continue;

    if (lane.memopType == PIN_MEMOP_LOAD) {
      RecordMemRead(tid,
                    lane.memoryAddress,
                    lane.bytesAccessed,
                    want_load_value);
      continue;
    }

    // STORE lane: fill a dst slot, leave value zero, mark as missed.
    bool slotted = false;
    for (int k = 0; k < NUM_INSTR_DESTINATIONS; k++) {
      if (ts->curr_instr.destination_memory[k] == 0) {
        ts->curr_instr.destination_memory[k]      =
            (uint64_t)lane.memoryAddress;
        ts->curr_instr.destination_memory_size[k] =
            (uint8_t)std::min<UINT32>(lane.bytesAccessed, 255);
        slotted = true;
        break;
      }
    }
    if (!slotted)
      g_overflowed_dst_memops.fetch_add(1, std::memory_order_relaxed);

    g_missed_store_values.fetch_add(1, std::memory_order_relaxed);
    g_scatter_missed_store_values.fetch_add(1, std::memory_order_relaxed);
  }
}

// Post-call fired at IPOINT_AFTER: the store has landed, so we can
// PIN_SafeCopy the written bytes out of the effective address.
VOID RecordMemWriteValue(THREADID tid, UINT32 op_idx)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase != Phase::TRACING)
    return;
  if (op_idx >= ThreadState::MAX_WRITE_OPS)
    return;

  int k = ts->pending_store_slot[op_idx];
  if (k < 0)
    return;  // pre-call overflowed into unassigned state

  UINT32  size = clamp_value_size(ts->pending_store_size[op_idx]);
  ADDRINT addr = ts->pending_store_addr[op_idx];

  safe_copy_value(&ts->curr_instr.destination_memory_value[k][0],
                  (const void *)addr,
                  size);

  ts->pending_store_slot[op_idx] = -1;
}

// Predicated pre-call: fires once per executed store on instructions
// whose IPOINT_AFTER is unavailable, so we can record the store value
// miss exactly on the dynamic executions we would have captured.
VOID NoteMissedStoreValue(THREADID /* tid */)
{
  g_missed_store_values.fetch_add(1, std::memory_order_relaxed);
}

// Pre-call specifically for CALL instructions: synthesises the 8-byte
// return-address store value that PIN cannot capture via IPOINT_AFTER
// (call has no static fall-through). Runs AFTER RecordMemWrite at the
// same IPOINT_BEFORE so pending_store_slot[op_idx] is already populated.
// ret_addr is baked in at instrumentation time as INS_NextAddress(ins).
VOID RecordCallRetAddrValue(THREADID tid, UINT32 op_idx, ADDRINT ret_addr)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase != Phase::TRACING)
    return;
  if (op_idx >= ThreadState::MAX_WRITE_OPS)
    return;

  int k = ts->pending_store_slot[op_idx];
  if (k < 0)
    return;  // RecordMemWrite overflowed past the 2-dst slot budget

  uint64_t v = (uint64_t)ret_addr;
  std::memcpy(&ts->curr_instr.destination_memory_value[k][0], &v, sizeof(v));

  ts->pending_store_slot[op_idx] = -1;
  g_call_store_values_filled.fetch_add(1, std::memory_order_relaxed);
}

VOID RecordRegRead(THREADID tid, UINT32 reg)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase != Phase::TRACING)
    return;

  for (int i = 0; i < NUM_INSTR_SOURCES; i++) {
    if (ts->curr_instr.source_registers[i] == 0) {
      ts->curr_instr.source_registers[i] = static_cast<unsigned char>(reg);
      return;
    }
  }
}

VOID RecordRegWrite(THREADID tid, UINT32 reg)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase != Phase::TRACING)
    return;

  for (int i = 0; i < NUM_INSTR_DESTINATIONS; i++) {
    if (ts->curr_instr.destination_registers[i] == 0) {
      ts->curr_instr.destination_registers[i] = static_cast<unsigned char>(reg);
      return;
    }
  }
}

// Final commit: stamp IP / branch info / instr_type, write the record,
// advance the phase counter. Scheduled at IPOINT_BEFORE so it fires
// AFTER the per-operand pre-calls but BEFORE any IPOINT_AFTER post-
// calls. Therefore: IPOINT_AFTER store-value post-calls fire AFTER
// RecordInstr writes the record out to the compressor.
//
// This means: by the time a store-value post-call runs, the record
// containing its slot has ALREADY been serialised. We must instead
// capture the store value into ts->curr_instr BEFORE RecordInstr, not
// after. There's no IPOINT that fires "after store, before next
// instruction"; the clean fix is to have RecordInstr NOT emit the
// record for instructions with pending stores, and have the last
// matching post-call do the emit. Simpler alternative implemented
// below: push RecordInstr to IPOINT_AFTER on fall-through instructions
// that carry at least one store, and keep it at IPOINT_BEFORE for
// everything else. See InstrumentTrace() for the selection logic.
VOID RecordInstrCommit(THREADID  tid,
                       ADDRINT   ip,
                       UINT8     is_branch,
                       UINT8     branch_taken,
                       UINT8     instr_type)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase != Phase::TRACING)
    return;

  ts->curr_instr.ip           = (uint64_t)ip;
  ts->curr_instr.is_branch    = is_branch;
  ts->curr_instr.branch_taken = branch_taken;
  ts->curr_instr.privilege    = 0;            // PIN is user-mode
  ts->curr_instr.instr_type   = instr_type;
  // destination_memory_pa / source_memory_pa / reserved already zero.

  ts->compress_write(&ts->curr_instr, sizeof(trace_instr_v2_t));

  ts->counter--;
  ts->reset_instr();
  ts->reset_pending_stores();

  if (ts->counter == 0)
    leave_tracing(ts);
}

/* =========================================================================
 * Per-instruction instrumentation injection
 *
 * Ordering requirements:
 *   1. Register reads/writes, memory-read pre-calls (with load value),
 *      and memory-write pre-calls (addr+size only) fire at IPOINT_BEFORE.
 *   2. Store-value post-calls fire at IPOINT_AFTER (value is now live).
 *   3. RecordInstrCommit serialises the record. It must run AFTER all
 *      per-operand calls have populated curr_instr.
 *
 * Decision tree for RecordInstrCommit placement:
 *   - Instruction has at least one store AND has fall-through AND
 *     values are enabled: commit at IPOINT_AFTER (after store-value
 *     post-calls). This guarantees destination values land in the
 *     record before it is serialised.
 *   - Otherwise: commit at IPOINT_BEFORE (after the pre-calls, whose
 *     insertion order at the same IPOINT is preserved by PIN).
 * ========================================================================= */

static void insert_full_analysis(INS ins, bool capture_values)
{
  uint8_t itype = classify_instr(ins);

  for (UINT32 i = 0; i < INS_MaxNumRRegs(ins); i++) {
    REG reg = INS_RegR(ins, i);
    if (REG_valid(reg) && !REG_is_flags(reg) && !REG_is_seg(reg)) {
      INS_InsertCall(ins,
                     IPOINT_BEFORE,
                     (AFUNPTR)RecordRegRead,
                     IARG_THREAD_ID,
                     IARG_UINT32,
                     REG_FullRegName(reg),
                     IARG_END);
    }
  }

  for (UINT32 i = 0; i < INS_MaxNumWRegs(ins); i++) {
    REG reg = INS_RegW(ins, i);
    if (REG_valid(reg) && !REG_is_flags(reg) && !REG_is_seg(reg)) {
      INS_InsertCall(ins,
                     IPOINT_BEFORE,
                     (AFUNPTR)RecordRegWrite,
                     IARG_THREAD_ID,
                     IARG_UINT32,
                     REG_FullRegName(reg),
                     IARG_END);
    }
  }

  UINT32 mem_ops          = INS_MemoryOperandCount(ins);
  bool   has_scattered    = INS_HasScatteredMemoryAccess(ins);
  bool   has_store        = false;
  bool   has_fallthrough  = INS_HasFallThrough(ins);
  bool   is_call_no_ft    = INS_IsCall(ins) && !has_fallthrough;
  // Scattered (VSIB gather/scatter) ops are funnelled through one
  // RecordScatteredMemAccess call; the per-operand store-value plumbing
  // below (IPOINT_AFTER capture, CALL ret-addr synthesis, missed-value
  // counting) is bypassed for them.
  bool   post_calls_ok    = capture_values && has_fallthrough && !has_scattered;
  // Stores whose value we can fill in: either via IPOINT_AFTER post-call
  // (fall-through instructions) or via synthesis at pre-call time for
  // calls (return address is known statically). Both paths populate
  // pending_store_slot[op_idx] via RecordMemWrite, so the 'will_capture'
  // flag is set for both.
  bool   fill_store_values = capture_values
                             && (has_fallthrough || is_call_no_ft)
                             && !has_scattered;

  if (has_scattered) {
    // VSIB gather/scatter: per-lane addrs/sizes via IARG_MULTI_MEMORYACCESS_EA.
    // Do NOT call INS_MemoryOperandSize() for any operand of this instr --
    // PIN errors out the moment it's called on a scattered operand
    // ("Instruction memory operand does not have a size").
    INS_InsertPredicatedCall(ins,
                             IPOINT_BEFORE,
                             (AFUNPTR)RecordScatteredMemAccess,
                             IARG_THREAD_ID,
                             IARG_MULTI_MEMORYACCESS_EA,
                             IARG_UINT32,
                             (UINT32)(capture_values ? 1 : 0),
                             IARG_END);
  } else {
    // Pre-calls for every executed memory operand.
    for (UINT32 i = 0; i < mem_ops; i++) {
      UINT32 op_size = INS_MemoryOperandSize(ins, i);

      if (INS_MemoryOperandIsRead(ins, i)) {
        INS_InsertPredicatedCall(ins,
                                 IPOINT_BEFORE,
                                 (AFUNPTR)RecordMemRead,
                                 IARG_THREAD_ID,
                                 IARG_MEMORYOP_EA,
                                 i,
                                 IARG_UINT32,
                                 op_size,
                                 IARG_UINT32,
                                 (UINT32)(capture_values ? 1 : 0),
                                 IARG_END);
      }
      if (INS_MemoryOperandIsWritten(ins, i)) {
        has_store = true;
        INS_InsertPredicatedCall(ins,
                                 IPOINT_BEFORE,
                                 (AFUNPTR)RecordMemWrite,
                                 IARG_THREAD_ID,
                                 IARG_MEMORYOP_EA,
                                 i,
                                 IARG_UINT32,
                                 op_size,
                                 IARG_UINT32,
                                 i,
                                 IARG_UINT32,
                                 (UINT32)(fill_store_values ? 1 : 0),
                                 IARG_END);
      }
    }
  }

  // CALL-synthesised store values: must come AFTER RecordMemWrite in
  // insertion order at IPOINT_BEFORE so pending_store_slot is live.
  // Return address = INS_NextAddress(ins), constant per static call.
  if (is_call_no_ft && capture_values && mem_ops > 0 && !has_scattered) {
    ADDRINT ret_addr = INS_NextAddress(ins);
    for (UINT32 i = 0; i < mem_ops; i++) {
      if (INS_MemoryOperandIsWritten(ins, i)) {
        INS_InsertPredicatedCall(ins,
                                 IPOINT_BEFORE,
                                 (AFUNPTR)RecordCallRetAddrValue,
                                 IARG_THREAD_ID,
                                 IARG_UINT32,
                                 i,
                                 IARG_ADDRINT,
                                 ret_addr,
                                 IARG_END);
      }
    }
  }

  // If there are stores but we cannot emit IPOINT_AFTER post-calls
  // AND we also can't synthesise them (non-call, no fall-through),
  // record each executed store as a missed value. Predicated pre-call
  // so only actually-executed ones are counted.
  if (has_store && capture_values && !has_fallthrough && !is_call_no_ft
      && !has_scattered) {
    for (UINT32 i = 0; i < mem_ops; i++) {
      if (INS_MemoryOperandIsWritten(ins, i)) {
        INS_InsertPredicatedCall(ins,
                                 IPOINT_BEFORE,
                                 (AFUNPTR)NoteMissedStoreValue,
                                 IARG_THREAD_ID,
                                 IARG_END);
      }
    }
  }

  // Post-calls for store values (only when fall-through and values on).
  if (post_calls_ok && has_store) {
    for (UINT32 i = 0; i < mem_ops; i++) {
      if (INS_MemoryOperandIsWritten(ins, i)) {
        INS_InsertPredicatedCall(ins,
                                 IPOINT_AFTER,
                                 (AFUNPTR)RecordMemWriteValue,
                                 IARG_THREAD_ID,
                                 IARG_UINT32,
                                 i,
                                 IARG_END);
      }
    }
  }

  // Commit: AFTER if we inserted post-calls (so store values are in
  // place before serialisation); BEFORE otherwise.
  IPOINT commit_point = (post_calls_ok && has_store) ? IPOINT_AFTER
                                                     : IPOINT_BEFORE;

  UINT8 is_branch = INS_IsBranch(ins) ? 1 : 0;

  if (is_branch) {
    INS_InsertCall(ins,
                   commit_point,
                   (AFUNPTR)RecordInstrCommit,
                   IARG_THREAD_ID,
                   IARG_INST_PTR,
                   IARG_UINT32,
                   (UINT32)1,
                   IARG_BRANCH_TAKEN,
                   IARG_UINT32,
                   (UINT32)itype,
                   IARG_END);
  } else {
    INS_InsertCall(ins,
                   commit_point,
                   (AFUNPTR)RecordInstrCommit,
                   IARG_THREAD_ID,
                   IARG_INST_PTR,
                   IARG_UINT32,
                   (UINT32)0,
                   IARG_UINT32,
                   (UINT32)0,
                   IARG_UINT32,
                   (UINT32)itype,
                   IARG_END);
  }
}

/* =========================================================================
 * TRACE-granularity instrumentation
 * ========================================================================= */

VOID InstrumentTrace(TRACE trace, VOID * /* unused */)
{
  bool use_markers = KnobUseMarkers.Value();
  bool roi_done    = roi_ended.load(std::memory_order_acquire);
  bool roi_active  = roi_started.load(std::memory_order_acquire);
  int  tracing     = active_tracing_threads.load(std::memory_order_acquire);
  bool values_on   = KnobCaptureValues.Value();

  if (roi_done)
    return;

  if (use_markers && !roi_active) {
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
      for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
        if (is_roi_marker(ins)) {
          INS_InsertCall(ins,
                         IPOINT_BEFORE,
                         (AFUNPTR)HandleMarker,
                         IARG_THREAD_ID,
                         IARG_REG_VALUE,
                         LEVEL_BASE::REG_RCX,
                         IARG_END);
        }
      }
    }
    return;
  }

  TRACE_InsertCall(trace,
                   IPOINT_BEFORE,
                   (AFUNPTR)CheckROITransition,
                   IARG_THREAD_ID,
                   IARG_END);

  if (tracing > 0) {
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
      for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
        if (use_markers && is_roi_marker(ins)) {
          INS_InsertCall(ins,
                         IPOINT_BEFORE,
                         (AFUNPTR)HandleMarker,
                         IARG_THREAD_ID,
                         IARG_REG_VALUE,
                         LEVEL_BASE::REG_RCX,
                         IARG_END);
        }
        insert_full_analysis(ins, values_on);
      }
    }
  } else {
    TRACE_InsertCall(trace,
                     IPOINT_BEFORE,
                     (AFUNPTR)FastForwardInitial,
                     IARG_THREAD_ID,
                     IARG_UINT32,
                     TRACE_NumIns(trace),
                     IARG_END);
    TRACE_InsertCall(trace,
                     IPOINT_BEFORE,
                     (AFUNPTR)FastForwardInter,
                     IARG_THREAD_ID,
                     IARG_UINT32,
                     TRACE_NumIns(trace),
                     IARG_END);

    if (use_markers) {
      for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl);
           bbl     = BBL_Next(bbl)) {
        for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
          if (is_roi_marker(ins)) {
            INS_InsertCall(ins,
                           IPOINT_BEFORE,
                           (AFUNPTR)HandleMarker,
                           IARG_THREAD_ID,
                           IARG_REG_VALUE,
                           LEVEL_BASE::REG_RCX,
                           IARG_END);
          }
        }
      }
    }
  }
}

/* =========================================================================
 * Thread lifecycle
 * ========================================================================= */

VOID ThreadStart(THREADID tid,
                 CONTEXT * /* unused */,
                 INT32 /* flags */,
                 VOID * /* unused */)
{
  if (main_thread_id == INVALID_THREADID)
    main_thread_id = tid;

  if (KnobMainThreadOnly.Value() && tid != main_thread_id)
    return;

  OS_THREAD_ID os_tid      = PIN_GetTid();
  bool         use_markers = KnobUseMarkers.Value();
  UINT64       inter_skip  = KnobInterSampleSkip.Value();
  UINT64       trace_count = KnobTraceInstructions.Value();
  UINT64       num_samples = KnobNumSamples.Value();
  int          level       = KnobZstdLevel.Value();
  bool         values_on   = KnobCaptureValues.Value();

  bool   roi_already_started = roi_started.load(std::memory_order_acquire);
  Phase  starting_phase;
  UINT64 starting_counter;

  if (use_markers) {
    if (roi_already_started) {
      starting_phase   = Phase::TRACING;
      starting_counter = trace_count;
    } else {
      starting_phase   = Phase::WAITING_FOR_ROI;
      starting_counter = 0;
    }
  } else {
    UINT64 initial_skip = KnobInitialSkip.Value();
    if (initial_skip > 0) {
      starting_phase   = Phase::INITIAL_SKIP;
      starting_counter = initial_skip;
    } else {
      starting_phase   = Phase::TRACING;
      starting_counter = trace_count;
    }
  }

  ThreadState *ts = new ThreadState(os_tid,
                                    KnobOutputBase.Value(),
                                    starting_phase,
                                    starting_counter,
                                    inter_skip,
                                    trace_count,
                                    num_samples,
                                    false,
                                    level,
                                    values_on);

  PIN_RWMutexWriteLock(&registry_lock);
  thread_states[tid] = ts;
  PIN_RWMutexUnlock(&registry_lock);

  if (ts->phase == Phase::TRACING) {
    active_tracing_threads.fetch_add(1, std::memory_order_acq_rel);
    mark_started_tracing(ts);
    PIN_RemoveInstrumentation();
  }

  {
    LogGuard _lg;
    std::cerr << "[tracer_roi_v2] Thread start:"
              << " PIN tid=" << tid << " OS tid=" << os_tid
              << " starting_phase=" << phase_name(ts->phase)
              << " inter_skip=" << inter_skip << " trace=" << trace_count
              << " max_samples=" << num_samples << " zstd_level=" << level
              << " values=" << (values_on ? 1 : 0) << std::endl;
  }
}

VOID ThreadFini(THREADID tid,
                const CONTEXT * /* unused */,
                INT32 /* code */,
                VOID * /* unused */)
{
  PIN_RWMutexWriteLock(&registry_lock);
  ThreadState *ts    = thread_states[tid];
  thread_states[tid] = nullptr;

  if (ts && ts->phase == Phase::TRACING) {
    active_tracing_threads.fetch_sub(1, std::memory_order_acq_rel);
    {
      LogGuard _lg;
      std::cerr << "[tracer_roi_v2] Thread " << ts->os_tid
                << " exited mid-trace (sample " << ts->samples_collected
                << "). Partial trace kept." << std::endl;
    }
  }

  PIN_RWMutexUnlock(&registry_lock);

  if (!ts)
    return;

  ts->force_close();
  {
    LogGuard _lg;
    std::cerr << "[tracer_roi_v2] Thread fini: OS tid=" << ts->os_tid
              << (ts->is_master ? " (master)" : "")
              << " final_phase=" << phase_name(ts->phase)
              << " samples_completed=" << ts->samples_collected << std::endl;
  }

  delete ts;
}

/* =========================================================================
 * Fini
 * ========================================================================= */

VOID Fini(INT32 /* code */, VOID * /* unused */)
{
  for (UINT32 i = 0; i < (UINT32)PIN_MAX_THREADS; i++) {
    if (thread_states[i]) {
      thread_states[i]->force_close();
      delete thread_states[i];
      thread_states[i] = nullptr;
    }
  }

  LogGuard _lg;
  std::cerr << "[tracer_roi_v2] All threads finished.\n"
            << "  overflowed src memops (>4 reads/instr)  : "
            << g_overflowed_src_memops.load() << "\n"
            << "  overflowed dst memops (>2 writes/instr) : "
            << g_overflowed_dst_memops.load() << "\n"
            << "  missed store values (no fall-through)   : "
            << g_missed_store_values.load() << "\n"
            << "  call-store values filled (synthesised)  : "
            << g_call_store_values_filled.load() << "\n"
            << "  values truncated to " << MAX_MEM_VALUE_SIZE << "B            : "
            << g_truncated_values.load() << "\n"
            << "  PIN_SafeCopy short reads                : "
            << g_safecopy_short_reads.load() << "\n"
            << "  scattered (gather/scatter) instrs seen  : "
            << g_scattered_instrs_seen.load() << "\n"
            << "  scatter store-value lanes skipped       : "
            << g_scatter_missed_store_values.load() << "\n"
            << "  threads started tracing                 : "
            << g_threads_started_tracing.load() << "\n"
            << "  threads reached DONE                    : "
            << g_threads_reached_done.load() << "\n"
            << "  exit_on_done triggered                  : "
            << (g_exit_triggered.load() ? 1 : 0) << std::endl;
}

/* =========================================================================
 * Usage
 * ========================================================================= */

INT32 Usage()
{
  std::cerr
    << "champsim_tracer_mt_roi_v2:\n"
    << "  ROI-aware, multi-threaded, sampled ChampSim tracer emitting\n"
    << "  the EXTENDED 512-byte input_instr_v2 record with memory\n"
    << "  values, access sizes, and instruction-type tags.\n\n"
    << "Output files:\n"
    << "  <base>_t<os_tid>_master_s<sid>.champsim2.zst  (discard)\n"
    << "  <base>_t<os_tid>_s<sid>.champsim2.zst         (use)\n\n"
    << "Decompress: zstd -d <file>  or  zstd -d -c <file> | <reader>\n\n"
    << "PA and privilege fields are zero-filled under PIN (user mode).\n\n"
    << KNOB_BASE::StringKnobSummary() << std::endl;
  return EXIT_FAILURE;
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(int argc, char *argv[])
{
  if (PIN_Init(argc, argv))
    return Usage();

  std::fill(std::begin(thread_states), std::end(thread_states), nullptr);
  PIN_RWMutexInit(&registry_lock);
  PIN_MutexInit(&cerr_lock);

  TRACE_AddInstrumentFunction(InstrumentTrace, nullptr);
  PIN_AddThreadStartFunction(ThreadStart, nullptr);
  PIN_AddThreadFiniFunction(ThreadFini, nullptr);
  PIN_AddFiniFunction(Fini, nullptr);

  {
    LogGuard _lg;
    std::cerr << "[tracer_roi_v2] Starting.\n"
              << "  record format : input_instr_v2 (" << sizeof(trace_instr_v2_t)
              << " bytes)\n"
              << "  output base   : " << KnobOutputBase.Value() << "\n"
              << "  use_markers   : " << KnobUseMarkers.Value() << "\n"
              << "  initial skip  : " << KnobInitialSkip.Value()
              << (KnobUseMarkers.Value() ? " (ignored)" : "") << "\n"
              << "  inter skip    : " << KnobInterSampleSkip.Value() << "\n"
              << "  trace/sample  : " << KnobTraceInstructions.Value() << "\n"
              << "  max samples   : " << KnobNumSamples.Value() << "\n"
              << "  zstd level    : " << KnobZstdLevel.Value() << "\n"
              << "  capture values: " << KnobCaptureValues.Value() << "\n"
              << "  out buf size  : " << OUT_BUF_SIZE / 1024 << " KB\n"
              << "  main_only     : " << KnobMainThreadOnly.Value() << "\n"
              << "  exit_on_done  : " << KnobExitOnDone.Value() << "\n"
              << std::endl;
  }

  PIN_StartProgram();
  return EXIT_SUCCESS;
}
