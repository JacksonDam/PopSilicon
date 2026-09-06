#ifndef LP32_COMPAT_RUNTIME_H
#define LP32_COMPAT_RUNTIME_H

#include "macho_loader.h"

#include <stddef.h>
#include <stdint.h>

void compat_runtime32_struct_return(void);

int compat_runtime32_initialize(struct macho_image32 *image);
/* The loaded guest image (valid after compat_runtime32_initialize). */
const struct macho_image32 *compat_runtime32_image(void);
uint32_t compat_runtime32_call(uint32_t function, const uint32_t *arguments,
                               size_t argument_count);
int compat_runtime32_last_call_trapped(void);
uint32_t compat_runtime32_copy_cstring(const char *string);
uint32_t compat_runtime32_allocate(size_t size, int clear);
uint32_t compat_runtime32_reallocate(uint32_t pointer, size_t size);
void compat_runtime32_deallocate(uint32_t pointer);
void compat_runtime32_heap_report(const char *reason);
void compat_runtime32_heap_frame(uint64_t swap_count);
int compat_runtime32_run_heap_self_test(void);
int compat_runtime32_run_file_self_test(void);
int compat_runtime32_run_cg_self_test(void);
int compat_runtime32_run_sync_self_test(void);
uint32_t compat_runtime32_cg_object_count(void);
uint32_t compat_runtime32_cg_string_count(void);
uint64_t compat_runtime32_dispatch_import(const char *name,
                                          const uint32_t *arguments);

/* A guest-callable (cdecl) entry point that dispatches to the host under the
   given import name, for function pointers the game expects to call back
   (the same mechanism dlsym results use).  0 if the thunk table is full. */
uint32_t compat_runtime32_guest_callback(const char *name);

/* Install a handler consulted first by the import dispatcher, before any
   built-in bridge.  Returns 1 (and sets *result) to handle an import by name,
   0 to fall through.  Used by the Steam DRM unwrap to shim the handful of
   libSystem/dyld/mach functions Valve's decryptor calls.  NULL disables it. */
void compat_runtime32_set_named_import_override(
    int (*handler)(const char *name, const uint32_t *arguments,
                   uint64_t *result));

/* Per-thread bridge cost counters, accumulated only while
   LP32_FRAME_STATS is set.  The render thread reads and clears them once
   per swap. */
struct compat_runtime32_frame_profile {
    uint64_t calls;
    uint64_t dispatch_ns;
    uint64_t audio_ns;
    uint64_t objc_ns;
    uint64_t lock_wait_ns;
    uint64_t lock_waits;
};
extern int compat_runtime32_frame_profile_enabled;
void compat_runtime32_take_frame_profile(
    struct compat_runtime32_frame_profile *out);
void compat_runtime32_report_import_profile(unsigned top);

/* Mode guards: how many times a thread arrived at a 64-bit pad still in
   i386 mode (to64) or at the i386 landing trampoline still in x86_64 mode
   (to32) and was transparently redirected.  See build_transition_bridge. */
void compat_runtime32_mode_guard_counts(uint32_t *recovered_to64,
                                        uint32_t *recovered_to32);
void compat_runtime32_check_mode_guards(uint64_t swap_count);

/* Non-zero once the game tried to read its pcconfig.txt and found none
   (a first launch, so no launcher-chosen resolution exists yet). */
int compat_runtime32_game_config_missing(void);
void compat_runtime32_set_diagnostic_sink(void (*sink)(const char *line));

/*
 * Direct handlers for the hottest imports.  The bridges match import names
 * with long if-chains; at 25-30 thousand imports per frame the chain walk
 * alone is a tenth of the render thread.  A bridge may offer a handler for a
 * name, and the runtime memoizes it per import id after the first (chained)
 * dispatch, so later calls skip the chains entirely.  Handlers receive the
 * i386 argument words and return the eax:edx value; float results go through
 * the same lp32_fp_result path as the chained handlers.
 */
typedef uint64_t (*lp32_fast_import_fn)(const uint32_t *arguments,
                                        uint32_t return_address);

/* Return a floating-point result to the guest (x87 st0), for bridges outside
   compat_runtime.c (e.g. objc_msgSend_fpret).  Returns the eax:edx value the
   import handler should return (0). */
uint64_t compat_runtime32_return_double(double value);
uint64_t compat_runtime32_return_float(float value);

#endif
