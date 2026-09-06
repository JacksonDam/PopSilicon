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
 */
void lp32_guest_profiler_start(void);

#endif
