#include "game_profile.h"
#include "macho_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Peggle Deluxe 1.0.5 (PopCap, 2007; original i386 Mac executable). */
static const struct lp32_game_profile peggle_profile = {
    .title = LP32_TITLE_PEGGLE,
    .name = "PeggleSilicon",
    .display_name = "Peggle Deluxe",
    .log_directory = "PeggleSilicon",
    .image_file = "Peggle.image",
    .entry_eip = 0x0000538c,
    .image_end = 0x0051b000,
    .main_address = 0x0006a33e,
};

static const struct lp32_game_profile unknown_profile = {
    .title = LP32_TITLE_UNKNOWN,
    .name = "unknown",
    .display_name = "unknown title",
    .log_directory = "PeggleSilicon",
    .image_file = "Peggle.image",
};

static const struct lp32_game_profile *current_profile = &unknown_profile;

const struct lp32_game_profile *lp32_profile(void)
{
    return current_profile;
}

const struct lp32_game_profile *lp32_profile_named(const char *name)
{
    if (!name) return NULL;
    if (strcasecmp(name, "PeggleSilicon") == 0 ||
        strcasecmp(name, "Peggle") == 0) return &peggle_profile;
    return NULL;
}

static uint32_t call_target(const struct macho_image32 *image, uint32_t site)
{
    int32_t relative;
    memcpy(&relative, (const void *)(uintptr_t)(site + 1), sizeof(relative));
    uint32_t target = site + 5 + (uint32_t)relative;
    return target >= image->min_address && target < image->max_address ? target : 0;
}

uint32_t lp32_profile_main_address(const struct macho_image32 *image)
{
    const uint8_t *stub = (const void *)(uintptr_t)image->entry_eip;
    uint32_t crt_start = 0;
    for (uint32_t offset = 0; offset + 6 <= 96; ++offset) {
        if (stub[offset] != 0xe8 || stub[offset + 5] != 0xf4) continue;
        crt_start = call_target(image, image->entry_eip + offset);
        if (crt_start) break;
    }
    if (!crt_start) return 0;
    const uint8_t *code = (const void *)(uintptr_t)crt_start;
    for (uint32_t offset = 0; offset + 9 <= 0x200; ++offset) {
        if (code[offset] != 0xe8 || code[offset + 5] != 0x89 ||
            code[offset + 6] != 0x04 || code[offset + 7] != 0x24 ||
            code[offset + 8] != 0xe8) continue;
        uint32_t target = call_target(image, crt_start + offset);
        if (target) return target;
    }
    return 0;
}

int lp32_profile_select(const struct macho_image32 *image)
{
    const char *override = getenv("LP32_GAME");
    if (override && override[0] && !lp32_profile_named(override)) {
        fprintf(stderr, "game_loader: unknown LP32_GAME profile: %s\n", override);
        return -1;
    }
    if ((image->entry_eip != peggle_profile.entry_eip ||
         image->max_address != peggle_profile.image_end) &&
        !(override && override[0])) {
        fprintf(stderr,
                "game_loader: unrecognised image (entry=0x%08x end=0x%08x)\n",
                image->entry_eip, image->max_address);
        return -1;
    }
    current_profile = &peggle_profile;
    fprintf(stderr, "compat32: title profile %s (%s)\n",
            current_profile->name, current_profile->display_name);
    return 0;
}
