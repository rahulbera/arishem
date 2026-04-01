/*
 * champsim_tracer_mt_roi.cpp
 *
 * ROI-marker-aware, optimized, multi-threaded, periodically-sampled
 * PIN-based trace generator for ChampSim, with online Zstandard (zstd)
 * compression.
 *
 * This file extends the uncompressed version with per-thread streaming
 * zstd compression. Each thread owns an independent compression context
 * (ZSTD_CCtx) and a fixed-size output buffer. Compressed bytes accumulate
 * in the output buffer and are flushed to disk only when the buffer is full
 * or when the sample file is closed. This amortizes fwrite(2) syscall
 * overhead across many records, keeping compression cost off the critical
 * path.
 *
 * COMPRESSION DESIGN CHOICES
 * --------------------------
 * Speed is the first-order concern. Every decision below prioritizes PIN
 * throughput over compression ratio:
 *
 *   Compressor  : Zstandard (libzstd)
 *     Chosen over zlib (slower encode) and lz4 (lower ratio). zstd level 1
 *     achieves ~500-800 MB/s encode throughput — well above PIN's output
 *     rate. Typical ratio on ChampSim traces: 5-8x at level 1.
 *
 *   Level       : 1 by default (tunable via -zstd_level knob).
 *     Level 1 is zstd's fastest setting. Level 3 (~300 MB/s) is also
 *     acceptable if you observe headroom. Do not exceed level 3 during
 *     tracing — the speed/ratio curve flattens and PIN throughput suffers.
 *
 *   Context     : One ZSTD_CCtx per thread per sample file.
 *     Created fresh in open_next_sample(), freed in close_sample().
 *     No sharing between threads — eliminates all compression-side locking.
 *
 *   Output buffer: 128 KB per thread (OUT_BUF_SIZE), allocated once at
 *     ThreadStart and reused across all sample files for that thread.
 *     fwrite is called only when the buffer fills up or at file close.
 *     This batches syscalls across ~1000-2000 trace records per write.
 *
 *   Input       : Fed 64 bytes at a time (one trace_instr_format record).
 *     zstd maintains its own internal input window. No input-side batching
 *     needed. ZSTD_e_continue used throughout to avoid forcing frame
 *     boundaries, maximizing compression speed and ratio.
 *
 *   File I/O    : FILE* + fwrite (not std::ofstream).
 *     Lower per-call overhead. Output buffer managed manually for precise
 *     control over when flushes occur.
 *
 * OUTPUT FILES
 * ------------
 *   <base>_t<os_tid>_master_s<sid>.champsim.zst  (master thread — discard)
 *   <base>_t<os_tid>_s<sid>.champsim.zst          (worker threads — keep)
 *
 * Decompress:  zstd -d <file>   or   zstd -d -c <file> | <reader>
 *
 * TWO OPERATING MODES
 * -------------------
 *   -use_markers 0  (default): skip-based mode. Use -i for initial skip.
 *   -use_markers 1:            ROI marker mode. -i is ignored.
 *
 * ROI MARKER MECHANISM
 * --------------------
 * Magic instruction: xchg %rcx, %rcx  (architecturally a NOP)
 * RCX value encodes marker type:
 *   CHAMPSIM_ROI_BEGIN (1) -> begin tracing
 *   CHAMPSIM_ROI_END   (2) -> end tracing
 * See champsim_roi_markers.h for the application-side API.
 *
 * MASTER THREAD IDENTIFICATION
 * ----------------------------
 * The thread that executes champsim_roi_begin() is the master thread.
 * Its files carry a "_master" infix and can be identified and discarded
 * without inspecting file contents (they typically contain only
 * pthread_join spin-wait instructions).
 *
 * PER-THREAD STATE MACHINE
 * ------------------------
 *   Marker mode:
 *     WAITING_FOR_ROI -> TRACING -> INTER_SKIP -> TRACING -> ... -> DONE
 *   Skip-based mode:
 *     INITIAL_SKIP    -> TRACING -> INTER_SKIP -> TRACING -> ... -> DONE
 *
 * OPTIMIZATIONS
 * -------------
 * 1. TRACE-granularity instrumentation during skip/waiting phases.
 * 2. PIN_RemoveInstrumentation() on phase transitions flushes the JIT
 *    code cache so skip phases run near-natively.
 * 3. Online zstd compression as described above.
 *
 * BUILD
 * -----
 * Add to your make_tracer.sh:
 *   LDFLAGS += -lzstd
 * Requires libzstd >= 1.4.0 (for ZSTD_compressStream2).
 * Requires PIN 3.17+ on Linux x86-64.
 *
 * USAGE
 * -----
 *   pin -t obj-intel64/champsim_tracer_mt_roi.so  \
 *       -use_markers 1                             \
 *       -o traces/bfs                              \
 *       -s 500000000                               \
 *       -t 10000000                                \
 *       -n 5                                       \
 *       [-zstd_level 1]                            \
 *       -- ./bfs -g 20 -n 1
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
 * Trace record format — identical to legacy ChampSim single-threaded format.
 * Do NOT change field order or sizes.
 * ========================================================================= */

