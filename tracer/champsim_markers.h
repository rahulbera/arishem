/*
 * champsim_roi_markers.h
 *
 * Lightweight ROI (Region of Interest) markers for use with the
 * ChampSim multi-threaded PIN tracer (champsim_tracer_mt_roi.cpp).
 *
 * USAGE
 * -----
 * Include this header in any C or C++ source file, then bracket the
 * region you want to trace with champsim_roi_begin() and champsim_roi_end():
 *
 *   #include "champsim_roi_markers.h"
 *
 *   int main() {
 *       load_graph(...);           // pre-processing: NOT traced
 *
 *       champsim_roi_begin();      // <<< tracing starts here
 *       run_bfs_kernel(...);       // multi-threaded kernel: traced
 *       champsim_roi_end();        // <<< tracing ends here
 *
 *       print_results(...);        // post-processing: NOT traced
 *   }
 *
 * HOW IT WORKS
 * ------------
 * The marker functions emit a distinctive "magic NOP" instruction:
 *
 *   xchg %rcx, %rcx
 *
 * This instruction is architecturally a no-op (swapping a register with
 * itself changes nothing) but has a specific opcode encoding that the PIN
 * tool can detect by scanning executed instructions. The value loaded into
 * RCX before the instruction encodes whether it is a begin or end marker:
 *
 *   RCX == CHAMPSIM_ROI_BEGIN (1)  ->  begin tracing
 *   RCX == CHAMPSIM_ROI_END   (2)  ->  end tracing
 *
 * Compiler memory barriers on both sides of the magic instruction prevent
 * the compiler from reordering memory operations across the marker, ensuring
 * that all pre-ROI work is architecturally complete before tracing starts.
 *
 * NATIVE EXECUTION (without PIN)
 * ------------------------------
 * The marker functions are safe to call in binaries run without PIN.
 * The xchg instruction is a true NOP and the printf calls are informational.
 * No side effects on program correctness.
 *
 * COMPATIBILITY
 * -------------
 * - C99 and later (compile with -std=c99 or later)
 * - C++11 and later
 * - x86-64 Linux only (the inline assembly is GCC/Clang syntax, x86-64 ABI)
 * - Header-only: no separate compilation unit required
 */

#ifndef CHAMPSIM_ROI_MARKERS_H
#define CHAMPSIM_ROI_MARKERS_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Marker opcode values
 *
 * These constants are shared between this header and the PIN tool.
 * If you change them here, you MUST update CHAMPSIM_ROI_BEGIN and
 * CHAMPSIM_ROI_END in champsim_tracer_mt_roi.cpp as well.
 * ========================================================================= */

#define CHAMPSIM_ROI_BEGIN       ((uint64_t)1)
#define CHAMPSIM_ROI_END         ((uint64_t)2)
#define CHAMPSIM_REGISTER_WORKER ((uint64_t)3)  /* v3 tracer only */

/* =========================================================================
 * Compiler barrier
 *
 * Prevents the compiler from moving memory accesses across this point.
 * Does not emit any machine instruction — purely a compiler hint.
 * ========================================================================= */

#define CHAMPSIM_COMPILER_BARRIER()                                            \
  do {                                                                         \
    __asm__ __volatile__("" ::: "memory");                                     \
  } while (0)

/* =========================================================================
 * Core marker primitive
 *
 * Loads `op` into RCX, then executes xchg %rcx, %rcx.
 * The "c" constraint maps the C variable `op` to the RCX register.
 * Compiler barriers on both sides ensure no reordering.
 * ========================================================================= */

static inline void champsim_marker(uint64_t op)
{
  CHAMPSIM_COMPILER_BARRIER();
  __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op) :);
  CHAMPSIM_COMPILER_BARRIER();
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/*
 * champsim_roi_begin()
 *
 * Marks the start of the region of interest. Call this immediately before
 * the multi-threaded computation you want to trace. All threads alive at
 * this point (and all threads spawned after this point) will begin tracing
 * from their next instruction.
 *
 * Typically called from the master thread, before spawning worker threads
 * or before entering a parallel region.
 */
static inline void champsim_roi_begin(void)
{
  printf("[ChampSim] ROI begin\n");
  fflush(stdout);
  champsim_marker(CHAMPSIM_ROI_BEGIN);
}

/*
 * champsim_roi_end()
 *
 * Marks the end of the region of interest. All threads will stop tracing
 * when this marker is detected, regardless of how many samples they have
 * collected. Partial sample files are kept.
 *
 * Typically called from the master thread, after joining all worker threads
 * or after exiting a parallel region.
 */
static inline void champsim_roi_end(void)
{
  champsim_marker(CHAMPSIM_ROI_END);
  printf("[ChampSim] ROI end\n");
  fflush(stdout);
}

/*
 * champsim_register_worker()  [v3 tracer only]
 *
 * Marks the calling thread as a foreground worker. The v3 tracer (when
 * launched with -trace_only_registered_workers 1) will only allow threads
 * that have called this marker to enter TRACING. Threads that never call
 * this marker (e.g. RocksDB pthread-pool flush/compaction threads, OpenMP
 * workers spawned by a library) stay in WAITING_FOR_ROI and never count
 * toward active_tracing_threads, so they cannot disrupt INTER_SKIP timing.
 *
 * Older tracers (v1, v2) ignore this marker -- the magic NOP is still
 * benign there, since unrecognised RCX values fall through HandleMarker.
 *
 * Call this from the top of each foreground worker thread, AFTER the
 * thread is pinned and ready to start its work. May be called BEFORE or
 * AFTER champsim_roi_begin(); the registration is permanent for the
 * thread's lifetime.
 */
static inline void champsim_register_worker(void)
{
  champsim_marker(CHAMPSIM_REGISTER_WORKER);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHAMPSIM_ROI_MARKERS_H */