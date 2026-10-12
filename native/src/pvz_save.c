#include "pvz_save.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint32_t u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }

bool pvz_save_is_profile(const char *name)
{
    if (!name || strncasecmp(name, "user", 4)) return false;
    name += 4;
    if (*name < '0' || *name > '9') return false;
    while (*name >= '0' && *name <= '9') ++name;
    return !strcasecmp(name, ".dat");
}

static bool layout(const unsigned char *data, size_t size, uint32_t count, size_t stride)
{
    if (count > (size - 820) / stride) return false;
    size_t tail = 820 + count * stride;
    size_t length = size - tail;
    if (length < 68) return false;
    return length == 68 + (size_t)data[tail + 43] * 72;
}

void *pvz_save_convert(const void *input, size_t size, bool to_mac, size_t *output_size)
{
    const unsigned char *data = input;
    if (!data || size < 888 || u32(data) != 12) return NULL;
    uint32_t count = u32(data + 816);
    bool win = layout(data, size, count, 88), mac = layout(data, size, count, 60);
    if ((!win && !mac) || (count && win && mac)) return NULL;
    size_t source_stride = win ? 88 : 60, target_stride = to_mac ? 60 : 88;
    size_t tail = 820 + count * source_stride;
    size_t out_size = 820 + (size_t)count * target_stride + size - tail;
    unsigned char *out = calloc(1, out_size);
    if (!out) return NULL;
    memcpy(out, data, 820);
    /* Zen Garden plant fields: four time_t values widen/pad on Windows.
       Verified layout: https://github.com/petronny/pvz_converter */
    static const size_t w[] = {0,24,32,56,64}, m[] = {0,20,24,44,48}, lengths[] = {20,4,20,4,12};
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *src = data + 820 + i * source_stride;
        unsigned char *dst = out + 820 + i * target_stride;
        if (source_stride == target_stride) { memcpy(dst, src, source_stride); continue; }
        if (win) {
            static const size_t omitted[] = {20,28,52,60,76,80,84};
            for (size_t j=0; j<sizeof(omitted)/sizeof(*omitted); ++j)
                if (u32(src + omitted[j])) { free(out); return NULL; }
        }
        for (size_t j=0; j<5; ++j)
            memcpy(dst + (to_mac ? m[j] : w[j]), src + (win ? w[j] : m[j]), lengths[j]);
    }
    memcpy(out + 820 + count * target_stride, data + tail, size - tail);
    *output_size = out_size;
    return out;
}