#define NUM_INSTR_DESTINATIONS 2
#define NUM_INSTR_SOURCES      4

struct trace_instr_format {
  unsigned long long int ip;
  unsigned char          is_branch;
  unsigned char          branch_taken;
  unsigned char          destination_registers[NUM_INSTR_DESTINATIONS];
  unsigned char          source_registers[NUM_INSTR_SOURCES];
  unsigned long long int destination_memory[NUM_INSTR_DESTINATIONS];
  unsigned long long int source_memory[NUM_INSTR_SOURCES];
};

/* =========================================================================
 * ROI marker constants — must match champsim_roi_markers.h
 * ========================================================================= */

#define CHAMPSIM_ROI_BEGIN ((ADDRINT)1)
#define CHAMPSIM_ROI_END   ((ADDRINT)2)

/* =========================================================================
 * Output buffer size
 *
 * 128 KB per thread. At ~6x compression and 64 bytes per record, this
 * flushes roughly every 1200 records, keeping fwrite syscall overhead
 * negligible. Must be >= ZSTD_CStreamOutSize() (typically 128 KB + header).
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
  "Files: <base>_t<os_tid>[_master]_s<sid>.champsim.zst");

KNOB<BOOL> KnobUseMarkers(
  KNOB_MODE_WRITEONCE,
  "pintool",
  "use_markers",
  "0",
  "If 1, use champsim_roi_begin/end markers to gate tracing "
  "(see champsim_roi_markers.h). The -i knob is ignored in this mode. "
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

/* =========================================================================
 * Per-thread phase state machine
 * ========================================================================= */

enum class Phase {
  WAITING_FOR_ROI,  // marker mode: waiting for champsim_roi_begin()
  INITIAL_SKIP,     // skip-based mode: skipping past initialization
  TRACING,          // recording instructions to a compressed sample file
  INTER_SKIP,       // skipping between sample windows
  DONE              // no more tracing (quota, end marker, or thread exit)
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
 * Per-thread state
 * ========================================================================= */

struct ThreadState {
  // --- State machine ---
  Phase  phase;
  UINT64 counter;  // counts DOWN to 0, triggers phase transition

  // --- Sample tracking ---
  UINT64 samples_collected;
  UINT64 sample_limit;  // 0 = unlimited

  // --- Compressed output ---
  // fp, zstd_ctx are opened/created per sample in open_next_sample()
  // and freed in close_sample(). out_buf is allocated once at construction
  // and reused across all sample files for this thread.
  FILE      *fp;
  ZSTD_CCtx *zstd_ctx;
  uint8_t   *out_buf;      // OUT_BUF_SIZE bytes, owned by this struct
  size_t     out_buf_pos;  // bytes currently valid in out_buf

  // --- Identification ---
  std::string  base_name;
  OS_THREAD_ID os_tid;
  bool         is_master;   // set true by HandleMarker on roi_begin
  int          zstd_level;  // cached from knob

  // --- Instruction record being built ---
  trace_instr_format curr_instr;

  // --- Cached knob values ---
  UINT64 inter_sample_skip;
  UINT64 trace_per_sample;

  ThreadState() = delete;

  explicit ThreadState(OS_THREAD_ID       tid,
                       const std::string &base,
                       Phase              starting_phase,
                       UINT64             initial_counter,
                       UINT64             inter_skip,
                       UINT64             trace_count,
                       UINT64             num_samples,
                       bool               master,
                       int                level)
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
        inter_sample_skip(inter_skip),
        trace_per_sample(trace_count)
  {
    reset_instr();
    if (phase == Phase::TRACING)
      open_next_sample();
  }

