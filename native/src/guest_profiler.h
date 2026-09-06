#ifndef LP32_GUEST_PROFILER_H
#define LP32_GUEST_PROFILER_H

/*
 * Sampling profiler for guest (i386) code.  `sample` cannot see inside the
 * Rosetta-translated guest, so this raises SIGPROF on the render thread at a
 * fixed wall-clock interval and records the interrupted PC together with
 * whether the thread was in the i386 code segment.  LP32_PROFILE_GUEST=<us>
 * enables it (interval in microseconds); LP32_PROFILE_OUT=<path> names the
 * output (default $TMPDIR/lp32-profile-<pid>.txt).  Report with
 * tools/guest_profile_report.py.
 *
 * Caveat: the sample is an asynchronous signal.  Rosetta occasionally aborts
 * the process when one lands while it is inside its own transition code
 * ("expected ARM LR to be in translated code"); this is a diagnostic, not
 * something to leave on.  1000 us has run for many minutes; 500 us crashed
 * once during a heavy effect.
 */
void lp32_guest_profiler_start(void);

#endif
