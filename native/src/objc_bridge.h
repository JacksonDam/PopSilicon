#ifndef LP32_OBJC_BRIDGE_H
#define LP32_OBJC_BRIDGE_H

#include "compat_runtime.h"

#include <stdint.h>

int objc_bridge32_dispatch(const char *import_name, const uint32_t *arguments,
                           uint64_t *result);
/* Direct handler for a hot OpenGL import, or NULL to keep using the chain. */
lp32_fast_import_fn objc_bridge32_fast_import(const char *import_name);
uint32_t objc_bridge32_pointer_import(const char *import_name);
int objc_bridge32_run_proxy_self_test(void);
int objc_bridge32_run_gl_parameter_self_test(void);
int objc_bridge32_run_gl_buffer_self_test(void);
int objc_bridge32_run_gl_texture_self_test(void);

#endif
