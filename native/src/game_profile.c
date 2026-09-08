#include "game_profile.h"
#include "macho_loader.h"

#include <stdbool.h>
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

/* Peggle Nights 1.0.4 (PopCap, 2008; original i386 Mac executable).  Same
   "Sexy" engine as Deluxe; main is derived from the crt start stub. */
static const struct lp32_game_profile peggle_nights_profile = {
    .title = LP32_TITLE_PEGGLE_NIGHTS,
    .name = "PeggleNights",
    .display_name = "Peggle Nights",
    .log_directory = "PeggleNights",
    .image_file = "PeggleNights.image",
    .entry_eip = 0x0000431c,
    .image_end = 0x004ad000,
    .main_address = 0,
};

/* Bejeweled 3 1.1.12.4196 (PopCap, 2010; original i386 Mac executable).  Same
   "Sexy" engine, newer/larger; the Steam copy is not Valve-DRM-wrapped but
   links libsteam_api.dylib (SteamAPI shims live in the runtime).  main is
   derived from the crt start stub. */
static const struct lp32_game_profile bejeweled3_profile = {
    .title = LP32_TITLE_BEJEWELED3,
    .name = "Bejeweled3",
    .display_name = "Bejeweled 3",
    .log_directory = "Bejeweled3",
    .image_file = "Bejeweled3.image",
    .entry_eip = 0x000020e0,
    .image_end = 0x00b427c4,
    /* The crt stub calls main directly (call main; mov [esp],eax; call exit;
       hlt), which the generic derivation doesn't recognise, so pin it. */
    .main_address = 0x00194a10,
};

/* Bejeweled 2 Deluxe 1.0.0 (PopCap, 2004; original i386 Mac executable).  The
   earliest "Sexy" engine of the four: a pure Carbon/AGL title (no Cocoa) whose
   music comes from BASSMOD rather than BASS.  The Steam copy carries a __STEAM
   segment but leaves __TEXT in the clear, so no unwrap step is needed.  main is
   derived from the crt start stub. */
static const struct lp32_game_profile bejeweled2_profile = {
    .title = LP32_TITLE_BEJEWELED2,
    .name = "Bejeweled2",
    .display_name = "Bejeweled 2 Deluxe",
    .log_directory = "Bejeweled2",
    .image_file = "Bejeweled2.image",
    .entry_eip = 0x000021dc,
    .image_end = 0x002d8338,
    .main_address = 0,
};

/* Zuma Deluxe 1.0.0 (PopCap, 2003; original i386 Mac executable).  Bejeweled
   2's twin: the same early Carbon/AGL engine with FMOD over the Sound Manager
   and BASSMOD for its music, and the same unencrypted __STEAM ownership stub.
   main is derived from the crt start stub. */
static const struct lp32_game_profile zuma_profile = {
    .title = LP32_TITLE_ZUMA,
    .name = "Zuma",
    .display_name = "Zuma Deluxe",
    .log_directory = "Zuma",
    .image_file = "Zuma.image",
    .entry_eip = 0x0000299c,
    .image_end = 0x002f6358,
    .main_address = 0,
};

/* Plants vs. Zombies 1.0.41 (PopCap, 2009; original i386 Mac executable).
   The same "Sexy" engine generation as Bejeweled 3 -- Cocoa, AGL, BASS and
   libsteam_api -- and, like the Peggle titles, its Steam copy is wrapped in
   Valve's Mach-O DRM, so tools/build.py unwraps it first.  main is derived
   from the crt start stub. */
static const struct lp32_game_profile plantsvszombies_profile = {
    .title = LP32_TITLE_PLANTSVSZOMBIES,
    .name = "PlantsVsZombies",
    .display_name = "Plants vs. Zombies",
    .log_directory = "PlantsVsZombies",
    .image_file = "PlantsVsZombies.image",
    .entry_eip = 0x0000a710,
    .image_end = 0x003fd000,
    .main_address = 0,
};

/* Chuzzle Deluxe 1.0.0 (Raptisoft for PopCap, 2005; original i386 Mac
   executable).  Not a "Sexy" engine title at all: it is an SDL 1.2 game whose
   window, input, threads and image loading come from the SDL and SDL_image
   frameworks bundled beside it, with BASS (the 2.0-era API) for audio.  Like
   Bejeweled 2, the Steam copy carries an unencrypted __STEAM ownership stub.
   main is derived from the crt start stub. */
static const struct lp32_game_profile chuzzle_profile = {
    .title = LP32_TITLE_CHUZZLE,
    .name = "Chuzzle",
    .display_name = "Chuzzle Deluxe",
    .log_directory = "Chuzzle",
    .image_file = "Chuzzle.image",
    .entry_eip = 0x00002b9c,
    .image_end = 0x0017a318,
    .main_address = 0,
};

static const struct lp32_game_profile unknown_profile = {
    .title = LP32_TITLE_UNKNOWN,
    .name = "unknown",
    .display_name = "unknown title",
    .log_directory = "PeggleSilicon",
    .image_file = "Peggle.image",
};