  ~ThreadState()
  {
    delete[] out_buf;
  }

  // Build filename for the current sample.
  std::string sample_filename() const
  {
    std::ostringstream ss;
    ss << base_name << "_t" << os_tid;
    if (is_master)
      ss << "_master";
    ss << "_s" << samples_collected << ".champsim.zst";
    return ss.str();
  }

  // Open a new compressed sample file. Called at the start of each
  // TRACING window. Creates a fresh ZSTD_CCtx so each sample file
  // is independently decompressible.
  void open_next_sample()
  {
    std::string fname = sample_filename();

    fp = std::fopen(fname.c_str(), "wb");
    if (!fp) {
      {
        LogGuard _lg;
        std::cerr << "[tracer_roi] ERROR: cannot open: " << fname << std::endl;
      }
      return;
    }

    zstd_ctx = ZSTD_createCCtx();
    if (!zstd_ctx) {
      {
        LogGuard _lg;
        std::cerr << "[tracer_roi] ERROR: ZSTD_createCCtx failed: " << fname
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
      {
        LogGuard _lg;
        std::cerr << "[tracer_roi] WARNING: cannot set zstd level "
                  << zstd_level << ": " << ZSTD_getErrorName(rc) << std::endl;
      }
    }

    out_buf_pos = 0;

    {
      LogGuard _lg;
      std::cerr << "[tracer_roi] Thread " << os_tid
                << (is_master ? " (master)" : "") << " sample "
                << samples_collected << " -> " << fname << " (zstd level "
                << zstd_level << ")" << std::endl;
    }
  }

  // Finalize the zstd stream and close the file.
  // Uses ZSTD_e_end to flush internal state and write the end frame.
  void close_sample()
  {
    if (!fp || !zstd_ctx)
      return;

    // Drive ZSTD_e_end until the compressor reports zero bytes remaining.
    // Multiple iterations may be needed if the output buffer fills during
    // finalization (rare but possible for large compressor window states).
    ZSTD_inBuffer empty_in = {nullptr, 0, 0};
    size_t        remaining;
    do {
      ZSTD_outBuffer out = {out_buf, OUT_BUF_SIZE, out_buf_pos};
      remaining = ZSTD_compressStream2(zstd_ctx, &out, &empty_in, ZSTD_e_end);

      if (ZSTD_isError(remaining)) {
        {
          LogGuard _lg;
          std::cerr << "[tracer_roi] ERROR: zstd finalization: "
                    << ZSTD_getErrorName(remaining) << std::endl;
        }
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

  // Hot-path write: feed `data` into the zstd streaming compressor.
  // Compressed output accumulates in out_buf. fwrite is called only
  // when out_buf is full (OUT_BUF_SIZE bytes), keeping syscall
  // frequency proportional to compressed output rate, not record count.
  //
  // ZSTD_e_continue: do not force a flush. This lets zstd defer output
  // until its internal window is filled, maximizing throughput and ratio.
  void compress_write(const void *data, size_t size)
  {
    if (!fp || !zstd_ctx)
      return;

    ZSTD_inBuffer in = {data, size, 0};

    while (in.pos < in.size) {
      ZSTD_outBuffer out = {out_buf, OUT_BUF_SIZE, out_buf_pos};

      size_t rc = ZSTD_compressStream2(zstd_ctx, &out, &in, ZSTD_e_continue);

      if (ZSTD_isError(rc)) {
        {
          LogGuard _lg;
          std::cerr << "[tracer_roi] ERROR: ZSTD_compressStream2: "
                    << ZSTD_getErrorName(rc) << std::endl;
        }
        return;
      }

      out_buf_pos = out.pos;

      // Flush only when the buffer is full.
      if (out_buf_pos == OUT_BUF_SIZE) {
        std::fwrite(out_buf, 1, OUT_BUF_SIZE, fp);
        out_buf_pos = 0;
      }
    }
  }

  // Close the current sample, advance counters, determine next phase.
  // Returns true if we immediately re-entered TRACING (zero inter-skip).
  bool finish_sample()
  {
    close_sample();
    samples_collected++;

    {
      LogGuard _lg;
      std::cerr << "[tracer_roi] Thread " << os_tid
                << (is_master ? " (master)" : "") << " sample "
                << (samples_collected - 1) << " complete." << std::endl;
    }

    if (sample_limit > 0 && samples_collected >= sample_limit) {
      phase = Phase::DONE;
      {
        LogGuard _lg;
        std::cerr << "[tracer_roi] Thread " << os_tid << " quota reached. Done."
                  << std::endl;
      }
      return false;
    }

    if (inter_sample_skip > 0) {
      phase   = Phase::INTER_SKIP;
      counter = inter_sample_skip;
      return false;
    }

    // No inter-sample skip: open next sample immediately.
    phase   = Phase::TRACING;
    counter = trace_per_sample;
    open_next_sample();
    return true;
  }

  // Flush and close without incrementing sample counter.
  // Used for partial samples on thread/program exit.
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
};

/* =========================================================================
 * Global state
 * ========================================================================= */

static ThreadState *thread_states[PIN_MAX_THREADS];
static PIN_RWMUTEX  registry_lock;
static THREADID     main_thread_id = INVALID_THREADID;

// Number of threads currently in TRACING phase.
// 0: all threads skipping/waiting -> inject cheap TRACE-level counters.
// >0: at least one tracing -> inject full per-instruction analysis.
static std::atomic<int>  active_tracing_threads{0};
static std::atomic<bool> roi_started{false};
static std::atomic<bool> roi_ended{false};

// -------------------------------------------------------------------------
// Thread-safe logging
//
// PIN analysis callbacks for different threads fire concurrently. Without
// serialization, multi-line std::cerr chains from different threads
// interleave arbitrarily, producing garbled output. cerr_lock serializes
// all log output so each logical message prints atomically.
//
// Usage: wrap any std::cerr block in a LogGuard scope:
//   { LogGuard _lg; std::cerr << "..." << std::endl; }
// The destructor releases the lock when the scope exits, even on exceptions.
// -------------------------------------------------------------------------

static PIN_MUTEX cerr_lock;

struct LogGuard {
  __attribute__((always_inline)) LogGuard()
  {
    PIN_MutexLock(&cerr_lock);
  }

  __attribute__((always_inline)) ~LogGuard()
  {
    PIN_MutexUnlock(&cerr_lock);
  }

  // Non-copyable, non-movable
  LogGuard(const LogGuard &)            = delete;
  LogGuard &operator=(const LogGuard &) = delete;
};

static inline ThreadState *get_state(THREADID tid)
{
  return thread_states[tid];
}

/* =========================================================================
 * Marker instruction detection
 *
 * Returns true if `ins` is xchg %rcx, %rcx.
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
  return (r0 == REG_RCX && r1 == REG_RCX);
}

/* =========================================================================
 * Phase transition helpers
 *
 * Both call PIN_RemoveInstrumentation() to flush the JIT code cache.
 * This is safe from analysis callbacks — PIN defers the invalidation to
 * the next safe point. Per-thread phase checks in every analysis callback
 * act as a correctness backstop during the brief window before re-JIT.
 * ========================================================================= */

static void enter_tracing(ThreadState *ts)
{
  ts->open_next_sample();
  active_tracing_threads.fetch_add(1, std::memory_order_acq_rel);
  // Re-JIT: active_tracing_threads > 0 -> full per-instruction analysis.
  PIN_RemoveInstrumentation();
}

static bool leave_tracing(ThreadState *ts)
{
  bool re_entered = ts->finish_sample();
  if (!re_entered) {
    active_tracing_threads.fetch_sub(1, std::memory_order_acq_rel);
    // Re-JIT: if no threads remain tracing -> cheap skip counters.
    PIN_RemoveInstrumentation();
  }
  return re_entered;
}

/* =========================================================================
 * Analysis callbacks — marker handling
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
      std::cerr << "[tracer_roi] ROI begin detected on thread "
                << (ts ? ts->os_tid : (OS_THREAD_ID)-1) << std::endl;
    }

    if (!ts)
      return;

    // Mark this thread as the master (its files get _master infix).
    ts->is_master = true;

    if (ts->phase == Phase::WAITING_FOR_ROI ||
        ts->phase == Phase::INITIAL_SKIP) {
      ts->phase   = Phase::TRACING;
      ts->counter = ts->trace_per_sample;
      enter_tracing(ts);
      // enter_tracing calls PIN_RemoveInstrumentation().
      // Re-JIT: full analysis + CheckROITransition for other threads.
    }

  } else if (rcx_val == CHAMPSIM_ROI_END) {
    bool expected = false;
    if (!roi_ended.compare_exchange_strong(expected,
                                           true,
                                           std::memory_order_acq_rel))
      return;

    {
      LogGuard _lg;
      std::cerr << "[tracer_roi] ROI end detected on thread "
                << (ts ? ts->os_tid : (OS_THREAD_ID)-1) << std::endl;
    }

    if (!ts)
      return;

    if (ts->phase == Phase::TRACING) {
      active_tracing_threads.fetch_sub(1, std::memory_order_acq_rel);
      ts->force_close();
      {
        LogGuard _lg;
        std::cerr << "[tracer_roi] Thread " << ts->os_tid
                  << " stopped at ROI end (partial sample "
                  << ts->samples_collected << " kept)." << std::endl;
      }
    }
    ts->phase = Phase::DONE;

    // Re-JIT: roi_ended == true -> Case 1 in InstrumentTrace ->
    // inject nothing. Other threads caught by CheckROITransition.
    PIN_RemoveInstrumentation();
  }
}

/* =========================================================================
 * Analysis callbacks — ROI transition check (TRACE granularity)
 *
 * Fires once per JIT trace while inside or entering the ROI. Handles
 * threads that were mid-execution when roi_started/roi_ended flipped.
 * ========================================================================= */

VOID CheckROITransition(THREADID tid)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase == Phase::DONE)
    return;

  // Case 1: ROI ended — transition to DONE.
  if (roi_ended.load(std::memory_order_acquire)) {
    if (ts->phase == Phase::TRACING) {
      active_tracing_threads.fetch_sub(1, std::memory_order_acq_rel);
      ts->force_close();
      {
        LogGuard _lg;
        std::cerr << "[tracer_roi] Thread " << ts->os_tid
                  << " -> DONE via CheckROITransition (roi_ended)."
                  << std::endl;
      }
    }
    ts->phase = Phase::DONE;
    return;
  }

  // Case 2: ROI started but this thread still waiting.
  if (roi_started.load(std::memory_order_acquire) &&
      ts->phase == Phase::WAITING_FOR_ROI) {
    ts->phase   = Phase::TRACING;
    ts->counter = ts->trace_per_sample;
    ts->open_next_sample();
    active_tracing_threads.fetch_add(1, std::memory_order_acq_rel);
    // No additional flush needed — HandleMarker already flushed the
    // cache with full analysis injected (active_tracing_threads > 0).
    {
      LogGuard _lg;
      std::cerr << "[tracer_roi] Thread " << ts->os_tid
                << " -> TRACING via CheckROITransition." << std::endl;
    }
  }
}

/* =========================================================================
 * Analysis callbacks — skip phase (TRACE granularity, near-native)
 * ========================================================================= */

VOID FastForwardInitial(THREADID tid, UINT32 trace_icount)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase != Phase::INITIAL_SKIP)
    return;

