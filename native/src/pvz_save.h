#ifndef LP32_PVZ_SAVE_H
#define LP32_PVZ_SAVE_H
#include <stddef.h>
#include <stdbool.h>
/* Validated v12 userN.dat conversion. Caller owns output; NULL rejects unknown/lossy layouts. */
void *pvz_save_convert(const void *data, size_t size, bool to_mac, size_t *output_size);
bool pvz_save_is_profile(const char *name);
#endif
