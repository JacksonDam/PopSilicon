#ifndef LP32_GAME_PROFILE_H
#define LP32_GAME_PROFILE_H

/*
 * Per-game knowledge. Fields set to zero/NULL mean that Peggle does not need
 * that optional compatibility hook.
 */

#include <stddef.h>
#include <stdint.h>

struct macho_image32;

enum lp32_title {
    LP32_TITLE_UNKNOWN = 0,
    LP32_TITLE_PEGGLE,
    LP32_TITLE_PEGGLE_NIGHTS,
};

/* Both shipping titles are the same PopCap "Sexy" engine, so the compatibility
   bridges apply to either.  Only the neutral unknown profile is excluded. */
static inline int lp32_title_is_peggle_engine(enum lp32_title title)
{
    return title == LP32_TITLE_PEGGLE || title == LP32_TITLE_PEGGLE_NIGHTS;
}

/* Splash-dismiss repeat latch (see game_loader.c). */
struct lp32_startup_latch_patch {
    uint32_t hook;
    uint32_t state_true;
    uint32_t original_frontend;
    uint32_t function_epilogue;
    uint32_t state_pointer;
};

/* A short code/data signature checked before a guest patch is applied. */
struct lp32_code_signature {
    uint32_t address;
    uint8_t expected[8];
    uint8_t length;
};

/*
 * Optional save-worker de-duplication patch.
 */
struct lp32_save_worker_patch {
    struct lp32_code_signature ctor_running_flag; /* mov byte [reg+d8], 0 */
    uint32_t start_call;        /* call <thread starter> in the start method */
    uint32_t thread_starter;    /* target of that call (spawns the thread) */
};

/*
 * Optional texture-stage binder NULL guard.
 */
struct lp32_texture_bind_guard {
    struct lp32_code_signature load; /* mov ecx, [eax+14h]; test ecx, ecx */
    uint32_t resume;                 /* the jnz that follows the test */
};

/* Display/frontend globals consulted by objc_bridge.m. */
struct lp32_display_layout {
    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t refresh_rate;
    uint32_t renderer_display_slot;
    uint32_t legacy_renderer_display;
    uint32_t frontend_state_pointer;
    uint32_t frontend_page_slot;
    uint32_t frontend_gate_slot;
    uint32_t frontend_active_word;
};

/* Fixed-size render object pool replenished by compat_runtime.c. */
struct lp32_render_pool {
    uint32_t free_count;
    uint32_t free_head;
    uint32_t mutex;
    uint32_t return_address;
    uint32_t object_size;
    uint32_t object_count;
};

/* Optional activation window delegate implemented in the guest. */
struct lp32_activator_layout {
    uint32_t app_delegate_isa;
    uint32_t awake_from_nib;
    uint32_t did_finish_launching;
    uint32_t text_did_change;
    uint32_t cancel;
    uint32_t activate_manually;
    uint32_t activate_online;
};

struct lp32_game_profile {
    enum lp32_title title;
    const char *name;               /* project identifier */
    const char *display_name;
    const char *log_directory;      /* under ~/Library/Logs */
    const char *image_file;         /* Contents/SharedSupport/<image_file> */
    uint32_t entry_eip;             /* detection key */
    uint32_t image_end;             /* detection key (max_address) */
    uint32_t main_address;          /* 0 = derive from the crt start stub */
    const struct lp32_startup_latch_patch *startup_latch;
    const struct lp32_save_worker_patch *save_worker;  /* NULL = single init */
    const struct lp32_texture_bind_guard *texture_bind_guard; /* NULL = none */
    const struct lp32_display_layout *display;
    const struct lp32_render_pool *render_pool;
    const struct lp32_activator_layout *activator;
    /* NuSound streamer render callback (diagnostics only: lets the audio
       bridge read the stream's refill-request ring). 0 = unknown. */
    uint32_t stream_input_callback;
};

/* Chooses the profile for a loaded image (LP32_GAME overrides detection).
   Returns -1 when the image is not a known title. */
int lp32_profile_select(const struct macho_image32 *image);

/* Always non-NULL after lp32_profile_select; before that a neutral profile
   with no patches is returned. */
const struct lp32_game_profile *lp32_profile(void);

/* Profile used when a bundle name is all we have. */
const struct lp32_game_profile *lp32_profile_named(const char *name);

/* All shipping title profiles, in detection order (never NULL). */
const struct lp32_game_profile *const *lp32_known_profiles(size_t *count);

/* Finds `main` from the crt `start` stub (the call immediately before hlt). */
uint32_t lp32_profile_main_address(const struct macho_image32 *image);

#endif