  if (ts->counter > (UINT64)trace_icount) {
    ts->counter -= trace_icount;
  } else {
    // Transition to TRACING. Boundary imprecision: at most trace_icount
    // instructions skipped beyond the requested count — negligible for
    // skip counts of 100M+ instructions.
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
 * Analysis callbacks — trace phase (INS granularity)
 * ========================================================================= */

VOID RecordInstr(THREADID tid, ADDRINT ip, UINT8 is_branch, UINT8 branch_taken)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase != Phase::TRACING)
    return;

  ts->curr_instr.ip           = ip;
  ts->curr_instr.is_branch    = is_branch;
  ts->curr_instr.branch_taken = branch_taken;

  // Feed 64 bytes into the per-thread zstd streaming compressor.
  // fwrite occurs only when the 128KB output buffer fills up.
  ts->compress_write(&ts->curr_instr, sizeof(trace_instr_format));

  ts->counter--;
  ts->reset_instr();

  if (ts->counter == 0)
    leave_tracing(ts);
}

VOID RecordMemRead(THREADID tid, VOID *addr)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase != Phase::TRACING)
    return;

  for (int i = 0; i < NUM_INSTR_SOURCES; i++) {
    if (ts->curr_instr.source_memory[i] == 0) {
      ts->curr_instr.source_memory[i] =
        reinterpret_cast<unsigned long long int>(addr);
      return;
    }
  }
}