/* All shipping titles, in detection order. */
static const struct lp32_game_profile *const known_profiles[] = {
    &peggle_profile,
    &peggle_nights_profile,
    &bejeweled3_profile,
    &bejeweled2_profile,
    &chuzzle_profile,
    &plantsvszombies_profile,
    &zuma_profile,
};

static const struct lp32_game_profile *current_profile = &unknown_profile;

const struct lp32_game_profile *lp32_profile(void)
{
    return current_profile;
}

const struct lp32_game_profile *const *lp32_known_profiles(size_t *count)
{
    if (count) *count = sizeof(known_profiles) / sizeof(known_profiles[0]);
    return known_profiles;
}

const struct lp32_game_profile *lp32_profile_named(const char *name)
{
    if (!name) return NULL;
    if (strcasecmp(name, "PeggleSilicon") == 0 ||
        strcasecmp(name, "Peggle") == 0 ||
        strcasecmp(name, "PeggleDeluxe") == 0 ||
        strcasecmp(name, "Peggle Deluxe") == 0) return &peggle_profile;
    if (strcasecmp(name, "PeggleNights") == 0 ||
        strcasecmp(name, "Peggle Nights") == 0) return &peggle_nights_profile;
    if (strcasecmp(name, "Bejeweled3") == 0 ||
        strcasecmp(name, "Bejeweled 3") == 0) return &bejeweled3_profile;
    if (strcasecmp(name, "Bejeweled2") == 0 ||
        strcasecmp(name, "Bejeweled 2") == 0 ||
        strcasecmp(name, "Bejeweled 2 Deluxe") == 0) return &bejeweled2_profile;
    if (strcasecmp(name, "Zuma") == 0 ||
        strcasecmp(name, "Zuma Deluxe") == 0) return &zuma_profile;
    if (strcasecmp(name, "PlantsVsZombies") == 0 ||
        strcasecmp(name, "Plants vs. Zombies") == 0 ||
        strcasecmp(name, "PvZ") == 0) return &plantsvszombies_profile;
    if (strcasecmp(name, "Chuzzle") == 0 ||
        strcasecmp(name, "Chuzzle Deluxe") == 0) return &chuzzle_profile;
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

/*
 * Valve's Mach-O DRM wrapper (Steam's copy of the game) keeps the original
 * load commands and entry stub but encrypts __TEXT,__text and appends its
 * decryption stub to an enlarged, executable __LINKEDIT.  The encrypted image
 * cannot be run directly; it must first be unwrapped (see steam_unwrap.c /
 * LP32_UNWRAP_STEAM), which tools/build.py does automatically.  The stub
 * carries its own source paths, which is the cheapest reliable signature.
 */
static bool steam_drm_wrapped(const struct macho_image32 *image)
{
    static const char marker[] = "/src/drm/mach-o/";
    const void *start = (const void *)(uintptr_t)image->min_address;
    size_t length = image->max_address - image->min_address;
    return memmem(start, length, marker, sizeof(marker) - 1) != NULL;
}

static const struct lp32_game_profile *detect_profile(
    const struct macho_image32 *image)
{
    for (size_t i = 0; i < sizeof(known_profiles) / sizeof(known_profiles[0]); ++i) {
        if (image->entry_eip == known_profiles[i]->entry_eip &&
            image->max_address == known_profiles[i]->image_end) {
            return known_profiles[i];
        }
    }
    return NULL;
}

int lp32_profile_select(const struct macho_image32 *image)
{
    const char *override = getenv("LP32_GAME");
    if (override && override[0]) {
        const struct lp32_game_profile *forced = lp32_profile_named(override);
        if (!forced) {
            fprintf(stderr, "game_loader: unknown LP32_GAME profile: %s\n", override);
            return -1;
        }
        if (image->entry_eip != forced->entry_eip ||
            image->max_address != forced->image_end) {
            fprintf(stderr,
                    "compat32: warning: image (entry=0x%08x end=0x%08x) does not "
                    "match forced profile %s (entry=0x%08x end=0x%08x)\n",
                    image->entry_eip, image->max_address, forced->name,
                    forced->entry_eip, forced->image_end);
        }
        current_profile = forced;
    } else {
        const struct lp32_game_profile *detected = detect_profile(image);
        if (!detected) {
            if (steam_drm_wrapped(image)) {
                fprintf(stderr,
                        "game_loader: the game image is Steam's DRM-protected "
                        "executable and must be unwrapped before it can run.  Set "
                        "LP32_UNWRAP_STEAM to recover a clean image, or reinstall "
                        "with tools/build.py, which unwraps the Steam copy "
                        "automatically (entry=0x%08x end=0x%08x).\n",
                        image->entry_eip, image->max_address);
                return -1;
            }
            fprintf(stderr,
                    "game_loader: unrecognised image (entry=0x%08x end=0x%08x)\n",
                    image->entry_eip, image->max_address);
            return -1;
        }
        current_profile = detected;
    }
    fprintf(stderr, "compat32: title profile %s (%s)\n",
            current_profile->name, current_profile->display_name);
    return 0;
}
