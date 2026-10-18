#ifndef LP32_STEAM_CLOUD_H
#define LP32_STEAM_CLOUD_H
#include <stdint.h>
/* Handles PvZ's legacy storage interface only in an explicitly Cloud-enabled bundle. */
int steam_cloud_dispatch(const char *name, const uint32_t *arguments, uint64_t *result);
#endif