VOID RecordMemWrite(THREADID tid, VOID *addr)
{
  ThreadState *ts = get_state(tid);
  if (!ts || ts->phase != Phase::TRACING)
    return;

  for (int i = 0; i < NUM_INSTR_DESTINATIONS; i++) {
    if (ts->curr_instr.destination_memory[i] == 0) {
      ts->curr_instr.destination_memory[i] =
        reinterpret_cast<unsigned long long int>(addr);
      return;
    }
  }
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

/* =========================================================================
 * Per-instruction full analysis injection helper
 * ========================================================================= */

static void insert_full_analysis(INS ins)
{
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

  UINT32 mem_ops = INS_MemoryOperandCount(ins);
  for (UINT32 i = 0; i < mem_ops; i++) {
    if (INS_MemoryOperandIsRead(ins, i)) {
      INS_InsertPredicatedCall(ins,
                               IPOINT_BEFORE,
                               (AFUNPTR)RecordMemRead,
                               IARG_THREAD_ID,
                               IARG_MEMORYOP_EA,
                               i,
                               IARG_END);
    }
    if (INS_MemoryOperandIsWritten(ins, i)) {
      INS_InsertPredicatedCall(ins,
                               IPOINT_BEFORE,
                               (AFUNPTR)RecordMemWrite,
                               IARG_THREAD_ID,
                               IARG_MEMORYOP_EA,
                               i,
                               IARG_END);
    }
  }

  // IPOINT_BEFORE throughout — PIN preserves insertion order at the
  // same IPOINT, so register/memory callbacks always fire before
  // RecordInstr commits the record to the compressor.
  if (INS_IsBranch(ins)) {
    INS_InsertCall(ins,
                   IPOINT_BEFORE,
                   (AFUNPTR)RecordInstr,
                   IARG_THREAD_ID,
                   IARG_INST_PTR,
                   IARG_UINT32,
                   (UINT32)1,
                   IARG_BRANCH_TAKEN,
                   IARG_END);
  } else {
    INS_InsertCall(ins,
                   IPOINT_BEFORE,
                   (AFUNPTR)RecordInstr,
                   IARG_THREAD_ID,
                   IARG_INST_PTR,
                   IARG_UINT32,
                   (UINT32)0,
                   IARG_UINT32,
                   (UINT32)0,
                   IARG_END);
  }
}

/* =========================================================================
 * TRACE-granularity instrumentation callback
 *
 * Four cases (evaluated in priority order):
 *
 *   Case 1 — roi_ended:
 *     Inject nothing. Near-native post-ROI execution.
 *
 *   Case 2 — marker mode, pre-ROI:
 *     Scan for xchg rcx,rcx only. No other instrumentation.
 *     True near-native speed for pre-ROI phases (graph loading, etc.).
 *
 *   Case 3 — inside ROI / skip-based mode, threads tracing:
 *     Full per-instruction analysis + CheckROITransition + marker scan.
 *
 *   Case 4 — inside ROI / skip-based mode, no threads tracing:
 *     Cheap TRACE-level skip counters + CheckROITransition + marker scan.
 *     Near-native speed during inter-sample skip windows.
 * ========================================================================= */

VOID InstrumentTrace(TRACE trace, VOID * /* unused */)
{
  bool use_markers = KnobUseMarkers.Value();
  bool roi_done    = roi_ended.load(std::memory_order_acquire);
  bool roi_active  = roi_started.load(std::memory_order_acquire);
  int  tracing     = active_tracing_threads.load(std::memory_order_acquire);

  // --- Case 1 ---
  if (roi_done)
    return;

  // --- Case 2 ---
  if (use_markers && !roi_active) {
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
      for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
        if (is_roi_marker(ins)) {
          INS_InsertCall(ins,
                         IPOINT_BEFORE,
                         (AFUNPTR)HandleMarker,
                         IARG_THREAD_ID,
                         IARG_REG_VALUE,
                         REG_RCX,
                         IARG_END);
        }
      }
    }
    return;
  }

  // --- Cases 3 & 4 ---

  // CheckROITransition fires once per trace to handle stragglers and
  // roi_ended propagation across thread trace boundaries.
  TRACE_InsertCall(trace,
                   IPOINT_BEFORE,
                   (AFUNPTR)CheckROITransition,
                   IARG_THREAD_ID,
                   IARG_END);

  if (tracing > 0) {
    // --- Case 3: full per-instruction analysis ---
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
      for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
        if (use_markers && is_roi_marker(ins)) {
          INS_InsertCall(ins,
                         IPOINT_BEFORE,
                         (AFUNPTR)HandleMarker,
                         IARG_THREAD_ID,
                         IARG_REG_VALUE,
                         REG_RCX,
                         IARG_END);
        }
        insert_full_analysis(ins);
      }
    }
  } else {
    // --- Case 4: cheap TRACE-level skip counters ---
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
                           REG_RCX,
                           IARG_END);
          }
        }
      }
    }
  }
}

