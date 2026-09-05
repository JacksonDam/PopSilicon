#ifndef ARB_PROGRAM_GUARD_H
#define ARB_PROGRAM_GUARD_H

#include <stddef.h>

char *arb_program_guard_undefined_math(const void *source, size_t source_size,
                                       size_t *output_size,
                                       size_t *rsq_guard_count,
                                       size_t *rcp_guard_count);

char *arb_program_plain_shadow_targets(const void *source, size_t source_size,
                                       size_t *output_size,
                                       size_t *rewrite_count);

#endif
