#include "arb_sampler_usage.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static uint32_t *mask_for_target(struct arb_sampler_usage *usage,
                                 const char *target, size_t target_size)
{
    if (target_size == 2 && !memcmp(target, "1D", 2)) {
        return &usage->texture_1d;
    }
    if (target_size == 2 && !memcmp(target, "2D", 2)) {
        return &usage->texture_2d;
    }
    if (target_size == 2 && !memcmp(target, "3D", 2)) {
        return &usage->texture_3d;
    }
    if (target_size == 4 && !memcmp(target, "CUBE", 4)) {
        return &usage->texture_cube;
    }
    if (target_size == 4 && !memcmp(target, "RECT", 4)) {
        return &usage->texture_rect;
    }
    return NULL;
}

void arb_sampler_usage_parse(const void *raw_source, size_t source_size,
                             struct arb_sampler_usage *usage)
{
    const char *source = raw_source;
    static const char marker[] = "texture[";
    memset(usage, 0, sizeof(*usage));
    if (!source) return;

    size_t offset = 0;
    while (offset + sizeof(marker) - 1 <= source_size) {
        const char *found = NULL;
        for (size_t index = offset;
             index + sizeof(marker) - 1 <= source_size; ++index) {
            if (!memcmp(source + index, marker, sizeof(marker) - 1)) {
                found = source + index;
                break;
            }
        }
        if (!found) break;

        const char *cursor = found + sizeof(marker) - 1;
        const char *end = source + source_size;
        unsigned int unit = 0;
        bool have_digit = false;
        while (cursor < end && isdigit((unsigned char)*cursor)) {
            have_digit = true;
            if (unit <= 1000) {
                unit = unit * 10 + (unsigned int)(*cursor - '0');
            }
            ++cursor;
        }
        if (!have_digit || cursor >= end || *cursor != ']') {
            offset = (size_t)(found - source) + sizeof(marker) - 1;
            continue;
        }
        ++cursor;
        while (cursor < end && *cursor != ',' && *cursor != ';' &&
               *cursor != '\n' && *cursor != '\r') {
            ++cursor;
        }
        if (cursor >= end || *cursor != ',') {
            offset = (size_t)(found - source) + sizeof(marker) - 1;
            continue;
        }
        ++cursor;
        while (cursor < end && isspace((unsigned char)*cursor)) ++cursor;
        const char *target = cursor;
        while (cursor < end && (isalnum((unsigned char)*cursor) ||
                                *cursor == '_')) {
            ++cursor;
        }
        uint32_t *mask = mask_for_target(usage, target,
                                         (size_t)(cursor - target));
        if (mask && unit < 32) *mask |= UINT32_C(1) << unit;
        offset = (size_t)(found - source) + sizeof(marker) - 1;
    }
}