/* =========================================================================
 * Thread lifecycle callbacks
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

  bool   roi_already_started = roi_started.load(std::memory_order_acquire);
  Phase  starting_phase;
  UINT64 starting_counter;

  if (use_markers) {
    if (roi_already_started) {
      // Thread spawned inside ROI — begin tracing immediately.
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

  // is_master starts false; set to true by HandleMarker if this thread
  // executes champsim_roi_begin().
  ThreadState *ts = new ThreadState(os_tid,
                                    KnobOutputBase.Value(),
                                    starting_phase,
                                    starting_counter,
                                    inter_skip,
                                    trace_count,
                                    num_samples,
                                    false,
                                    level);

  PIN_RWMutexWriteLock(&registry_lock);
  thread_states[tid] = ts;
  PIN_RWMutexUnlock(&registry_lock);

  if (ts->phase == Phase::TRACING) {
    active_tracing_threads.fetch_add(1, std::memory_order_acq_rel);
    // Re-JIT: full analysis injected (active_tracing_threads > 0).
    PIN_RemoveInstrumentation();
  }

  {
    LogGuard _lg;
    std::cerr << "[tracer_roi] Thread start:"
              << " PIN tid=" << tid << " OS tid=" << os_tid
              << " starting_phase=" << phase_name(ts->phase)
              << " inter_skip=" << inter_skip << " trace=" << trace_count
              << " max_samples=" << num_samples << " zstd_level=" << level
              << std::endl;
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
      std::cerr << "[tracer_roi] Thread " << ts->os_tid
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
    std::cerr << "[tracer_roi] Thread fini: OS tid=" << ts->os_tid
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
  for (int i = 0; i < PIN_MAX_THREADS; i++) {
    if (thread_states[i]) {
      thread_states[i]->force_close();
      delete thread_states[i];
      thread_states[i] = nullptr;
    }
  }
  {
    LogGuard _lg;
    std::cerr << "[tracer_roi] All threads finished." << std::endl;
  }
}

/* =========================================================================
 * Usage
 * ========================================================================= */

