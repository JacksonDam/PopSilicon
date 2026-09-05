#ifndef ARB_SAMPLER_USAGE_H
#define ARB_SAMPLER_USAGE_H

#include <stddef.h>
#include <stdint.h>

struct arb_sampler_usage {
    uint32_t texture_1d;
    uint32_t texture_2d;
    uint32_t texture_3d;
    uint32_t texture_cube;
    uint32_t texture_rect;
};

void arb_sampler_usage_parse(const void *source, size_t source_size,
                             struct arb_sampler_usage *usage);

#endif