INT32 Usage()
{
  std::cerr
    << "champsim_tracer_mt_roi:\n"
    << "  ROI-aware, optimized, multi-threaded, periodically-sampled\n"
    << "  ChampSim tracer with online zstd compression.\n\n"
    << "Output files:\n"
    << "  <base>_t<os_tid>_master_s<sid>.champsim.zst  (discard)\n"
    << "  <base>_t<os_tid>_s<sid>.champsim.zst          (use)\n\n"
    << "Decompress: zstd -d <file>  or  zstd -d -c <file> | <reader>\n\n"
    << "Compression: default level 1 (~500-800 MB/s).\n"
    << "  Use -zstd_level 3 if you observe headroom. Never exceed 3.\n\n"
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
    std::cerr << "[tracer_roi] Starting.\n"
              << "  output base   : " << KnobOutputBase.Value() << "\n"
              << "  use_markers   : " << KnobUseMarkers.Value() << "\n"
              << "  initial skip  : " << KnobInitialSkip.Value()
              << (KnobUseMarkers.Value() ? " (ignored)" : "") << "\n"
              << "  inter skip    : " << KnobInterSampleSkip.Value() << "\n"
              << "  trace/sample  : " << KnobTraceInstructions.Value() << "\n"
              << "  max samples   : " << KnobNumSamples.Value() << "\n"
              << "  zstd level    : " << KnobZstdLevel.Value() << "\n"
              << "  out buf size  : " << OUT_BUF_SIZE / 1024 << " KB\n"
              << "  main_only     : " << KnobMainThreadOnly.Value() << "\n"
              << std::endl;
  }

  PIN_StartProgram();
  return EXIT_SUCCESS;
}
