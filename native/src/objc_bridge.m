#include "objc_bridge.h"
#include "arb_program_guard.h"
#include "audio_bridge.h"
#include "arb_sampler_usage.h"
#include "compat_runtime.h"
#include "game_profile.h"
#include "name_match.h"

#define GL_SILENCE_DEPRECATION 1
#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Carbon/Carbon.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#import <OpenGL/glext.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <malloc/malloc.h>

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach/mach_time.h>

/* These GL 3.0 access flags are absent from Apple's legacy <OpenGL/gl.h>. */
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT 0x0004
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_MAP_FLUSH_EXPLICIT_BIT 0x0010
#endif

enum {
    kProxyBase = 0x7f018000,
    kProxyLimit = 0x7f020000,
    kProxyStride = 16,
    kProxyCapacity = (kProxyLimit - kProxyBase) / kProxyStride,
    /*
     * NSEvent and values derived from an event (most notably -characters)
     * have event-loop lifetime.  Giving every one a permanent bridge proxy
     * exhausted kProxyCapacity after a few thousand key-repeat events.  Keep
     * those short-lived objects in a separate pool which is replaced when
     * AppKit delivers the next event.  This address range is an opaque token
     * range in the gap between the guest heap and its thread stacks.
     */
    kEventProxyBase = 0x71000000,
    kEventProxyCapacity = 64,
};

struct proxy_entry {
    uint32_t handle;
    id object;
};

static struct proxy_entry proxies[kProxyCapacity];
static uint32_t proxy_count;
static struct proxy_entry event_proxies[kEventProxyCapacity];
static uint32_t event_proxy_count;
static NSEvent *current_proxy_event;
static bool logged_proxy_exhaustion;
static uint64_t objc_bridge_swap_count;

/*
 * O(1) object -> proxy-index map (open addressing, append-only).  The
 * persistent proxy pool can fill with long-lived objects (e.g. a title that
 * hands out thousands of NSNotification objects during load), after which a
 * linear scan of all kProxyCapacity entries ran on every bridge call and
 * showed up as steady per-frame CPU.  Keyed by the host object pointer; each
 * slot stores index+1 (0 = empty).  Never deletes, so a present key is always
 * reached before an empty slot; register_proxy_with_handle's in-place edits
 * only add keys (stale ones are rejected by the proxies[] re-check).
 */
enum { kProxyHashSize = 1u << 13, kProxyHashMask = kProxyHashSize - 1 };
static uint32_t proxy_object_hash[kProxyHashSize];

static inline uint32_t proxy_hash_pointer(const void *pointer)
{
    uintptr_t value = (uintptr_t)pointer >> 4;
    return (uint32_t)((value * 2654435761u) & kProxyHashMask);
}

static void proxy_object_hash_insert(uint32_t index)
{
    uint32_t slot = proxy_hash_pointer((const void *)proxies[index].object);
    for (uint32_t probe = 0; probe < kProxyHashSize; ++probe) {
        if (proxy_object_hash[slot] == 0) {
            proxy_object_hash[slot] = index + 1;
            return;
        }
        slot = (slot + 1) & kProxyHashMask;
    }
    /* Table saturated: leave it; proxy_for_object falls back to appending. */
}

static int32_t proxy_object_hash_find(id object)
{
    uint32_t slot = proxy_hash_pointer((const void *)object);
    for (uint32_t probe = 0; probe < kProxyHashSize; ++probe) {
        uint32_t stored = proxy_object_hash[slot];
        if (stored == 0) return -1;
        if (proxies[stored - 1].object == object) return (int32_t)(stored - 1);
        slot = (slot + 1) & kProxyHashMask;
    }
    return -1;
}

/* Section ranges of the loaded guest image (class-name refs, CF constants). */
#define bridge_image (compat_runtime32_image())

/* Guest-defined Objective-C class synthesis (objc_guest_class.inc, included
   below).  Forward-declared so object_for_receiver / proxy_for_object can map
   a synthesized instance's guest block to and from its host companion. */
id guest_companion_for_block(uint32_t block);
uint32_t guest_block_for_companion(id obj);
static bool guest_objc_msgsend(const uint32_t *arguments, uint32_t avail, uint64_t *out);
static bool guest_objc_msgsend_stret(const uint32_t *arguments, uint32_t avail, uint64_t *out);
bool guest_objc_super_init(uint32_t receiver, const char *selector,
                           const uint32_t *init_args, uint64_t *out);
static GLuint trace_vertex_program;
static GLuint trace_fragment_program;

struct gl_frame_diagnostics {
    uint64_t draw_calls;
    uint64_t clear_calls;
    uint64_t framebuffer_binds;
    uint64_t framebuffer_attachments;
    uint64_t framebuffer_blits;
    uint64_t texture_binds;
    uint64_t texture_uploads;
    uint64_t program_binds;
    uint64_t parameter_uploads;
};

static struct gl_frame_diagnostics gl_frame_diagnostics;
static volatile sig_atomic_t pending_gl_trace_arm;
static FILE *gl_trace_file;
static bool gl_trace_output_initialized;
static char gl_program_dump_directory[PATH_MAX];

static void arm_pending_gl_trace(void);
static void trace_gl_frame_boundary(void);
static void present_frame_with_stats(NSOpenGLContext *context,
                                     NSWindow *window, bool pace);

/*
 * Finder launches discard stderr.  Rendering diagnostics therefore need a
 * file of their own; trying to redirect a live process with a debugger proved
 * both fragile and intrusive.  This file is deliberately separate from the
 * async-signal-safe crash log maintained by game_loader.c.
 */
static void initialize_gl_trace_output(void)
{
    if (gl_trace_output_initialized) return;
    gl_trace_output_initialized = true;

    const char *home = getenv("HOME");
    if (!home || !home[0]) return;

    char directory[PATH_MAX];
    int length = snprintf(directory, sizeof(directory),
                          "%s/Library/Logs/%s", home,
                          lp32_profile()->log_directory);
    if (length < 0 || (size_t)length >= sizeof(directory)) return;
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) return;

    char path[PATH_MAX];
    length = snprintf(path, sizeof(path), "%s/gl-render.log", directory);
    if (length < 0 || (size_t)length >= sizeof(path)) return;
    /* Preserve earlier clean/bad captures across diagnostic self-tests and
     * relaunches; each session and armed segment already has a marker. */
    gl_trace_file = fopen(path, "a");
    if (gl_trace_file) {
        setvbuf(gl_trace_file, NULL, _IOLBF, 0);
        fprintf(gl_trace_file,
                "compat32: GL diagnostic session pid=%ld\n", (long)getpid());
    }

    length = snprintf(gl_program_dump_directory,
                      sizeof(gl_program_dump_directory),
                      "%s/gl-programs", directory);
    if (length < 0 || (size_t)length >= sizeof(gl_program_dump_directory)) {
        gl_program_dump_directory[0] = '\0';
    }
}

static void gl_trace_printf(const char *format, ...)
{
    initialize_gl_trace_output();
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    if (gl_trace_file) {
        vfprintf(gl_trace_file, format, copy);
        fflush(gl_trace_file);
    }
    va_end(copy);
}

/*
 * A few Cg-generated material programs always execute optional texture
 * lookups even when the engine leaves that sampler bound to texture object 0.
 * The trace can identify Cg-generated programs which execute optional texture
 * lookups while the corresponding unit is bound to object 0.  An off-screen
 * probe confirms that Apple's current driver returns the specified opaque-
 * black incomplete-texture value, so explicit substitution is diagnostic and
 * opt-in rather than a normal compatibility repair.
 */
enum {
    kSamplerProgramCapacity = 4096,
    kSamplerFallbackShareCapacity = 32,
    kSamplerFallbackUnitCapacity = 32,
};

struct sampler_program_entry {
    CGLShareGroupObj share_group;
    GLuint program;
    struct arb_sampler_usage usage;
    unsigned char state; /* 0 empty, 1 occupied, 2 tombstone */
};

struct sampler_fallback_share_entry {
    CGLShareGroupObj share_group;
    GLuint texture_2d;
    bool occupied;
};

struct sampler_fallback_restore {
    GLint active_texture;
    uint32_t changed_2d;
};

static struct sampler_program_entry
    sampler_programs[kSamplerProgramCapacity];
static struct sampler_fallback_share_entry
    sampler_fallback_shares[kSamplerFallbackShareCapacity];
static pthread_mutex_t sampler_program_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sampler_fallback_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int sampler_fallback_reports;

static size_t sampler_program_slot(CGLShareGroupObj share_group,
                                   GLuint program)
{
    return ((size_t)program * 2654435761u +
            ((uintptr_t)share_group >> 4)) % kSamplerProgramCapacity;
}

static void remember_fragment_program_samplers(GLuint program,
                                               const void *source,
                                               size_t source_size)
{
    if (!program || !source) return;
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    struct arb_sampler_usage usage;
    arb_sampler_usage_parse(source, source_size, &usage);

    pthread_mutex_lock(&sampler_program_mutex);
    size_t start = sampler_program_slot(share_group, program);
    size_t tombstone = SIZE_MAX;
    for (size_t probe = 0; probe < kSamplerProgramCapacity; ++probe) {
        size_t slot = (start + probe) % kSamplerProgramCapacity;
        struct sampler_program_entry *entry = &sampler_programs[slot];
        if (entry->state == 2 && tombstone == SIZE_MAX) tombstone = slot;
        if (entry->state == 1 && entry->share_group == share_group &&
            entry->program == program) {
            entry->usage = usage;
            pthread_mutex_unlock(&sampler_program_mutex);
            return;
        }
        if (entry->state != 0) continue;
        if (tombstone != SIZE_MAX) entry = &sampler_programs[tombstone];
        entry->share_group = share_group;
        entry->program = program;
        entry->usage = usage;
        entry->state = 1;
        pthread_mutex_unlock(&sampler_program_mutex);
        return;
    }
    pthread_mutex_unlock(&sampler_program_mutex);
    gl_trace_printf("compat32: fragment sampler table exhausted\n");
}

static bool fragment_program_sampler_usage(
    GLuint program, struct arb_sampler_usage *usage)
{
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    bool found = false;
    memset(usage, 0, sizeof(*usage));
    pthread_mutex_lock(&sampler_program_mutex);
    size_t start = sampler_program_slot(share_group, program);
    for (size_t probe = 0; probe < kSamplerProgramCapacity; ++probe) {
        struct sampler_program_entry *entry =
            &sampler_programs[(start + probe) % kSamplerProgramCapacity];
        if (entry->state == 0) break;
        if (entry->state == 1 && entry->share_group == share_group &&
            entry->program == program) {
            *usage = entry->usage;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&sampler_program_mutex);
    return found;
}

static void forget_fragment_program_samplers(GLsizei count,
                                             const GLuint *programs)
{
    if (count <= 0 || !programs) return;
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    pthread_mutex_lock(&sampler_program_mutex);
    for (size_t slot = 0; slot < kSamplerProgramCapacity; ++slot) {
        struct sampler_program_entry *entry = &sampler_programs[slot];
        if (entry->state != 1 || entry->share_group != share_group) continue;
        for (GLsizei index = 0; index < count; ++index) {
            if (entry->program == programs[index]) {
                memset(&entry->usage, 0, sizeof(entry->usage));
                entry->state = 2;
                break;
            }
        }
    }
    pthread_mutex_unlock(&sampler_program_mutex);
}

static GLuint sampler_fallback_texture_2d(void)
{
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    pthread_mutex_lock(&sampler_fallback_mutex);
    struct sampler_fallback_share_entry *available = NULL;
    for (size_t index = 0; index < kSamplerFallbackShareCapacity; ++index) {
        struct sampler_fallback_share_entry *entry =
            &sampler_fallback_shares[index];
        if (entry->occupied && entry->share_group == share_group) {
            GLuint texture = entry->texture_2d;
            pthread_mutex_unlock(&sampler_fallback_mutex);
            return texture;
        }
        if (!entry->occupied && !available) available = entry;
    }
    if (!available) {
        pthread_mutex_unlock(&sampler_fallback_mutex);
        return 0;
    }

    GLint previous_texture = 0;
    const GLubyte black_opaque[4] = {0, 0, 0, 255};
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, black_opaque);
        glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
        available->share_group = share_group;
        available->texture_2d = texture;
        available->occupied = true;
    }
    pthread_mutex_unlock(&sampler_fallback_mutex);
    return texture;
}

static bool repair_unbound_fragment_samplers(
    struct sampler_fallback_restore *restore)
{
    memset(restore, 0, sizeof(*restore));
    static int repair_enabled = -1;
    if (repair_enabled < 0) {
        repair_enabled = getenv("LP32_ENABLE_EXPLICIT_UNBOUND_SAMPLER") != NULL &&
                         getenv("LP32_DISABLE_UNBOUND_SAMPLER_REPAIR") == NULL;
    }
    if (!repair_enabled || !glIsEnabled(GL_FRAGMENT_PROGRAM_ARB)) {
        return false;
    }

    GLint program = (GLint)trace_fragment_program;
    struct arb_sampler_usage usage;
    if (program <= 0 ||
        !fragment_program_sampler_usage((GLuint)program, &usage) ||
        !usage.texture_2d) {
        return false;
    }

    GLint maximum_units = 0;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maximum_units);
    if (maximum_units < 0) maximum_units = 0;
    if (maximum_units > kSamplerFallbackUnitCapacity) {
        maximum_units = kSamplerFallbackUnitCapacity;
    }
    glGetIntegerv(GL_ACTIVE_TEXTURE, &restore->active_texture);
    for (GLint unit = 0; unit < maximum_units; ++unit) {
        uint32_t bit = UINT32_C(1) << unit;
        if (!(usage.texture_2d & bit)) continue;
        glActiveTexture((GLenum)(GL_TEXTURE0 + unit));
        GLint binding = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        if (binding) continue;
        GLuint fallback = sampler_fallback_texture_2d();
        if (!fallback) continue;
        glBindTexture(GL_TEXTURE_2D, fallback);
        restore->changed_2d |= bit;
        if (sampler_fallback_reports < 32) {
            gl_trace_printf(
                "compat32: repaired unbound ARB sampler fp=%d unit=%d "
                "target=2D fallback=%u swap=%llu\n",
                program, unit, fallback,
                (unsigned long long)objc_bridge_swap_count);
            ++sampler_fallback_reports;
        }
    }
    glActiveTexture((GLenum)restore->active_texture);
    return restore->changed_2d != 0;
}

static void restore_unbound_fragment_samplers(
    const struct sampler_fallback_restore *restore)
{
    if (!restore->changed_2d) return;
    for (unsigned int unit = 0; unit < kSamplerFallbackUnitCapacity; ++unit) {
        if (!(restore->changed_2d & (UINT32_C(1) << unit))) continue;
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture((GLenum)restore->active_texture);
}

static id object_for_receiver(uint32_t receiver);
static id peggle_object_for_handle(uint32_t handle);

static bool proxy_object_is_retained(id object)
{
    return object && ![object isKindOfClass:[NSAutoreleasePool class]];
}

static void reset_event_proxies(void)
{
    for (uint32_t index = 0; index < event_proxy_count; ++index) {
        if (proxy_object_is_retained(event_proxies[index].object)) {
            [event_proxies[index].object release];
        }
        event_proxies[index].handle = 0;
        event_proxies[index].object = nil;
    }
    event_proxy_count = 0;
    current_proxy_event = nil;
}

static bool is_event_proxy_object(id object)
{
    if (!object) return false;
    for (uint32_t index = 0; index < event_proxy_count; ++index) {
        if (event_proxies[index].object == object) return true;
    }
    return false;
}

static uint32_t event_proxy_for_object(id object)
{
    if (!object) return 0;
    if ([object isKindOfClass:[NSEvent class]] && object != current_proxy_event) {
        reset_event_proxies();
        current_proxy_event = (NSEvent *)object;
    }
    for (uint32_t index = 0; index < event_proxy_count; ++index) {
        if (event_proxies[index].object == object) {
            return event_proxies[index].handle;
        }
    }
    if (event_proxy_count >= kEventProxyCapacity) {
        fprintf(stderr,
                "compat32: event-scoped Objective-C proxy pool exhausted "
                "at swap %llu for %s\n",
                (unsigned long long)objc_bridge_swap_count,
                class_getName(object_getClass(object)));
        return 0;
    }
    uint32_t handle = kEventProxyBase + event_proxy_count * kProxyStride;
    event_proxies[event_proxy_count].handle = handle;
    event_proxies[event_proxy_count].object = object;
    if (proxy_object_is_retained(object)) [object retain];
    ++event_proxy_count;
    return handle;
}

static uint32_t proxy_for_object(id object)
{
    if (!object) return 0;
    /* A synthesized guest-class companion maps back to its guest block so the
       guest keeps a single stable identity for the object. */
    uint32_t guest_block = guest_block_for_companion(object);
    if (guest_block) return guest_block;
    if ([object isKindOfClass:[NSEvent class]]) {
        return event_proxy_for_object(object);
    }
    int32_t existing = proxy_object_hash_find(object);
    if (existing >= 0) return proxies[existing].handle;
    if (proxy_count >= kProxyCapacity) {
        if (!logged_proxy_exhaustion) {
            fprintf(stderr,
                    "compat32: persistent Objective-C proxy pool exhausted "
                    "at swap %llu for %s\n",
                    (unsigned long long)objc_bridge_swap_count,
                    class_getName(object_getClass(object)));
            logged_proxy_exhaustion = true;
        }
        return 0;
    }
    uint32_t handle = kProxyBase + proxy_count * kProxyStride;
    proxies[proxy_count].handle = handle;
    proxies[proxy_count].object = object;
    if (proxy_object_is_retained(object)) [object retain];
    proxy_object_hash_insert(proxy_count);
    ++proxy_count;
    return handle;
}

static uint32_t proxy_for_returned_object(id receiver, id object)
{
    return is_event_proxy_object(receiver) ? event_proxy_for_object(object) :
                                             proxy_for_object(object);
}

int objc_bridge32_run_proxy_self_test(void)
{
    @autoreleasepool {
        NSString *persistent = [[[NSString alloc]
            initWithUTF8String:"persistent-proxy"] autorelease];
        uint32_t persistent_handle = proxy_for_object(persistent);
        if (!persistent_handle || object_for_receiver(persistent_handle) != persistent) {
            fputs("compat32: Objective-C proxy self-test could not create "
                  "persistent handle\n", stderr);
            return -1;
        }

        /* Exercise the real NSInvocation dispatch boundary.  Without the
           per-call autorelease pool this loop retains about 6 MiB of
           NSInvocation objects and return buffers in the outer pool. */
        uint32_t selector = compat_runtime32_copy_cstring("length");
        if (!selector) {
            fputs("compat32: Objective-C proxy self-test could not allocate "
                  "selector\n", stderr);
            return -1;
        }
        const uint32_t message_arguments[] = {persistent_handle, selector};
        malloc_statistics_t before = {0};
        malloc_statistics_t after = {0};
        malloc_zone_statistics(malloc_default_zone(), &before);
        for (uint32_t iteration = 0; iteration < 10000; ++iteration) {
            uint64_t result = 0;
            if (!objc_bridge32_dispatch("_objc_msgSend", message_arguments,
                                        &result) ||
                result != [persistent length]) {
                fprintf(stderr,
                        "compat32: Objective-C invocation self-test failed "
                        "at message %u\n", iteration);
                compat_runtime32_deallocate(selector);
                return -1;
            }
        }
        malloc_zone_statistics(malloc_default_zone(), &after);
        compat_runtime32_deallocate(selector);
        size_t invocation_growth = after.size_in_use > before.size_in_use ?
            after.size_in_use - before.size_in_use : 0;
        if (invocation_growth > 2 * 1024 * 1024) {
            fprintf(stderr,
                    "compat32: Objective-C invocation self-test retained "
                    "%zu bytes\n", invocation_growth);
            return -1;
        }

        for (uint32_t iteration = 0; iteration < 10000; ++iteration) {
            @autoreleasepool {
                NSString *characters = (iteration & 1) ? @"w" : @"s";
                NSEvent *event = [NSEvent
                    keyEventWithType:(iteration & 1) ? NSEventTypeKeyDown :
                                                          NSEventTypeKeyUp
                             location:NSZeroPoint
                        modifierFlags:0
                            timestamp:(NSTimeInterval)iteration / 1000.0
                         windowNumber:0
                              context:nil
                           characters:characters
          charactersIgnoringModifiers:characters
                            isARepeat:NO
                              keyCode:(iteration & 1) ? 13 : 1];
                uint32_t event_handle = proxy_for_object(event);
                uint32_t characters_handle =
                    proxy_for_returned_object(event, [event characters]);
                if (!event_handle || !characters_handle ||
                    object_for_receiver(event_handle) != event ||
                    ![object_for_receiver(characters_handle)
                        isEqualToString:characters]) {
                    fprintf(stderr,
                            "compat32: Objective-C proxy self-test failed at "
                            "event %u\n", iteration);
                    return -1;
                }
            }
        }

        if (proxy_count != 1 ||
            object_for_receiver(persistent_handle) != persistent) {
            fprintf(stderr,
                    "compat32: Objective-C proxy self-test leaked event "
                    "objects into persistent table (persistent=%u event=%u)\n",
                    proxy_count, event_proxy_count);
            return -1;
        }
        reset_event_proxies();
        printf("Objective-C proxy self-test: PASS "
               "(10000 messages, +%zu bytes; 10000 events, persistent=%u, "
               "event=%u)\n", invocation_growth, proxy_count,
               event_proxy_count);
        return 0;
    }
}

static uint32_t register_proxy_with_handle(id object, uint32_t handle)
{
    if (!object || !handle) return 0;
    for (uint32_t index = 0; index < proxy_count; ++index) {
        if (proxies[index].object == object || proxies[index].handle == handle) {
            proxies[index].object = object;
            proxies[index].handle = handle;
            proxy_object_hash_insert(index);
            return handle;
        }
    }
    if (proxy_count >= kProxyCapacity) {
        if (!logged_proxy_exhaustion) {
            fprintf(stderr,
                    "compat32: persistent Objective-C proxy pool exhausted "
                    "while registering guest handle 0x%08x\n", handle);
            logged_proxy_exhaustion = true;
        }
        return 0;
    }
    proxies[proxy_count].handle = handle;
    proxies[proxy_count].object = object;
    if (proxy_object_is_retained(object)) [object retain];
    proxy_object_hash_insert(proxy_count);
    ++proxy_count;
    return handle;
}

static id object_for_receiver(uint32_t receiver)
{
    if (!receiver) return nil;
    /* A synthesized guest-class instance resolves to its host companion so
       host Cocoa and inherited-method dispatch see a real host object. */
    id companion = guest_companion_for_block(receiver);
    if (companion) return companion;
    for (uint32_t index = 0; index < event_proxy_count; ++index) {
        if (event_proxies[index].handle == receiver) {
            return event_proxies[index].object;
        }
    }
    /* proxy_for_object hands out sequential kProxyBase-relative handles, so the
       common reverse lookup is O(1) arithmetic instead of a scan of the full
       pool.  Handles registered with an arbitrary value (register_proxy_with_
       handle, used once for the activation delegate) fall outside this range
       and take the linear path. */
    if (receiver >= kProxyBase && receiver < kProxyLimit) {
        uint32_t index = (receiver - kProxyBase) / kProxyStride;
        if (index < proxy_count && proxies[index].handle == receiver) {
            return proxies[index].object;
        }
    } else {
        for (uint32_t index = 0; index < proxy_count; ++index) {
            if (proxies[index].handle == receiver) {
                return proxies[index].object;
            }
        }
    }

    id peggle_object = peggle_object_for_handle(receiver);
    if (peggle_object) return peggle_object;
    /* Old 32-bit CF constant strings are four-word records in __DATA. */
    if (receiver >= bridge_image->cfstring_start &&
        receiver < bridge_image->cfstring_end) {
        const uint32_t *constant = (const void *)(uintptr_t)receiver;
        uint32_t bytes_address = constant[2];
        uint32_t length = constant[3];
        if (bytes_address >= UINT32_C(0x00001000) &&
            bytes_address < bridge_image->max_address && length < UINT32_C(0x100000)) {
            NSString *string = [[[NSString alloc]
                initWithBytes:(const void *)(uintptr_t)bytes_address
                       length:length
                     encoding:NSUTF8StringEncoding] autorelease];
            if (!string) {
                string = [[[NSString alloc]
                    initWithBytes:(const void *)(uintptr_t)bytes_address
                           length:length
                         encoding:NSISOLatin1StringEncoding] autorelease];
            }
            return string;
        }
    }

    /* Legacy class references contain the class name rather than a Class. */
    if (receiver >= bridge_image->cstring_start &&
        receiver < bridge_image->cstring_end) {
        const char *class_name = (const char *)(uintptr_t)receiver;
        return (id)objc_getClass(class_name);
    }
    return nil;
}

static const char *skip_type_qualifiers(const char *type)
{
    while (*type && strchr("rnNoORV", *type)) ++type;
    return type;
}

static id object_for_argument(uint32_t value)
{
    return object_for_receiver(value);
}

static NSBundle *legacy_game_bundle(void)
{
    static NSBundle *bundle;
    if (!bundle) {
        bundle = [[NSBundle mainBundle] retain];
    }
    return bundle;
}

static size_t guest_words_for_type(const char *type)
{
    type = skip_type_qualifiers(type);
    switch (*type) {
        /* NSInteger/NSUInteger are 32-bit in the guest even though the host
           method signature encodes them as q/Q.  Explicit LongLong selectors
           are handled separately by the invocation bridge. */
        case 'q': case 'Q': return 1;
        case 'd': return 2;
        case '{': {
            /* The legacy ABI uses 32-bit CGFloat, so count encoded floats. */
            size_t words = 0;
            for (const char *cursor = type; *cursor; ++cursor) {
                if (*cursor == 'f' || *cursor == 'i' || *cursor == 'I') ++words;
                if (*cursor == '}') break;
            }
            return words ? words : 1;
        }
        default: return 1;
    }
}

static bool invoke_simple_message(id receiver, const char *selector_name,
                                  const uint32_t *guest_arguments,
                                  uint64_t *guest_result)
{
    SEL selector = sel_registerName(selector_name);
    if (![receiver respondsToSelector:selector]) return false;
    NSMethodSignature *signature = [receiver methodSignatureForSelector:selector];
    if (!signature) return false;

    NSInvocation *invocation = [NSInvocation invocationWithMethodSignature:signature];
    [invocation setTarget:receiver];
    [invocation setSelector:selector];

    size_t word = 2;
    bool explicit_long_long = strstr(selector_name, "LongLong") != NULL;
    for (NSUInteger index = 2; index < [signature numberOfArguments]; ++index) {
        const char *type = skip_type_qualifiers([signature getArgumentTypeAtIndex:index]);
        switch (*type) {
            case '@': case '#': {
                id value = object_for_argument(guest_arguments[word]);
                [invocation setArgument:&value atIndex:index];
                break;
            }
            case ':': {
                const char *name = (const char *)(uintptr_t)guest_arguments[word];
                SEL value = name ? sel_registerName(name) : (SEL)0;
                [invocation setArgument:&value atIndex:index];
                break;
            }
            case '*': {
                char *value = (char *)(uintptr_t)guest_arguments[word];
                [invocation setArgument:&value atIndex:index];
                break;
            }
            case '^': {
                /* Opaque-pointer args (e.g. a CGImageRef for
                   -[NSBitmapImageRep initWithCGImage:]) arrive as CF handles
                   into pg_cf_table; unwrap them to the real host object.  A
                   genuine guest buffer pointer is not a handle and passes
                   through identity-mapped. */
                id handle_object = peggle_object_for_handle(guest_arguments[word]);
                void *value = handle_object
                    ? (void *)handle_object
                    : (void *)(uintptr_t)guest_arguments[word];
                [invocation setArgument:&value atIndex:index];
                break;
            }
            case 'c': { int8_t value = (int8_t)guest_arguments[word]; [invocation setArgument:&value atIndex:index]; break; }
            case 'C': case 'B': { uint8_t value = (uint8_t)guest_arguments[word]; [invocation setArgument:&value atIndex:index]; break; }
            case 's': { int16_t value = (int16_t)guest_arguments[word]; [invocation setArgument:&value atIndex:index]; break; }
            case 'S': { uint16_t value = (uint16_t)guest_arguments[word]; [invocation setArgument:&value atIndex:index]; break; }
            case 'i': case 'l': { int32_t value = (int32_t)guest_arguments[word]; [invocation setArgument:&value atIndex:index]; break; }
            case 'I': case 'L': { uint32_t value = guest_arguments[word]; [invocation setArgument:&value atIndex:index]; break; }
            case 'q': { int64_t value = (int32_t)guest_arguments[word]; if (explicit_long_long) memcpy(&value, guest_arguments + word, sizeof(value)); [invocation setArgument:&value atIndex:index]; break; }
            case 'Q': { uint64_t value = guest_arguments[word]; if (explicit_long_long) memcpy(&value, guest_arguments + word, sizeof(value)); [invocation setArgument:&value atIndex:index]; break; }
            case 'f': { float value; memcpy(&value, guest_arguments + word, sizeof(value)); [invocation setArgument:&value atIndex:index]; break; }
            case 'd': { double value; memcpy(&value, guest_arguments + word, sizeof(value)); [invocation setArgument:&value atIndex:index]; break; }
            case '{': {
                /* i386 AppKit geometry is float-based and passed by value on
                   the stack; widen to the host's CGFloat layout. */
                const float *fields = (const float *)(guest_arguments + word);
                if (strncmp(type, "{CGRect=", 8) == 0) {
                    NSRect value = NSMakeRect(fields[0], fields[1], fields[2], fields[3]);
                    [invocation setArgument:&value atIndex:index];
                    word += 4;
                } else if (strncmp(type, "{CGPoint=", 9) == 0) {
                    NSPoint value = NSMakePoint(fields[0], fields[1]);
                    [invocation setArgument:&value atIndex:index];
                    word += 2;
                } else if (strncmp(type, "{CGSize=", 8) == 0) {
                    NSSize value = NSMakeSize(fields[0], fields[1]);
                    [invocation setArgument:&value atIndex:index];
                    word += 2;
                } else {
                    return false;
                }
                continue;
            }
            default:
                return false;
        }
        word += ((*type == 'q' || *type == 'Q') && explicit_long_long) ?
                2 : guest_words_for_type(type);
    }

    @try {
        [invocation invoke];
    } @catch (NSException *exception) {
        fprintf(stderr, "compat32: host Objective-C message %s raised %s\n",
                selector_name, [[exception reason] UTF8String]);
        return false;
    }

    const char *return_type = skip_type_qualifiers([signature methodReturnType]);
    switch (*return_type) {
        case 'v': *guest_result = 0; return true;
        case '@': case '#': {
            id value = nil;
            [invocation getReturnValue:&value];
            *guest_result = proxy_for_returned_object(receiver, value);
            return true;
        }
        case ':': {
            SEL value = (SEL)0;
            [invocation getReturnValue:&value];
            *guest_result = value ? compat_runtime32_copy_cstring(sel_getName(value)) : 0;
            return true;
        }
        case '*': {
            const char *value = NULL;
            [invocation getReturnValue:&value];
            *guest_result = value ? compat_runtime32_copy_cstring(value) : 0;
            return true;
        }
        case '^': {
            void *value = NULL;
            [invocation getReturnValue:&value];
            uintptr_t pointer = (uintptr_t)value;
            if (pointer > UINT32_MAX) return false;
            *guest_result = (uint32_t)pointer;
            return true;
        }
        case 'c': { int8_t value = 0; [invocation getReturnValue:&value]; *guest_result = (uint32_t)(int32_t)value; return true; }
        case 'C': case 'B': { uint8_t value = 0; [invocation getReturnValue:&value]; *guest_result = value; return true; }
        case 's': { int16_t value = 0; [invocation getReturnValue:&value]; *guest_result = (uint32_t)(int32_t)value; return true; }
        case 'S': { uint16_t value = 0; [invocation getReturnValue:&value]; *guest_result = value; return true; }
        case 'i': case 'l': { int32_t value = 0; [invocation getReturnValue:&value]; *guest_result = (uint32_t)value; return true; }
        case 'I': case 'L': { uint32_t value = 0; [invocation getReturnValue:&value]; *guest_result = value; return true; }
        case 'q': case 'Q': { uint64_t value = 0; [invocation getReturnValue:&value]; *guest_result = value; return true; }
        case '{': {
            /* A struct <= 8 bytes (NSPoint/NSSize: two CGFloats) is returned
               in EDX:EAX on i386, not through the stret hidden pointer.  The
               host returns two doubles (16 bytes); narrow to two guest floats
               and pack them into the 64-bit result the gateway splits across
               EDX:EAX (first field low/EAX, second high/EDX). */
            if ([signature methodReturnLength] == 2 * sizeof(CGFloat)) {
                CGFloat pair[2] = {0, 0};
                [invocation getReturnValue:pair];
                float first = (float)pair[0], second = (float)pair[1];
                uint32_t low, high;
                memcpy(&low, &first, sizeof(low));
                memcpy(&high, &second, sizeof(high));
                *guest_result = (uint64_t)low | ((uint64_t)high << 32);
                return true;
            }
            return false;
        }
        default: return false;
    }
}

@interface NSFullScreenWindow : NSWindow
@end

@implementation NSFullScreenWindow
- (BOOL)canBecomeMainWindow { return YES; }
- (BOOL)canBecomeKeyWindow { return YES; }
@end

/*
 * The game runs on the main display, as the shipped build does.  AppKit orders
 * NSScreen objects with the menu-bar display first, but identify the primary
 * by display ID rather than relying on that ordering.  Test launches set
 * LP32_USE_SECONDARY_DISPLAY=1 to keep the game off the operator's workspace
 * when another display is connected; LP32_DISPLAY_INDEX selects an explicit
 * zero-based entry from [NSScreen screens].
 */
static CGDirectDisplayID display_id_for_screen(NSScreen *screen)
{
    NSNumber *number = screen ?
        [[screen deviceDescription] objectForKey:@"NSScreenNumber"] : nil;
    return number ? (CGDirectDisplayID)[number unsignedIntValue] : 0;
}

static NSScreen *preferred_game_screen(void)
{
    static bool resolved;
    static NSScreen *screen;
    if (resolved) return screen;

    NSArray *screens = [NSScreen screens];
    NSUInteger count = [screens count];
    CGDirectDisplayID primary_id = CGMainDisplayID();
    const char *index_text = getenv("LP32_DISPLAY_INDEX");
    if (index_text && index_text[0]) {
        char *end = NULL;
        unsigned long index = strtoul(index_text, &end, 10);
        if (end && *end == '\0' && index < count) {
            screen = [[screens objectAtIndex:(NSUInteger)index] retain];
        }
    }
    if (!screen && getenv("LP32_USE_SECONDARY_DISPLAY")) {
        for (NSScreen *candidate in screens) {
            CGDirectDisplayID candidate_id = display_id_for_screen(candidate);
            if (candidate_id && candidate_id != primary_id) {
                screen = [candidate retain];
                break;
            }
        }
    }
    if (!screen) {
        for (NSScreen *candidate in screens) {
            if (display_id_for_screen(candidate) == primary_id) {
                screen = [candidate retain];
                break;
            }
        }
    }
    if (!screen) screen = [[NSScreen mainScreen] retain];
    if (!screen && count) screen = [[screens objectAtIndex:0] retain];
    resolved = true;

    NSRect frame = screen ? [screen frame] : NSZeroRect;
    NSUInteger selected_index = screen ? [screens indexOfObjectIdenticalTo:screen] :
                                         NSNotFound;
    fprintf(stderr,
            "compat32: selected game display id=%u index=%ld/%lu "
            "frame=%.0f,%.0f %.0fx%.0f primary=%u%s\n",
            display_id_for_screen(screen),
            selected_index == NSNotFound ? -1L : (long)selected_index,
            (unsigned long)count, frame.origin.x, frame.origin.y,
            frame.size.width, frame.size.height, primary_id,
            display_id_for_screen(screen) == primary_id ? " (primary fallback)" :
                                                          " (secondary)");
    return screen;
}

static CGDirectDisplayID preferred_game_display_id(void)
{
    CGDirectDisplayID display = display_id_for_screen(preferred_game_screen());
    return display ? display : CGMainDisplayID();
}

/*
 * Emulated display mode switch. The legacy display path runs the game at the
 * resolution from pcconfig.txt (1024x768 unless changed in Options) by
 * capturing the display and switching its mode; the game then sizes its GL
 * view and viewport to that mode and lays the frame out for it.  Modern macOS
 * keeps the desktop resolution, so the bridge keeps the borderless window at
 * the display's real size and instead renders the requested mode into a
 * fixed-size backing surface (kCGLCPSurfaceBackingSize) that CGL scales to an
 * aspect-correct rectangle centred in the window.  While a mode is active the
 * legacy CG "current mode" and the NSScreen frame report it too, so every
 * dimension the guest sees agrees.  Zero width means no switch is in effect.
 */
static NSSize guest_display_mode;

static NSSize preferred_display_native_size(void)
{
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(preferred_game_display_id());
    if (!current) return [preferred_game_screen() frame].size;
    NSSize size = NSMakeSize((CGFloat)CGDisplayModeGetWidth(current),
                             (CGFloat)CGDisplayModeGetHeight(current));
    CFRelease(current);
    return size;
}

static bool guest_display_mode_active(void)
{
    return guest_display_mode.width > 0 && guest_display_mode.height > 0;
}

/* Largest rectangle with the guest mode's aspect ratio that fits in bounds,
   centred (letterboxed/pillarboxed against the window's black background). */
static NSRect guest_display_mode_rect(NSRect bounds)
{
    if (!guest_display_mode_active() || bounds.size.width <= 0 ||
        bounds.size.height <= 0) {
        return bounds;
    }
    CGFloat scale = fmin(bounds.size.width / guest_display_mode.width,
                         bounds.size.height / guest_display_mode.height);
    NSSize size = NSMakeSize(floor(guest_display_mode.width * scale),
                             floor(guest_display_mode.height * scale));
    return NSMakeRect(bounds.origin.x + floor((bounds.size.width - size.width) / 2),
                      bounds.origin.y + floor((bounds.size.height - size.height) / 2),
                      size.width, size.height);
}

static void set_guest_display_mode(NSSize requested)
{
    NSSize native = preferred_display_native_size();
    NSSize previous = guest_display_mode;
    if (requested.width <= 0 || requested.height <= 0 ||
        (requested.width == native.width && requested.height == native.height)) {
        guest_display_mode = NSZeroSize;
    } else {
        guest_display_mode = requested;
    }
    if (!NSEqualSizes(previous, guest_display_mode)) {
        fprintf(stderr,
                "compat32: guest display mode %.0fx%.0f on a %.0fx%.0f display -> %s\n",
                requested.width, requested.height, native.width, native.height,
                guest_display_mode_active() ? "scaled backing surface" : "native");
    }
}

/*
 * Test-only input bridge.  AppKit events must be created on the main thread,
 * so the async-signal-safe handlers merely publish a key code.  swapBuffers
 * consumes it on the game's main thread.  Nothing is installed unless the
 * launcher explicitly sets LP32_ENABLE_TEST_INPUT.
 */
static volatile sig_atomic_t pending_test_key_plus_one;
static bool test_input_wasd;

static void test_input_signal_handler(int signal_number)
{
    unsigned short key_code;
    switch (signal_number) {
        case SIGUSR1: key_code = 36; break;  /* Return */
        case SIGUSR2: key_code = 49; break;  /* Space */
        case SIGINFO: key_code = test_input_wasd ? 1 : 125; break; /* S/Down */
        case SIGHUP: key_code = test_input_wasd ? 13 : 126; break; /* W/Up */
        case SIGWINCH: key_code = 53; break; /* Escape */
        case SIGURG: key_code = test_input_wasd ? 0 : 123; break;  /* A/Left */
        case SIGALRM: key_code = test_input_wasd ? 2 : 124; break; /* D/Right */
        case SIGPROF: key_code = 51; break;  /* Backspace */
        case SIGXCPU: key_code = 32; break;  /* U (skip cutscene) */
        default: return;
    }
    pending_test_key_plus_one = (sig_atomic_t)key_code + 1;
}

static void install_test_input_signals(void)
{
    const int signals[] = {
        SIGUSR1, SIGUSR2, SIGINFO, SIGHUP, SIGWINCH, SIGURG, SIGALRM,
        SIGPROF, SIGXCPU,
    };
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = test_input_signal_handler;
    sigemptyset(&action.sa_mask);
    for (size_t index = 0; index < sizeof(signals) / sizeof(signals[0]); ++index) {
        sigaction(signals[index], &action, NULL);
    }
}

/*
 * Test-only scripted input.  LP32_TEST_KEY_FIFO names a FIFO (mkfifo) from
 * which swapBuffers reads one "keycode [hold_seconds]" line per frame while no
 * other test key is active.  Unlike the signal bridge this can express any
 * key code and a hold duration, which is what walking a character needs.
 */
static int test_key_fifo_fd = -2;
static char test_key_fifo_buffer[256];
static size_t test_key_fifo_length;

static int read_test_key_fifo(double *hold_seconds)
{
    if (test_key_fifo_fd == -2) {
        const char *path = getenv("LP32_TEST_KEY_FIFO");
        test_key_fifo_fd = path && path[0] ?
            open(path, O_RDONLY | O_NONBLOCK) : -1;
        if (path && path[0] && test_key_fifo_fd < 0) {
            perror("compat32: open LP32_TEST_KEY_FIFO");
        }
    }
    if (test_key_fifo_fd < 0) return -1;

    char *newline = memchr(test_key_fifo_buffer, '\n', test_key_fifo_length);
    if (!newline) {
        ssize_t count = read(test_key_fifo_fd,
                             test_key_fifo_buffer + test_key_fifo_length,
                             sizeof(test_key_fifo_buffer) - 1 -
                                 test_key_fifo_length);
        if (count > 0) test_key_fifo_length += (size_t)count;
        newline = memchr(test_key_fifo_buffer, '\n', test_key_fifo_length);
        if (!newline) {
            if (test_key_fifo_length == sizeof(test_key_fifo_buffer) - 1) {
                test_key_fifo_length = 0;
            }
            return -1;
        }
    }
    *newline = '\0';
    char *end = NULL;
    long key_code = strtol(test_key_fifo_buffer, &end, 10);
    double hold = end ? strtod(end, NULL) : 0.0;
    size_t consumed = (size_t)(newline + 1 - test_key_fifo_buffer);
    memmove(test_key_fifo_buffer, newline + 1,
            test_key_fifo_length - consumed);
    test_key_fifo_length -= consumed;
    if (end == test_key_fifo_buffer || key_code < 0 || key_code > 127) {
        return -1;
    }
    *hold_seconds = hold;
    return (int)key_code;
}

static void gl_trace_signal_handler(int signal_number)
{
    (void)signal_number;
    pending_gl_trace_arm = 1;
}

static void install_gl_trace_signal(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = gl_trace_signal_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGQUIT, &action, NULL);
}

static NSString *characters_for_test_key(unsigned short key_code)
{
    unichar character;
    switch (key_code) {
        case 36: return @"\r";
        case 49: return @" ";
        case 53: return @"\x1b";
        case 32: return @"u";
        case 1: return @"s";
        case 2: return @"d";
        case 13: return @"w";
        case 123: character = NSLeftArrowFunctionKey; break;
        case 124: character = NSRightArrowFunctionKey; break;
        case 125: character = NSDownArrowFunctionKey; break;
        case 126: character = NSUpArrowFunctionKey; break;
        case 0: return @"a";
        default: return @"";
    }
    return [NSString stringWithCharacters:&character length:1];
}

static void post_test_key_event(NSWindow *window, unsigned short key_code,
                                BOOL is_down)
{
    if (!window) return;
    NSString *characters = characters_for_test_key(key_code);
    NSTimeInterval timestamp = [[NSProcessInfo processInfo] systemUptime];
    NSEvent *event = [NSEvent
        keyEventWithType:is_down ? NSEventTypeKeyDown : NSEventTypeKeyUp
                 location:NSZeroPoint
            modifierFlags:0
                timestamp:timestamp
             windowNumber:[window windowNumber]
                  context:nil
               characters:characters
      charactersIgnoringModifiers:characters
                isARepeat:NO
                  keyCode:key_code];
    [NSApp postEvent:event atStart:NO];
}

/*
 * ScreenWidth, ScreenHeight, and ScreenRefreshRate are populated from
 * pcconfig.txt by the recovered setters at 0x252d40, 0x252d10, and 0x252c80.
 * A zero width/height is the old game's "current display" sentinel.  Modern
 * display APIs no longer turn that sentinel into a concrete menu selection,
 * so resolve it before the guest builds its cached Video Settings model.
 */
static void materialize_automatic_display_mode(void)
{
    const struct lp32_display_layout *display = lp32_profile()->display;
    if (!display || !display->screen_width) return;
    volatile int32_t *legacy_screen_width =
        (void *)(uintptr_t)display->screen_width;
    volatile int32_t *legacy_screen_height =
        (void *)(uintptr_t)display->screen_height;
    volatile int32_t *legacy_refresh_rate =
        (void *)(uintptr_t)display->refresh_rate;
    if (getenv("LP32_TRACE_DISPLAY")) {
        fprintf(stderr, "compat32: guest display config %dx%d@%d\n",
                *legacy_screen_width, *legacy_screen_height,
                *legacy_refresh_rate);
    }
    if (*legacy_screen_width != 0 && *legacy_screen_height != 0 &&
        *legacy_refresh_rate != 0) {
        return;
    }

    CGDisplayModeRef current_mode =
        CGDisplayCopyDisplayMode(preferred_game_display_id());
    if (!current_mode) return;
    size_t width = CGDisplayModeGetWidth(current_mode);
    size_t height = CGDisplayModeGetHeight(current_mode);
    double refresh_value = CGDisplayModeGetRefreshRate(current_mode);
    if (refresh_value < 1.0) refresh_value = 60.0;
    if (*legacy_screen_width == 0 && width <= INT32_MAX) {
        *legacy_screen_width = (int32_t)width;
    }
    if (*legacy_screen_height == 0 && height <= INT32_MAX) {
        *legacy_screen_height = (int32_t)height;
    }
    if (*legacy_refresh_rate == 0 && refresh_value <= INT32_MAX) {
        *legacy_refresh_rate = (int32_t)llround(refresh_value);
    }
    static bool logged_resolution_materialization;
    if (!logged_resolution_materialization) {
        fprintf(stderr,
                "compat32: materialized automatic display mode as %dx%d@%d\n",
                *legacy_screen_width, *legacy_screen_height,
                *legacy_refresh_rate);
        logged_resolution_materialization = true;
    }
    CFRelease(current_mode);
}

static void trace_guest_display_selection(void)
{
    if (!getenv("LP32_TRACE_RESOLUTION")) return;
    const struct lp32_display_layout *display = lp32_profile()->display;
    if (!display || !display->renderer_display_slot) return;

    uint32_t renderer_address =
        *(volatile uint32_t *)(uintptr_t)display->renderer_display_slot;
    if (renderer_address < UINT32_C(0x00001000) ||
        renderer_address >= bridge_image->max_address) {
        return;
    }
    const volatile uint8_t *renderer =
        (const void *)(uintptr_t)renderer_address;
    int32_t count = *(const volatile int32_t *)(renderer + 0x64);
    uint32_t modes_address =
        *(const volatile uint32_t *)(renderer + 0x68);
    int32_t selected = *(const volatile int32_t *)(renderer + 0xb4);
    static bool logged_raw_renderer;
    if (!logged_raw_renderer) {
        fprintf(stderr,
                "compat32: raw renderer display object slot=0x%08x "
                "value=0x%08x count=%d modes=0x%08x index=%d\n",
                display->renderer_display_slot, renderer_address, count,
                modes_address, selected);
        logged_raw_renderer = true;
    }
    if (count < 1 || count > 32 || modes_address < UINT32_C(0x00001000) ||
        modes_address >= bridge_image->max_address || selected < 0 ||
        selected >= count) {
        return;
    }
    const volatile int32_t *mode =
        (const void *)(uintptr_t)(modes_address + (uint32_t)selected * 12);
    static int32_t previous_selected = INT32_MIN;
    static int32_t previous_width = INT32_MIN;
    static int32_t previous_height = INT32_MIN;
    if (selected != previous_selected || mode[0] != previous_width ||
        mode[1] != previous_height) {
        fprintf(stderr,
                "compat32: renderer display selection object=0x%08x "
                "count=%d index=%d mode=%dx%d@%d config=%dx%d@%d\n",
                renderer_address, count, selected, mode[0], mode[1], mode[2],
                *(volatile int32_t *)(uintptr_t)display->screen_width,
                *(volatile int32_t *)(uintptr_t)display->screen_height,
                *(volatile int32_t *)(uintptr_t)display->refresh_rate);
        previous_selected = selected;
        previous_width = mode[0];
        previous_height = mode[1];
    }
}

/* The bridge owns the swap interval from the first frame, so a title that
   ships without a VerticalSync setting and asks for interval 0 still presents
   through the pacer rather than running free at the driver's mercy. */
static bool frame_pacer_enabled = true;
static int frame_pacer_requested_interval;

static bool frame_pacer_takes_over_swap_interval(GLint requested)
{
    static int honor = -1;
    if (honor < 0) honor = getenv("LP32_HONOR_SWAP_INTERVAL") != NULL;
    frame_pacer_requested_interval = requested;
    if (honor) {
        frame_pacer_enabled = false;
        return false;
    }
    frame_pacer_enabled = true;
    if (getenv("LP32_TRACE_DISPLAY")) {
        fprintf(stderr,
                "compat32: game requested swap interval %d; bridge paces "
                "frames instead\n", requested);
    }
    return true;
}

static double frame_pacer_target_fps(NSWindow *window)
{
    /* -1: not read yet; -2: no override; otherwise the requested cap. */
    static int override_fps = -1;
    if (override_fps == -1) {
        const char *text = getenv("LP32_MAX_FPS");
        override_fps = text ? (atoi(text) > 0 ? atoi(text) : 0) : -2;
    }
    if (override_fps >= 0) return override_fps;
    (void)window;
    return 0.0;
}

/*
 * Only reached with LP32_MAX_FPS at or above 100; without it the pacer has no
 * target and never waits.
 * On a 120 Hz display a cap of 120 is only smooth if the game reaches it.
 * Gameplay frames take 8-15 ms of CPU here, so at 120 the pacer never
 * waits and every frame lands at a different time, which reads as constant
 * small drops.  The pacer therefore chooses between the display rate and
 * half of it (never below 60) from the recent frame work times: half rate
 * while any of the last 90 frames would not have fit a full-rate slot with
 * margin, full rate again once all of them fit comfortably.
 */
enum { kPacerHistory = 90 };
static uint64_t pacer_work_history[kPacerHistory];
static unsigned pacer_work_index;
static bool pacer_half_rate;

static uint64_t frame_pacer_choose_interval(double display_fps, uint64_t work_ns)
{
    uint64_t full_ns = (uint64_t)(1e9 / display_fps);
    pacer_work_history[pacer_work_index++ % kPacerHistory] = work_ns;
    if (display_fps < 100.0) return full_ns;

    uint64_t worst = 0;
    for (unsigned index = 0; index < kPacerHistory; ++index) {
        if (pacer_work_history[index] > worst) worst = pacer_work_history[index];
    }
    if (!pacer_half_rate && worst > full_ns * 9 / 10) {
        pacer_half_rate = true;
    } else if (pacer_half_rate && worst < full_ns * 7 / 10) {
        pacer_half_rate = false;
    }
    return pacer_half_rate ? full_ns * 2 : full_ns;
}

/* Called after flushBuffer.  Sleeps until the next presentation slot when
   the frame finished early.  The next slot is always one interval after the
   actual presentation, so a slow frame is never followed by a rushed one. */
extern _Thread_local uint64_t lp32_pacer_wait_ns;
static void frame_pacer_wait(NSWindow *window)
{
    static uint64_t next_slot_ns;
    static uint64_t last_present_ns;
    static double cached_fps;
    static uint64_t cached_fps_swap;
    static mach_timebase_info_data_t timebase;

    if (!frame_pacer_enabled) {
        next_slot_ns = 0;
        return;
    }
    if (!cached_fps || objc_bridge_swap_count - cached_fps_swap >= 120) {
        cached_fps = frame_pacer_target_fps(window);
        cached_fps_swap = objc_bridge_swap_count;
    }
    if (cached_fps <= 0.0) {
        next_slot_ns = 0;
        return;
    }
    uint64_t now_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t work_ns = last_present_ns ? now_ns - last_present_ns : 0;
    uint64_t interval_ns = frame_pacer_choose_interval(cached_fps, work_ns);
    if (next_slot_ns > now_ns) {
        if (!timebase.denom) mach_timebase_info(&timebase);
        uint64_t wait_ns = next_slot_ns - now_ns;
        uint64_t wait_ticks = wait_ns * timebase.denom / timebase.numer;
        /* mach_wait_until returns early when a signal lands (a profiler's
           SIGPROF, for instance); keep waiting until the slot. */
        uint64_t deadline = mach_absolute_time() + wait_ticks;
        while (mach_absolute_time() < deadline) mach_wait_until(deadline);
        lp32_pacer_wait_ns += wait_ns;
        /* Schedule from the slot, not the wake-up time, so the few hundred
           microseconds the kernel oversleeps do not accumulate into a
           slower cadence. */
        last_present_ns = next_slot_ns;
        next_slot_ns += interval_ns;
    } else {
        last_present_ns = now_ns;
        next_slot_ns = now_ns + interval_ns;
    }
}

@interface OpenGLView : NSOpenGLView
- (id)initWithFrame:(NSRect)frame shareContext:(NSOpenGLContext *)shareContext;
- (id)initFullscreen:(NSRect)frame;
- (id)initWithFrame:(NSRect)frame shareContext:(NSOpenGLContext *)shareContext
  openGLDisplayMask:(uint32_t)displayMask sampleBuffers:(int)sampleBuffers
            samples:(int)samples;
- (id)initFullscreen:(NSRect)frame openGLDisplayMask:(uint32_t)displayMask
       sampleBuffers:(int)sampleBuffers samples:(int)samples;
- (int)updateMSAASettings:(int)sampleBuffers :(int)samples;
- (void)swapBuffers;
- (void)makeContextCurrent;
- (void)applyGuestDisplayMode;
- (NSRect)letterboxedFrame;
@end

/* The view the game presents through; the display-mode emulation re-applies
   its surface size here when the guest switches modes mid-session. */
static OpenGLView *presenting_view;

/*
 * Activation.  macOS 14 made it cooperative: activateIgnoringOtherApps: from a
 * process the user did not launch is ignored, and even a Finder launch
 * activates the app asynchronously, after the guest has ordered its window
 * front, so that window is not key yet when the first frame is presented.
 * Request activation and, once the app does become active, make the game
 * window key so keyboard input reaches it.
 */
/* LP32_BACKGROUND_TEST=1: an unattended run that must not disturb the user.
   The game is never activated or made key (its own requests are answered
   without effect), and it keeps running while inactive. */
static bool background_test_mode(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("LP32_BACKGROUND_TEST") != NULL;
    return enabled;
}

/*
 * Quit from the Dock menu (or any other quit Apple event) reaches AppKit's own
 * -terminate:, not the guest's, so the objc_msgSend hook that ends a guest
 * quit with _Exit never sees it.  AppKit then runs the normal termination
 * teardown, which can wait forever on the mixed-ABI audio graph (see the exit
 * handling in compat_runtime.c), leaving a process that only Force Quit ends.
 * Leave the same way the guest's quit does, from the notification AppKit
 * posts right before exit().
 */
@interface LP32QuitEventHandler : NSObject
- (void)handleQuit:(NSAppleEventDescriptor *)event
    withReplyEvent:(NSAppleEventDescriptor *)reply;
@end

@implementation LP32QuitEventHandler
- (void)handleQuit:(NSAppleEventDescriptor *)event
    withReplyEvent:(NSAppleEventDescriptor *)reply
{
    (void)event;
    (void)reply;
    fprintf(stderr, "compat32: quit Apple event received; exiting\n");
    compat_runtime32_heap_report("quit-apple-event");
    fflush(NULL);
    _Exit(EXIT_SUCCESS);
}
@end

static void install_termination_handler(void)
{
    static bool installed;
    if (installed) return;
    installed = true;
    /* The guest calls finishLaunching but never runs NSApplication's own
       loop, and AppKit's quit handler did not fire for a Dock quit under the
       bridge; register one directly with the Apple event manager. */
    static LP32QuitEventHandler *quit_handler;
    quit_handler = [[LP32QuitEventHandler alloc] init];
    [[NSAppleEventManager sharedAppleEventManager]
        setEventHandler:quit_handler
            andSelector:@selector(handleQuit:withReplyEvent:)
          forEventClass:kCoreEventClass
             andEventID:kAEQuitApplication];
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationWillTerminateNotification
                    object:NSApp
                     queue:nil
                usingBlock:^(NSNotification *note) {
        (void)note;
        fprintf(stderr, "compat32: AppKit termination requested (Dock quit "
                "or quit Apple event); exiting\n");
        compat_runtime32_heap_report("AppKit-will-terminate");
        fflush(NULL);
        _Exit(EXIT_SUCCESS);
    }];
}

static void activate_game_application(void)
{
    if (background_test_mode()) return;
    static bool observing;
    if (!observing) {
        observing = true;
        [[NSNotificationCenter defaultCenter]
            addObserverForName:NSApplicationDidBecomeActiveNotification
                        object:NSApp
                         queue:nil
                    usingBlock:^(NSNotification *note) {
            (void)note;
            NSWindow *window = [presenting_view window];
            if (window && ![window isKeyWindow]) [window makeKeyAndOrderFront:nil];
        }];
    }
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    if (@available(macOS 14.0, *)) {
        [NSApp activate];
    } else {
        [NSApp activateIgnoringOtherApps:YES];
    }
    /* A terminal-launched process may not truly become frontmost, so the real
       NSApplicationDidBecomeActive is never posted and games that create their
       window from -applicationDidBecomeActive: stall.  Post it once directly so
       the delegate's observer fires.  isActive-gated AppKit internals still see
       the real state; this only drives the app delegate. */
    static bool posted_active;
    if (!posted_active) {
        posted_active = true;
        [[NSNotificationCenter defaultCenter]
            postNotificationName:NSApplicationDidBecomeActiveNotification
                          object:NSApp];
    }
}

@implementation OpenGLView

/* Size the context's backing surface to the emulated display mode (CGL scales
   it to the view) and letterbox the view inside its window.  With no mode in
   effect the surface follows the view and the view fills the window. */
- (void)applyGuestDisplayMode
{
    CGLContextObj context = [[self openGLContext] CGLContextObj];
    if (context) {
        if (guest_display_mode_active()) {
            GLint dimensions[2] = {
                (GLint)guest_display_mode.width, (GLint)guest_display_mode.height,
            };
            CGLSetParameter(context, kCGLCPSurfaceBackingSize, dimensions);
            CGLEnable(context, kCGLCESurfaceBackingSize);
        } else {
            CGLDisable(context, kCGLCESurfaceBackingSize);
        }
    }
    NSRect frame = [self letterboxedFrame];
    if (!NSEqualRects([self frame], frame)) [super setFrame:frame];
    [[self openGLContext] update];
}

/* Where the view belongs inside its window: the whole content area, or the
   aspect-correct rectangle for the emulated mode when the view sits in the
   letterbox container installed by the setContentView: bridge below.  A view
   that is itself the content view must fill the window (AppKit invariant), so
   it falls back to a stretched fill. */
- (NSRect)letterboxedFrame
{
    NSView *superview = [self superview];
    if (!superview) return [self frame];
    NSRect bounds = [superview bounds];
    return [[self window] contentView] == self ? bounds : guest_display_mode_rect(bounds);
}

- (void)setFrame:(NSRect)frame
{
    /* The game sizes the view to the mode it believes the display switched
       to; the emulated mode decides the real frame instead. */
    if ([self superview]) frame = [self letterboxedFrame];
    [super setFrame:frame];
    [self applyGuestDisplayMode];
}

static NSOpenGLPixelFormat *legacy_pixel_format(void)
{
    NSOpenGLPixelFormatAttribute attributes[] = {
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAAccelerated,
        NSOpenGLPFAColorSize, 24,
        NSOpenGLPFADepthSize, 24,
        NSOpenGLPFAStencilSize, 8,
        0,
    };
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return [[[NSOpenGLPixelFormat alloc] initWithAttributes:attributes] autorelease];
#pragma clang diagnostic pop
}

- (id)initWithFrame:(NSRect)frame
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return [super initWithFrame:frame pixelFormat:legacy_pixel_format()];
#pragma clang diagnostic pop
}

- (id)initWithFrame:(NSRect)frame shareContext:(NSOpenGLContext *)shareContext
{
    self = [self initWithFrame:frame];
    if (self && shareContext) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSOpenGLContext *context = [[[NSOpenGLContext alloc]
            initWithFormat:[self pixelFormat] shareContext:shareContext] autorelease];
        [self setOpenGLContext:context];
#pragma clang diagnostic pop
    }
    return self;
}

- (id)initFullscreen:(NSRect)frame { return [self initWithFrame:frame]; }

/* The legacy display path passes the display mask and MSAA request
   explicitly.  Multisampling is not applied to the window's pixel format:
   the title renders through its own render targets and the swap-chain
   format is what the bridge's frame pacer and capture path expect. */
- (id)initWithFrame:(NSRect)frame shareContext:(NSOpenGLContext *)shareContext
  openGLDisplayMask:(uint32_t)displayMask sampleBuffers:(int)sampleBuffers
            samples:(int)samples
{
    (void)displayMask; (void)sampleBuffers; (void)samples;
    return [self initWithFrame:frame shareContext:shareContext];
}
- (id)initFullscreen:(NSRect)frame openGLDisplayMask:(uint32_t)displayMask
       sampleBuffers:(int)sampleBuffers samples:(int)samples
{
    (void)displayMask; (void)sampleBuffers; (void)samples;
    return [self initWithFrame:frame];
}
- (int)updateMSAASettings:(int)sampleBuffers :(int)samples
{
    (void)sampleBuffers; (void)samples;
    return 0;
}
- (void)swapBuffers
{
    static bool logged_first_frame;
    static bool captured_frame;
    static bool posted_debug_key;
    static bool posted_title_key;
    static bool posted_title_skip_return;
    static bool posted_title_skip_key;
    static CFAbsoluteTime title_skip_time;
    static bool installed_test_input_signals;
    static bool installed_gl_trace_signal;
    static int active_test_key = -1;
    static uint64_t test_key_release_swap;
    static CFAbsoluteTime test_key_release_time;
    static uint64_t posted_proxy_stress_events;
    static CFAbsoluteTime heap_test_start_time;
    static CFAbsoluteTime heap_gameplay_script_start;
    static CFAbsoluteTime heap_gameplay_next_movement;
    static unsigned heap_gameplay_script_step;
    static unsigned heap_gameplay_movement_step;
    static bool heap_gameplay_level_skip_posted;
    uint64_t swap_count = ++objc_bridge_swap_count;
    if (!installed_gl_trace_signal) {
        initialize_gl_trace_output();
        install_gl_trace_signal();
        installed_gl_trace_signal = true;
        gl_trace_printf(
            "compat32: enabled SIGQUIT-triggered GL tracing at swap %llu\n",
            (unsigned long long)swap_count);
    }
    arm_pending_gl_trace();
    compat_runtime32_heap_frame(swap_count);
    compat_runtime32_check_mode_guards(swap_count);
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (heap_test_start_time == 0.0) heap_test_start_time = now;
    const char *test_exit_text = getenv("LP32_TEST_EXIT_AT_SWAP");
    uint64_t test_exit_swap = test_exit_text ?
        strtoull(test_exit_text, NULL, 0) : 0;
    const char *test_seconds_text = getenv("LP32_TEST_EXIT_AFTER_SECONDS");
    double test_seconds = test_seconds_text ?
        strtod(test_seconds_text, NULL) : 0.0;
    if ((test_exit_swap && swap_count >= test_exit_swap) ||
        (test_seconds > 0.0 && now - heap_test_start_time >= test_seconds)) {
        compat_runtime32_heap_report("test-exit");
        fflush(NULL);
        _Exit(EXIT_SUCCESS);
    }
    if (!installed_test_input_signals && getenv("LP32_ENABLE_TEST_INPUT")) {
        test_input_wasd = getenv("LP32_TEST_WASD") != NULL;
        install_test_input_signals();
        installed_test_input_signals = true;
        fputs(test_input_wasd ?
              "compat32: enabled signal-driven test input "
              "(USR1=return USR2=space INFO=s HUP=w WINCH=escape "
              "URG=a ALRM=d PROF=backspace XCPU=u)\n" :
              "compat32: enabled signal-driven test input "
              "(USR1=return USR2=space INFO=down HUP=up "
              "WINCH=escape URG=left ALRM=right PROF=backspace "
              "XCPU=u)\n",
              stderr);
    }
    const char *stress_event_text = getenv("LP32_PROXY_STRESS_EVENTS");
    uint64_t stress_event_target = stress_event_text ?
        strtoull(stress_event_text, NULL, 0) : 0;
    if (stress_event_target > 100000) stress_event_target = 100000;
    if (posted_proxy_stress_events < stress_event_target) {
        NSWindow *window = [self window];
        uint64_t batch = stress_event_target - posted_proxy_stress_events;
        if (batch > 16) batch = 16;
        for (uint64_t index = 0; index < batch; ++index) {
            post_test_key_event(window, 90, (posted_proxy_stress_events & 1) == 0);
            ++posted_proxy_stress_events;
        }
        if (posted_proxy_stress_events == stress_event_target) {
            fprintf(stderr,
                    "compat32: posted %llu F20 proxy-stress events\n",
                    (unsigned long long)posted_proxy_stress_events);
        }
    }
    if (getenv("LP32_TRACE_OBJC_PROXIES") &&
        (swap_count <= 10 || swap_count % 300 == 0)) {
        fprintf(stderr,
                "compat32: Objective-C proxies swap=%llu persistent=%u "
                "event=%u\n",
                (unsigned long long)swap_count, proxy_count,
                event_proxy_count);
    }
    materialize_automatic_display_mode();
    trace_guest_display_selection();
    sig_atomic_t pending_key = pending_test_key_plus_one;
    if (pending_key > 0) {
        pending_test_key_plus_one = 0;
        unsigned short key_code = (unsigned short)(pending_key - 1);
        if (active_test_key >= 0) {
            post_test_key_event([self window],
                                (unsigned short)active_test_key, NO);
        }
        post_test_key_event([self window], key_code, YES);
        active_test_key = key_code;
        /* The guest applies its own menu-repeat policy to held arrows. */
        bool is_directional = (key_code >= 123 && key_code <= 126) ||
                              key_code == 0 || key_code == 1 ||
                              key_code == 2 || key_code == 13;
        test_key_release_swap = swap_count + (is_directional ? 1 : 6);
        test_key_release_time = now + (is_directional ? 0.05 : 0.15);
        fprintf(stderr, "compat32: pressed test key %hu at swap %llu\n",
                key_code, (unsigned long long)swap_count);
    }
    if (active_test_key < 0) {
        double hold_seconds = 0.0;
        int fifo_key = read_test_key_fifo(&hold_seconds);
        if (fifo_key >= 0) {
            post_test_key_event([self window], (unsigned short)fifo_key, YES);
            active_test_key = fifo_key;
            test_key_release_swap = swap_count + 1;
            test_key_release_time = now + (hold_seconds > 0.0 ? hold_seconds : 0.15);
            fprintf(stderr,
                    "compat32: pressed fifo test key %d for %.2fs at swap %llu\n",
                    fifo_key, hold_seconds > 0.0 ? hold_seconds : 0.15,
                    (unsigned long long)swap_count);
        }
    }
    if (active_test_key >= 0 && swap_count >= test_key_release_swap &&
        (test_key_release_time == 0.0 || now >= test_key_release_time)) {
        post_test_key_event([self window], (unsigned short)active_test_key, NO);
        fprintf(stderr, "compat32: released test key %d at swap %llu\n",
                active_test_key, (unsigned long long)swap_count);
        active_test_key = -1;
        test_key_release_time = 0.0;
    }
    if (!posted_title_skip_key && title_skip_time > 0.0 &&
        now >= title_skip_time) {
        if (active_test_key >= 0) {
            post_test_key_event([self window],
                                (unsigned short)active_test_key, NO);
        }
        if (!posted_title_skip_return) {
            /* The title's movie skip is a two-step chord: Return reveals the
               prompt, then U confirms it.  A lone U was timing-dependent. */
            post_test_key_event([self window], 36, YES);
            active_test_key = 36;
            test_key_release_swap = swap_count + 6;
            test_key_release_time = now + 0.15;
            title_skip_time = now + 0.5;
            posted_title_skip_return = true;
            fprintf(stderr,
                    "compat32: posted title cutscene Return at swap %llu\n",
                    (unsigned long long)swap_count);
        } else {
            post_test_key_event([self window], 32, YES);
            active_test_key = 32;
            test_key_release_swap = swap_count + 6;
            test_key_release_time = now + 0.25;
            posted_title_skip_key = true;
            fprintf(stderr,
                    "compat32: posted title cutscene U at swap %llu\n",
                    (unsigned long long)swap_count);
        }
    }
    if (getenv("LP32_HEAP_GAMEPLAY_STRESS") && posted_title_skip_key) {
        static const double menu_times[] = {40.0, 42.5, 45.0, 48.0, 118.0};
        static const unsigned short movement_keys[] = {13, 2, 13, 0, 49, 32};
        if (heap_gameplay_script_start == 0.0) {
            const char *movement_delay_text =
                getenv("LP32_HEAP_GAMEPLAY_MOVEMENT_DELAY");
            double movement_delay = movement_delay_text ?
                strtod(movement_delay_text, NULL) : 270.0;
            /* Step five starts gameplay at 118 seconds.  Keep at least a
               small loading margin when a shorter diagnostic delay is used. */
            if (movement_delay < 125.0) movement_delay = 125.0;
            heap_gameplay_script_start = now;
            heap_gameplay_next_movement = now + movement_delay;
            fprintf(stderr,
                    "compat32: armed heap gameplay stress after title at "
                    "swap %llu (movement delay %.1fs)\n",
                    (unsigned long long)swap_count, movement_delay);
        }
        double elapsed = now - heap_gameplay_script_start;
        if (heap_gameplay_script_step <
                sizeof(menu_times) / sizeof(menu_times[0]) &&
            elapsed >= menu_times[heap_gameplay_script_step] &&
            active_test_key < 0) {
            post_test_key_event([self window], 36, YES);
            active_test_key = 36;
            test_key_release_swap = swap_count + 6;
            test_key_release_time = now + 0.15;
            fprintf(stderr,
                    "compat32: heap gameplay stress Return step %u at "
                    "swap %llu\n", heap_gameplay_script_step + 1,
                    (unsigned long long)swap_count);
            ++heap_gameplay_script_step;
        } else if (heap_gameplay_script_step ==
                       sizeof(menu_times) / sizeof(menu_times[0]) &&
                   getenv("LP32_HEAP_GAMEPLAY_SKIP_LEVEL_CUTSCENE") &&
                   !heap_gameplay_level_skip_posted && elapsed >= 123.0 &&
                   active_test_key < 0) {
            post_test_key_event([self window], 32, YES);
            active_test_key = 32;
            test_key_release_swap = swap_count + 6;
            test_key_release_time = now + 0.25;
            heap_gameplay_level_skip_posted = true;
            fprintf(stderr,
                    "compat32: heap gameplay stress posted level cutscene U "
                    "at swap %llu\n", (unsigned long long)swap_count);
        } else if (heap_gameplay_script_step ==
                       sizeof(menu_times) / sizeof(menu_times[0]) &&
                   now >= heap_gameplay_next_movement &&
                   active_test_key < 0) {
            unsigned short key = movement_keys[heap_gameplay_movement_step %
                (sizeof(movement_keys) / sizeof(movement_keys[0]))];
            post_test_key_event([self window], key, YES);
            active_test_key = key;
            test_key_release_swap = swap_count + 30;
            test_key_release_time = now + 0.5;
            heap_gameplay_next_movement = now + 1.5;
            ++heap_gameplay_movement_step;
            if (heap_gameplay_movement_step <= 12 ||
                heap_gameplay_movement_step % 30 == 0) {
                fprintf(stderr,
                        "compat32: heap gameplay stress movement %u key %hu "
                        "at swap %llu\n", heap_gameplay_movement_step, key,
                        (unsigned long long)swap_count);
            }
        }
    }
    if (getenv("LP32_TRACE_TIMING") &&
        (swap_count <= 10 || swap_count % 60 == 0)) {
        fprintf(stderr, "compat32: OpenGL swap %llu\n",
                (unsigned long long)swap_count);
    }
    const struct lp32_display_layout *frontend = lp32_profile()->display;
    if (getenv("LP32_TRACE_FRONTEND_STATE") && frontend &&
        frontend->frontend_state_pointer &&
        (swap_count <= 10 || swap_count % 60 == 0)) {
        const uint32_t *state_slot =
            (const void *)(uintptr_t)frontend->frontend_state_pointer;
        const uint32_t *page_slot =
            (const void *)(uintptr_t)frontend->frontend_page_slot;
        const uint32_t *state = state_slot[0] ?
            (const void *)(uintptr_t)state_slot[0] : NULL;
        const uint32_t *page_base = page_slot[0] ?
            (const void *)(uintptr_t)page_slot[0] : NULL;
        const uint32_t *page = page_base ? page_base + 3 : NULL;
        const uint32_t *gate_slot =
            (const void *)(uintptr_t)frontend->frontend_gate_slot;
        const uint32_t *gate = gate_slot[0] ?
            (const void *)(uintptr_t)gate_slot[0] : NULL;
        fprintf(stderr,
                "compat32: frontend swap=%llu state=%d page=%08x "
                "count10=%d cursor18=%d limit40=%d item44=%d "
                "fade48=%08x clock4c=%08x:%08x now54=%08x:%08x "
                "delay5c=%08x wait60=%u flags64=%08x owner68=%08x "
                "gate=%08x gate2c=%d gate30=%08x:%08x "
                "gate38=%08x:%08x gate40=%08x gate44=%u "
                "gate48=%08x gate50=%08x gate54=%d active=%08x\n",
                (unsigned long long)swap_count,
                state ? (int32_t)state[0] : -999,
                page ? (uint32_t)(uintptr_t)page : 0,
                page ? (int32_t)page[0x10 / 4] : -999,
                page ? (int32_t)page[0x18 / 4] : -999,
                page ? (int32_t)page[0x40 / 4] : -999,
                page ? (int32_t)page[0x44 / 4] : -999,
                page ? page[0x48 / 4] : 0,
                page ? page[0x4c / 4] : 0,
                page ? page[0x50 / 4] : 0,
                page ? page[0x54 / 4] : 0,
                page ? page[0x58 / 4] : 0,
                page ? page[0x5c / 4] : 0,
                page ? ((const uint8_t *)page)[0x60] : 0,
                page ? page[0x64 / 4] : 0,
                page ? page[0x68 / 4] : 0,
                gate ? (uint32_t)(uintptr_t)gate : 0,
                gate ? (int32_t)gate[0x2c / 4] : -999,
                gate ? gate[0x30 / 4] : 0,
                gate ? gate[0x34 / 4] : 0,
                gate ? gate[0x38 / 4] : 0,
                gate ? gate[0x3c / 4] : 0,
                gate ? gate[0x40 / 4] : 0,
                gate ? ((const uint8_t *)gate)[0x44] : 0,
                gate ? gate[0x48 / 4] : 0,
                gate ? gate[0x50 / 4] : 0,
                gate ? (int32_t)gate[0x54 / 4] : -999,
                *(const uint32_t *)(uintptr_t)frontend->frontend_active_word);
    }
    presenting_view = self;
    /*
     * The game's fullscreen window hides on deactivate.  Ordering a window out
     * destroys its GL surface, and AppKit only re-attaches the context to the
     * new surface on an update, which nothing triggers when the window comes
     * back with an unchanged frame: every flushBuffer after reactivation went
     * to a detached drawable and the window stayed black while the game ran
     * on (audio and input still working).  Re-attach on the first frame
     * presented after the window becomes visible again.  A window hidden by
     * hidesOnDeactivate still answers isVisible=YES (only an explicit
     * orderOut: clears it) while its occlusion state drops the visible bit;
     * the occlusion bit in turn is not reliable for a window on another
     * display that is ordered in without the application being active.  A
     * window that regains either is treated as shown again.
     */
    {
        static unsigned window_visibility;
        static uint64_t reattach_until;
        NSWindow *window = [self window];
        unsigned visibility = 0;
        if (window && [window isVisible]) visibility |= 1;
        if (window && ([window occlusionState] &
                       NSWindowOcclusionStateVisible)) visibility |= 2;
        bool visible = visibility != 0;
        if ((visibility & ~window_visibility) && logged_first_frame) {
            reattach_until = swap_count + 30;
            fprintf(stderr, "compat32: window shown again at swap %llu "
                    "(visibility %u -> %u); re-attaching the GL context\n",
                    (unsigned long long)swap_count, window_visibility,
                    visibility);
        }
        window_visibility = visibility;
        if (visible && swap_count <= reattach_until) {
            NSOpenGLContext *context = [self openGLContext];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            if ([context view] != self) [context setView:self];
#pragma clang diagnostic pop
            [self applyGuestDisplayMode];
            [window display];
        }
    }
    if (!logged_first_frame) {
        NSWindow *window = [self window];
        install_termination_handler();
        [self applyGuestDisplayMode];
        if (window && !background_test_mode()) {
            activate_game_application();
            [window makeKeyAndOrderFront:nil];
            [window makeMainWindow];
        } else if (window) {
            [window orderFront:nil];
        }
        fprintf(stderr,
                "compat32: presenting first OpenGL frame window=%s visible=%d key=%d main=%d\n",
                window ? class_getName([window class]) : "(none)",
                window ? [window isVisible] : 0,
                window ? [window isKeyWindow] : 0,
                window ? [window isMainWindow] : 0);
        logged_first_frame = true;
    }
    const char *debug_key_swap_text = getenv("LP32_AUTOPRESS_RETURN_AT_SWAP");
    uint64_t debug_key_swap = debug_key_swap_text ?
        strtoull(debug_key_swap_text, NULL, 10) : 0;
    if (!posted_debug_key && debug_key_swap && swap_count >= debug_key_swap) {
        NSWindow *window = [self window];
        const char *debug_key_code_text = getenv("LP32_AUTOPRESS_KEYCODE");
        unsigned long parsed_key_code = debug_key_code_text ?
            strtoul(debug_key_code_text, NULL, 10) : 36;
        unsigned short key_code = parsed_key_code <= UINT16_MAX ?
            (unsigned short)parsed_key_code : 36;
        if (active_test_key >= 0) {
            post_test_key_event(window, (unsigned short)active_test_key, NO);
        }
        post_test_key_event(window, key_code, YES);
        active_test_key = key_code;
        test_key_release_swap = swap_count + 6;
        test_key_release_time = now + 0.15;
        fprintf(stderr, "compat32: posted debug key %hu at swap %llu\n",
                key_code, (unsigned long long)swap_count);
        posted_debug_key = true;
    }
    if (!posted_title_key && getenv("LP32_AUTOPRESS_ON_TITLE") &&
        swap_count % 15 == 0) {
        GLint viewport[4] = {0, 0, 0, 0};
        GLint old_read_buffer = GL_BACK;
        GLint old_pack_alignment = 4;
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack_alignment);
        int sample_x = viewport[0] + viewport[2] * 12 / 100;
        int sample_y = viewport[1] + viewport[3] * 66 / 100;
        int sample_width = viewport[2] * 16 / 100;
        int sample_height = viewport[3] * 18 / 100;
        size_t pixel_count = sample_width > 0 && sample_height > 0 ?
            (size_t)sample_width * (size_t)sample_height : 0;
        uint8_t *pixels = pixel_count ? malloc(pixel_count * 3) : NULL;
        if (pixels) {
            glReadBuffer(GL_BACK);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(sample_x, sample_y, sample_width, sample_height,
                         GL_RGB, GL_UNSIGNED_BYTE, pixels);
            size_t red_pixels = 0;
            for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
                const uint8_t *rgb = pixels + pixel * 3;
                if (rgb[0] > 160 && rgb[1] < 90 && rgb[2] < 90) {
                    ++red_pixels;
                }
            }
            free(pixels);
            if (red_pixels * 100 >= pixel_count * 30) {
                if (active_test_key >= 0) {
                    post_test_key_event([self window],
                                        (unsigned short)active_test_key, NO);
                }
                post_test_key_event([self window], 36, YES);
                active_test_key = 36;
                test_key_release_swap = swap_count + 6;
                test_key_release_time = now + 0.15;
                posted_title_key = true;
                if (getenv("LP32_AUTOSKIP_AFTER_TITLE")) {
                    title_skip_time = now + 1.0;
                }
                fprintf(stderr,
                        "compat32: detected title and posted test Return at swap %llu\n",
                        (unsigned long long)swap_count);
            }
        }
        glReadBuffer(old_read_buffer);
        glPixelStorei(GL_PACK_ALIGNMENT, old_pack_alignment);
    }
    const char *capture_path = getenv("LP32_CAPTURE_FRAME");
    const char *capture_number_text = getenv("LP32_CAPTURE_FRAME_NUMBER");
    uint64_t capture_number = capture_number_text ?
        strtoull(capture_number_text, NULL, 10) : 1;
    const char *capture_interval_text = getenv("LP32_CAPTURE_INTERVAL");
    uint64_t capture_interval = capture_interval_text ?
        strtoull(capture_interval_text, NULL, 10) : 0;
    bool capture_due = swap_count >= capture_number &&
        (!capture_interval || (swap_count - capture_number) % capture_interval == 0);
    if ((!captured_frame || capture_interval) && capture_due &&
        capture_path && capture_path[0]) {
        GLint viewport[4] = {0, 0, 0, 0};
        GLint old_read_buffer = GL_BACK;
        GLint old_pack_alignment = 4;
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack_alignment);
        if (viewport[2] > 0 && viewport[3] > 0) {
            size_t row_size = (size_t)viewport[2] * 3;
            size_t byte_size = row_size * (size_t)viewport[3];
            uint8_t *pixels = malloc(byte_size);
            if (pixels) {
                glReadBuffer(GL_BACK);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3],
                             GL_RGB, GL_UNSIGNED_BYTE, pixels);
                bool has_visible_pixel = false;
                for (size_t offset = 0; offset < byte_size; ++offset) {
                    if (pixels[offset] > 2) {
                        has_visible_pixel = true;
                        break;
                    }
                }
                if (has_visible_pixel) {
                    char sequenced_path[4096];
                    const char *output_path = capture_path;
                    if (getenv("LP32_CAPTURE_SEQUENCE")) {
                        int length = snprintf(sequenced_path,
                                              sizeof(sequenced_path),
                                              "%s.swap-%llu.ppm", capture_path,
                                              (unsigned long long)swap_count);
                        if (length > 0 && (size_t)length < sizeof(sequenced_path)) {
                            output_path = sequenced_path;
                        }
                    }
                    FILE *file = fopen(output_path, "wb");
                    if (file) {
                        fprintf(file, "P6\n%d %d\n255\n", viewport[2], viewport[3]);
                        for (GLint row = viewport[3] - 1; row >= 0; --row) {
                            fwrite(pixels + (size_t)row * row_size, row_size, 1,
                                   file);
                        }
                        fclose(file);
                        fprintf(stderr,
                                "compat32: captured non-black swap %llu %dx%d at %s\n",
                                (unsigned long long)swap_count,
                                viewport[2], viewport[3], output_path);
                        captured_frame = capture_interval == 0;
                    }
                }
                free(pixels);
            }
        }
        glReadBuffer((GLenum)old_read_buffer);
        glPixelStorei(GL_PACK_ALIGNMENT, old_pack_alignment);
    }
    trace_gl_frame_boundary();
    present_frame_with_stats([self openGLContext], [self window], true);
}

/* Presents one frame and keeps the frame-pacing statistics.  Shared by the
   bridge's own OpenGLView and the guest's direct NSOpenGLContext flushBuffer
   messages (Bejeweled 3 never goes through the bridge view).  `pace` runs the
   bridge frame pacer after the flush. */
static void present_frame_with_stats(NSOpenGLContext *context,
                                     NSWindow *window, bool pace)
{
    if (!compat_runtime32_frame_profile_enabled) {
        [context flushBuffer];
        if (pace) frame_pacer_wait(window);
        audio_bridge32_note_frame_presented();
        return;
    }

    /* Frame pacing statistics (LP32_FRAME_STATS=<swap interval>).  The
       bridge counters cover the render thread since the previous swap; the
       previous flushBuffer wait is part of that dispatch total and is
       reported separately so bridge overhead can be read as
       dispatch - flush. */
    static struct {
        uint64_t last_swap_ns;
        uint64_t frames;
        uint64_t frame_ns_total, frame_ns_max;
        uint64_t frames_over_20ms, frames_over_34ms;
        uint64_t flush_ns_total, flush_ns_max;
        uint64_t calls_total, calls_max;
        uint64_t dispatch_ns_total, dispatch_ns_max;
        uint64_t audio_ns_total, audio_ns_max;
        uint64_t sleep_ns_total;
        uint64_t objc_ns_total;
        uint64_t lock_wait_ns_total, lock_wait_ns_max, lock_waits_total;
        uint64_t last_flush_ns;
        uint64_t present_ns_total, present_ns_max;
    } stats;
    static uint64_t report_interval;
    if (!report_interval) {
        /* The timing is also on when only LP32_SLOW_IMPORT_MS was asked for,
           so the interval variable may be absent. */
        const char *interval_text = getenv("LP32_FRAME_STATS");
        report_interval = interval_text ? strtoull(interval_text, NULL, 0) : 0;
        if (!report_interval) report_interval = 300;
    }
    uint64_t now_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    struct compat_runtime32_frame_profile profile;
    compat_runtime32_take_frame_profile(&profile);
    if (stats.last_swap_ns) {
        uint64_t frame_ns = now_ns - stats.last_swap_ns;
        uint64_t bridge_ns = profile.dispatch_ns > stats.last_flush_ns ?
            profile.dispatch_ns - stats.last_flush_ns : 0;
        /* LP32_FRAME_STATS_SLOW=<ms>: dump the imports of any frame whose CPU
           time (excluding the previous flush/pacer wait) exceeds the limit. */
        static int64_t slow_frame_ns = -1;
        if (slow_frame_ns < 0) {
            const char *text = getenv("LP32_FRAME_STATS_SLOW");
            slow_frame_ns = text && text[0] ? (int64_t)(strtod(text, NULL) * 1e6) : 0;
        }
        if (slow_frame_ns > 0) {
            /* frame_ns already runs from the previous flush's end, so it is
               the frame's CPU time (no flush or pacer wait inside). */
            uint64_t cpu_ns = frame_ns;
            bool slow = (int64_t)cpu_ns >= slow_frame_ns;
            if (slow) {
                fprintf(stderr,
                        "compat32: slow frame t=%.3f swap=%llu cpu=%.2fms bridge=%.2fms imports=%llu\n",
                        now_ns / 1e9, (unsigned long long)objc_bridge_swap_count,
                        cpu_ns / 1e6, bridge_ns / 1e6,
                        (unsigned long long)profile.calls);
            }
            compat_runtime32_frame_import_delta(slow, 14);
        }
        ++stats.frames;
        stats.frame_ns_total += frame_ns;
        if (frame_ns > stats.frame_ns_max) stats.frame_ns_max = frame_ns;
        if (frame_ns > 20000000) ++stats.frames_over_20ms;
        if (frame_ns > 34000000) ++stats.frames_over_34ms;
        stats.calls_total += profile.calls;
        if (profile.calls > stats.calls_max) stats.calls_max = profile.calls;
        stats.dispatch_ns_total += bridge_ns;
        if (bridge_ns > stats.dispatch_ns_max) stats.dispatch_ns_max = bridge_ns;
        stats.audio_ns_total += profile.audio_ns;
        stats.sleep_ns_total += profile.sleep_ns;
        if (profile.audio_ns > stats.audio_ns_max) stats.audio_ns_max = profile.audio_ns;
        stats.objc_ns_total += profile.objc_ns > stats.last_flush_ns ?
            profile.objc_ns - stats.last_flush_ns : 0;
        stats.lock_wait_ns_total += profile.lock_wait_ns;
        if (profile.lock_wait_ns > stats.lock_wait_ns_max) {
            stats.lock_wait_ns_max = profile.lock_wait_ns;
        }
        stats.lock_waits_total += profile.lock_waits;
        if (stats.frames % report_interval == 0) {
            double frames = (double)stats.frames;
            /* Presented frames per second: the interval between
               consecutive returns from flush (including pacing) is the
               real frame rate; "frame" below is the CPU time between
               flushes, i.e. how fast the game could run uncapped. */
            double present_seconds = (double)stats.present_ns_total / 1e9;
            uint64_t pool_hits = 0, pool_misses = 0;
            audio_bridge32_pool_statistics(&pool_hits, &pool_misses);
            uint32_t mode_to64 = 0, mode_to32 = 0;
            compat_runtime32_mode_guard_counts(&mode_to64, &mode_to32);
            struct audio_bridge32_worker_stats worker = {0};
            audio_bridge32_worker_statistics(&worker);
            fprintf(stderr,
                    "compat32: frames t=%.3f swap=%llu n=%llu fps=%.1f "
                    "present(avg=%.2f max=%.2f) "
                    "frame(avg=%.2f max=%.2f ms >20ms=%llu >34ms=%llu) "
                    "sleep(avg=%.2f) "
                    "flush(avg=%.2f max=%.2f) "
                    "imports(avg=%.0f max=%llu) "
                    "bridge(avg=%.2f max=%.2f objc=%.2f audio=%.2f audio-max=%.2f) "
                    "lock-wait(avg=%.2f max=%.2f waits=%llu) "
                    "pool(hits=%llu misses=%llu) pace=%s "
                    "mode-recoveries(to64=%u to32=%u) "
                    "audio-worker(starts=%llu wait-avg=%.1f wait-max=%.1f "
                    "op-max=%.1f depth-max=%u) "
                    "audio-stream(silent=%llu gaps=%llu gap-frames=%llu "
                    "cb-max=%.2f backlog-max=%llu behind=%llu) "
                    "audio-hold(frames=%llu held=%.0fms renders=%llu)\n",
                    now_ns / 1e9,
                    (unsigned long long)objc_bridge_swap_count,
                    (unsigned long long)stats.frames,
                    present_seconds > 0 ? frames / present_seconds : 0.0,
                    (double)stats.present_ns_total / frames / 1e6,
                    (double)stats.present_ns_max / 1e6,
                    (double)stats.frame_ns_total / frames / 1e6,
                    (double)stats.frame_ns_max / 1e6,
                    (unsigned long long)stats.frames_over_20ms,
                    (unsigned long long)stats.frames_over_34ms,
                    (double)stats.sleep_ns_total / frames / 1e6,
                    (double)stats.flush_ns_total / frames / 1e6,
                    (double)stats.flush_ns_max / 1e6,
                    (double)stats.calls_total / frames,
                    (unsigned long long)stats.calls_max,
                    (double)stats.dispatch_ns_total / frames / 1e6,
                    (double)stats.dispatch_ns_max / 1e6,
                    (double)stats.objc_ns_total / frames / 1e6,
                    (double)stats.audio_ns_total / frames / 1e6,
                    (double)stats.audio_ns_max / 1e6,
                    (double)stats.lock_wait_ns_total / frames / 1e6,
                    (double)stats.lock_wait_ns_max / 1e6,
                    (unsigned long long)stats.lock_waits_total,
                    (unsigned long long)pool_hits,
                    (unsigned long long)pool_misses,
                    !frame_pacer_enabled ? "off" : pacer_half_rate ? "half" : "full",
                    mode_to64, mode_to32,
                    (unsigned long long)worker.starts, worker.start_wait_avg_ms,
                    worker.start_wait_max_ms, worker.op_run_max_ms,
                    worker.queue_depth_max,
                    (unsigned long long)worker.silent_renders,
                    (unsigned long long)worker.timestamp_gaps,
                    (unsigned long long)worker.timestamp_gap_frames,
                    worker.callback_max_ms,
                    (unsigned long long)worker.stream_pending_max,
                    (unsigned long long)worker.stream_behind_renders,
                    (unsigned long long)worker.hold_episodes, worker.hold_ms,
                    (unsigned long long)worker.hold_renders);
            if (getenv("LP32_FRAME_STATS_IMPORTS")) {
                compat_runtime32_report_import_profile(
                    (unsigned)strtoul(getenv("LP32_FRAME_STATS_IMPORTS"), NULL, 0));
            }
            /* LP32_WATCH=name@0xaddr:f|d|b,... prints guest cells with each
               stats line (diagnostic for engine-side clocks and flags). */
            const char *watch = getenv("LP32_WATCH");
            if (watch && watch[0]) {
                char buffer[512];
                strlcpy(buffer, watch, sizeof(buffer));
                char *cursor = buffer;
                fprintf(stderr, "compat32: watch t=%.3f", now_ns / 1e9);
                for (char *item = strsep(&cursor, ","); item; item = strsep(&cursor, ",")) {
                    char *at = strchr(item, '@');
                    char *colon = at ? strchr(at, ':') : NULL;
                    if (!at || !colon) continue;
                    *at = 0;
                    *colon = 0;
                    uint32_t address = (uint32_t)strtoul(at + 1, NULL, 0);
                    if (address < 0x1000 || address >= 0x7f000000) continue;
                    const void *cell = (const void *)(uintptr_t)address;
                    switch (colon[1]) {
                    case 'f': { float v; memcpy(&v, cell, 4); fprintf(stderr, " %s=%g", item, v); break; }
                    case 'd': { uint32_t v; memcpy(&v, cell, 4); fprintf(stderr, " %s=%u", item, v); break; }
                    case 'b': fprintf(stderr, " %s=%u", item, *(const uint8_t *)cell); break;
                    default: break;
                    }
                }
                fputc('\n', stderr);
            }
            uint64_t keep_last = stats.last_swap_ns;
            memset(&stats, 0, sizeof(stats));
            stats.last_swap_ns = keep_last;
        }
    }
    /* The pacer wait is folded into "flush" so bridge time stays
       dispatch - flush. */
    uint64_t flush_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    [context flushBuffer];
    if (pace) frame_pacer_wait(window);
    uint64_t flush_end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    audio_bridge32_note_frame_presented();
    stats.last_flush_ns = flush_end - flush_start;
    stats.flush_ns_total += stats.last_flush_ns;
    if (stats.last_flush_ns > stats.flush_ns_max) stats.flush_ns_max = stats.last_flush_ns;
    if (stats.last_swap_ns) {
        uint64_t present_ns = flush_end - stats.last_swap_ns;
        stats.present_ns_total += present_ns;
        if (present_ns > stats.present_ns_max) stats.present_ns_max = present_ns;
    }
    stats.last_swap_ns = flush_end;
}
- (void)makeContextCurrent
{
    [[self openGLContext] makeCurrentContext];
    NSRect bounds = [self bounds];
    if (bounds.size.width <= 0 || bounds.size.height <= 0) {
        bounds = [preferred_game_screen() frame];
    }
    const struct lp32_display_layout *display = lp32_profile()->display;
    if (!display || !display->legacy_renderer_display) return;
    uint32_t *legacy_renderer =
        (void *)(uintptr_t)display->legacy_renderer_display;
    if (legacy_renderer[0x48 / 4] == 0 || legacy_renderer[0x4c / 4] == 0) {
        legacy_renderer[0x48 / 4] = (uint32_t)llround(bounds.size.width);
        legacy_renderer[0x4c / 4] = (uint32_t)llround(bounds.size.height);
        fprintf(stderr, "compat32: renderer display size %.0fx%.0f\n",
                bounds.size.width, bounds.size.height);
    }
}
- (BOOL)canBecomeKeyView { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
@end

@interface GameWindow : NSObject
{
    NSWindow *_fullScreenWindow;
    OpenGLView *_fullScreenOpenGLView;
    NSWindow *_window;
    OpenGLView *_openGLView;
    BOOL _isFullScreen;
}
- (id)initWithFullscreenView:(NSWindow *)window openGLView:(OpenGLView *)view;
- (id)initWithWindowedView:(NSWindow *)window openGLView:(OpenGLView *)view;
- (OpenGLView *)getActiveOpenGLView;
- (NSWindow *)getActiveWindow;
- (BOOL)isFullScreen;
- (BOOL)hasFocus;
- (void)goFullScreen;
- (void)goWindowed;
@end

@implementation GameWindow
- (id)initWithFullscreenView:(NSWindow *)window openGLView:(OpenGLView *)view
{
    self = [super init];
    if (self) {
        _fullScreenWindow = [window retain];
        _fullScreenOpenGLView = [view retain];
        _isFullScreen = YES;
    }
    return self;
}
- (id)initWithWindowedView:(NSWindow *)window openGLView:(OpenGLView *)view
{
    self = [super init];
    if (self) {
        _window = [window retain];
        _openGLView = [view retain];
        _isFullScreen = NO;
    }
    return self;
}
- (OpenGLView *)getActiveOpenGLView { return _isFullScreen ? _fullScreenOpenGLView : _openGLView; }
- (NSWindow *)getActiveWindow { return _isFullScreen ? _fullScreenWindow : _window; }
- (BOOL)isFullScreen { return _isFullScreen; }
- (BOOL)hasFocus
{
    /*
     * The game pauses its simulation and movie player whenever its window
     * stops being key, exactly like the shipped build.  Unattended test runs
     * set LP32_CONTINUE_WHEN_INACTIVE=1 so the game keeps running while the
     * operator works in another app.
     */
    static int keep_running = -1;
    if (keep_running < 0) {
        keep_running = getenv("LP32_CONTINUE_WHEN_INACTIVE") != NULL ||
            (background_test_mode() && !getenv("LP32_TEST_FOCUS_LOSS"));
    }
    if (keep_running) return YES;
    /* Under exclusive fullscreen the game's window was key whenever the
       application was active.  Modern activation is asynchronous and the
       borderless window is not always picked as key, so treat an active
       application as focus; the guest reads key events straight from the
       event queue, so it does not need the window to be key for input. */
    BOOL active = [NSApp isActive];
    BOOL key = [[self getActiveWindow] isKeyWindow];
    /* Test hook: LP32_TEST_FOCUS_LOSS="<lost>,<regained>" (seconds after the
       first call) reports focus lost between those times regardless of the
       real state, to exercise the game's pause/resume path unattended. */
    static double script_lost = -1, script_regained = -1, script_start;
    if (script_lost < 0) {
        const char *script = getenv("LP32_TEST_FOCUS_LOSS");
        script_lost = 0;
        if (script && sscanf(script, "%lf,%lf", &script_lost, &script_regained) == 2) {
            script_start = CFAbsoluteTimeGetCurrent();
        } else {
            script_lost = 0;
        }
    }
    if (script_lost > 0) {
        double elapsed = CFAbsoluteTimeGetCurrent() - script_start;
        BOOL lost = elapsed >= script_lost && elapsed < script_regained;
        /* Mirror what a real deactivation does to the window (it hides on
           deactivate and is re-shown, then clicked, on reactivation). */
        static int scripted_state;
        NSWindow *window = [self getActiveWindow];
        if (lost && scripted_state == 0) {
            scripted_state = 1;
            [window orderOut:nil];
            fprintf(stderr, "compat32: focus script: window hidden\n");
        } else if (!lost && scripted_state == 1) {
            scripted_state = 2;
            [window orderFront:nil];
            NSPoint where = NSMakePoint(NSMidX([[window contentView] bounds]),
                                        NSMidY([[window contentView] bounds]));
            NSTimeInterval stamp = [[NSProcessInfo processInfo] systemUptime];
            NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                location:where modifierFlags:0 timestamp:stamp
                windowNumber:[window windowNumber] context:nil eventNumber:1
                clickCount:1 pressure:1.0];
            NSEvent *up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                location:where modifierFlags:0 timestamp:stamp + 0.05
                windowNumber:[window windowNumber] context:nil eventNumber:2
                clickCount:1 pressure:0.0];
            [NSApp postEvent:down atStart:NO];
            [NSApp postEvent:up atStart:NO];
            fprintf(stderr, "compat32: focus script: window shown and clicked\n");
        }
        active = !lost;
        key = !lost;
    }
    static int last_state = -1;
    int state = (active ? 2 : 0) | (key ? 1 : 0);
    if (state != last_state) {
        fprintf(stderr, "compat32: game focus %s (app %s, window %s)\n",
                active || key ? "gained" : "lost",
                active ? "active" : "inactive", key ? "key" : "not key");
        last_state = state;
    }
    return active || key;
}
- (void)goFullScreen { _isFullScreen = YES; [[self getActiveWindow] makeKeyAndOrderFront:nil]; }
- (void)goWindowed { _isFullScreen = NO; [[self getActiveWindow] makeKeyAndOrderFront:nil]; }
- (void)close { [[self getActiveWindow] close]; }
- (void)dealloc
{
    [_fullScreenWindow release];
    [_fullScreenOpenGLView release];
    [_window release];
    [_openGLView release];
    [super dealloc];
}
@end

@interface ActivatorAppDelegate : NSObject <NSApplicationDelegate>
{
    NSWindow *_activateOnlineWindow;
    NSWindow *_activateManuallyWindow;
    NSButton *_activateOnlineButton;
    NSButton *_activateManuallyButton;
    NSTextField *_onlineSerialNumber;
    NSTextField *_manualSerialNumber;
    NSTextField *_manualUnlockCode;
    NSTextView *_unlockRequestCode;
    NSTextView *_manualUnlockLink;
    uint32_t _guestObject;
}
@property(nonatomic, retain) IBOutlet NSWindow *activateOnlineWindow;
@property(nonatomic, retain) IBOutlet NSWindow *activateManuallyWindow;
@property(nonatomic, retain) IBOutlet NSButton *activateOnlineButton;
@property(nonatomic, retain) IBOutlet NSButton *activateManuallyButton;
@property(nonatomic, retain) IBOutlet NSTextField *onlineSerialNumber;
@property(nonatomic, retain) IBOutlet NSTextField *manualSerialNumber;
@property(nonatomic, retain) IBOutlet NSTextField *manualUnlockCode;
@property(nonatomic, retain) IBOutlet NSTextView *unlockRequestCode;
@property(nonatomic, retain) IBOutlet NSTextView *manualUnlockLink;
- (IBAction)cancel:(id)sender;
- (IBAction)activateManually:(id)sender;
- (IBAction)activateOnline:(id)sender;
@end

static uint32_t guest_selector(const char *name)
{
    return compat_runtime32_copy_cstring(name);
}

static uint32_t call_guest_method(uint32_t imp, uint32_t self_handle,
                                  const char *selector_name, id argument)
{
    uint32_t arguments[3] = {
        self_handle,
        guest_selector(selector_name),
        argument ? proxy_for_object(argument) : 0,
    };
    uint32_t count = argument ? 3 : 2;
    uint32_t result = compat_runtime32_call(imp, arguments, count);
    if (compat_runtime32_last_call_trapped()) {
        fprintf(stderr, "compat32: guest Objective-C callback %s escaped\n",
                selector_name);
    }
    return result;
}

static const struct lp32_activator_layout *activator_layout(void)
{
    static const struct lp32_activator_layout none;
    const struct lp32_activator_layout *layout = lp32_profile()->activator;
    return layout ? layout : &none;
}

/* Titles without a guest activation delegate never load its nib, but keep
   the outlets harmless if one is instantiated anyway. */
static uint32_t call_activator_method(uint32_t imp, uint32_t self_handle,
                                      const char *selector_name, id argument)
{
    if (!imp || !self_handle) return 0;
    return call_guest_method(imp, self_handle, selector_name, argument);
}

@implementation ActivatorAppDelegate

@synthesize activateOnlineWindow = _activateOnlineWindow;
@synthesize activateManuallyWindow = _activateManuallyWindow;
@synthesize activateOnlineButton = _activateOnlineButton;
@synthesize activateManuallyButton = _activateManuallyButton;
@synthesize onlineSerialNumber = _onlineSerialNumber;
@synthesize manualSerialNumber = _manualSerialNumber;
@synthesize manualUnlockCode = _manualUnlockCode;
@synthesize unlockRequestCode = _unlockRequestCode;
@synthesize manualUnlockLink = _manualUnlockLink;

- (uint32_t)ensureGuestObject
{
    if (!_guestObject) {
        _guestObject = compat_runtime32_allocate(0x28, 1);
        if (_guestObject) {
            uint32_t *words = (void *)(uintptr_t)_guestObject;
            words[0] = lp32_profile()->activator ? lp32_profile()->activator->app_delegate_isa : 0;
            register_proxy_with_handle(self, _guestObject);
        }
    }
    return _guestObject;
}

- (void)setGuestOutlet:(id)object offset:(uint32_t)offset
{
    uint32_t guest = [self ensureGuestObject];
    if (guest) {
        uint32_t *slot = (void *)(uintptr_t)(guest + offset);
        *slot = proxy_for_object(object);
    }
}

- (void)setActivateOnlineWindow:(NSWindow *)value
{
    if (_activateOnlineWindow != value) {
        [_activateOnlineWindow release];
        _activateOnlineWindow = [value retain];
    }
    [self setGuestOutlet:value offset:0x04];
}

- (void)setActivateManuallyWindow:(NSWindow *)value
{
    if (_activateManuallyWindow != value) {
        [_activateManuallyWindow release];
        _activateManuallyWindow = [value retain];
    }
    [self setGuestOutlet:value offset:0x08];
}

- (void)setActivateOnlineButton:(NSButton *)value
{
    if (_activateOnlineButton != value) {
        [_activateOnlineButton release];
        _activateOnlineButton = [value retain];
    }
    [self setGuestOutlet:value offset:0x0c];
}

- (void)setActivateManuallyButton:(NSButton *)value
{
    if (_activateManuallyButton != value) {
        [_activateManuallyButton release];
        _activateManuallyButton = [value retain];
    }
    [self setGuestOutlet:value offset:0x10];
}

- (void)setOnlineSerialNumber:(NSTextField *)value
{
    if (_onlineSerialNumber != value) {
        [_onlineSerialNumber release];
        _onlineSerialNumber = [value retain];
    }
    [self setGuestOutlet:value offset:0x14];
}

- (void)setManualSerialNumber:(NSTextField *)value
{
    if (_manualSerialNumber != value) {
        [_manualSerialNumber release];
        _manualSerialNumber = [value retain];
    }
    [self setGuestOutlet:value offset:0x18];
}

- (void)setManualUnlockCode:(NSTextField *)value
{
    if (_manualUnlockCode != value) {
        [_manualUnlockCode release];
        _manualUnlockCode = [value retain];
    }
    [self setGuestOutlet:value offset:0x1c];
}

- (void)setUnlockRequestCode:(NSTextView *)value
{
    if (_unlockRequestCode != value) {
        [_unlockRequestCode release];
        _unlockRequestCode = [value retain];
    }
    [self setGuestOutlet:value offset:0x20];
}

- (void)setManualUnlockLink:(NSTextView *)value
{
    if (_manualUnlockLink != value) {
        [_manualUnlockLink release];
        _manualUnlockLink = [value retain];
    }
    [self setGuestOutlet:value offset:0x24];
}

- (void)awakeFromNib
{
    call_activator_method(activator_layout()->awake_from_nib, [self ensureGuestObject],
                      "awakeFromNib", nil);
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    call_activator_method(activator_layout()->did_finish_launching, [self ensureGuestObject],
                      "applicationDidFinishLaunching:", notification);
}

- (void)textDidChange:(NSNotification *)notification
{
    call_activator_method(activator_layout()->text_did_change, [self ensureGuestObject],
                      "textDidChange:", notification);
}

- (IBAction)cancel:(id)sender
{
    call_activator_method(activator_layout()->cancel, [self ensureGuestObject],
                      "cancel:", sender);
}

- (IBAction)activateManually:(id)sender
{
    call_activator_method(activator_layout()->activate_manually, [self ensureGuestObject],
                      "activateManually:", sender);
}

- (IBAction)activateOnline:(id)sender
{
    call_activator_method(activator_layout()->activate_online, [self ensureGuestObject],
                      "activateOnline:", sender);
}

- (void)dealloc
{
    [_activateOnlineWindow release];
    [_activateManuallyWindow release];
    [_activateOnlineButton release];
    [_activateManuallyButton release];
    [_onlineSerialNumber release];
    [_manualSerialNumber release];
    [_manualUnlockCode release];
    [_unlockRequestCode release];
    [_manualUnlockLink release];
    [super dealloc];
}

@end

struct guest_rect {
    float x;
    float y;
    float width;
    float height;
};

static float guest_float_argument(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

enum {
    kGuestBufferStateCapacity = 8192,
    kGuestBufferMappingCapacity = 16,
};

/*
 * GL_BUFFER_FLUSHING_UNMAP_APPLE and mapped storage belong to a buffer
 * object, not to its binding target.  The game rotates several large array
 * buffers through GL_ARRAY_BUFFER and also creates level-specific buffers.
 * Keeping one flush flag per target lets those independent objects inherit
 * one another's policy, either dropping an upload or uploading stale bytes.
 * Buffer state is per object while low-address staging is retained only by a
 * small reusable pool of active-map slots, avoiding a duplicate of every
 * static level VBO in the guest heap.
 */
struct guest_buffer_state {
    CGLShareGroupObj share_group;
    GLuint buffer;
    bool flush_on_unmap;
};

struct guest_buffer_mapping {
    CGLContextObj context;
    GLenum target;
    GLenum access;
    GLbitfield access_flags;
    GLuint buffer;
    size_t buffer_size;
    size_t mapped_offset;
    size_t size;
    uint32_t storage;
    size_t capacity;
    struct guest_buffer_state *state;
    bool range_mapping;
    bool active;
};

static struct guest_buffer_state guest_buffer_states[kGuestBufferStateCapacity];
static struct guest_buffer_mapping
    guest_buffer_mappings[kGuestBufferMappingCapacity];
static bool logged_guest_buffer_state_exhaustion;
static bool logged_guest_buffer_mapping_exhaustion;

/*
 * CGDisplayAvailableModes/CGDisplayCurrentMode stopped returning useful
 * objects after macOS 10.6, but this 2011 title consumes their legacy
 * dictionary representation directly.  Rebuild that representation from
 * CGDisplayMode so the guest sees real resolutions on current macOS.
 */
static NSDictionary *legacy_dictionary_for_display_mode(CGDisplayModeRef mode)
{
    if (!mode) return nil;

    size_t width = CGDisplayModeGetWidth(mode);
    size_t height = CGDisplayModeGetHeight(mode);
    double refresh = CGDisplayModeGetRefreshRate(mode);
    if (refresh < 1.0) refresh = 60.0;
    uint32_t flags = CGDisplayModeGetIOFlags(mode);
    uint32_t mode_id = CGDisplayModeGetIODisplayModeID(mode);

    return [NSDictionary dictionaryWithObjectsAndKeys:
        [NSNumber numberWithUnsignedLongLong:width], @"Width",
        [NSNumber numberWithUnsignedLongLong:height], @"Height",
        [NSNumber numberWithUnsignedInt:mode_id], @"Mode",
        [NSNumber numberWithInt:32], @"BitsPerPixel",
        [NSNumber numberWithInt:8], @"BitsPerSample",
        [NSNumber numberWithInt:4], @"SamplesPerPixel",
        [NSNumber numberWithDouble:refresh], @"RefreshRate",
        [NSNumber numberWithBool:CGDisplayModeIsUsableForDesktopGUI(mode)],
            @"UsableForDesktopGUI",
        [NSNumber numberWithUnsignedInt:flags], @"IOFlags",
        [NSNumber numberWithUnsignedLongLong:width * 4],
            @"kCGDisplayBytesPerRow",
        [NSNumber numberWithUnsignedInt:mode_id], @"IODisplayModeID",
        [NSNumber numberWithBool:YES], @"kCGDisplayModeIsSafeForHardware",
        [NSNumber numberWithBool:(flags & kDisplayModeInterlacedFlag) != 0],
            @"kCGDisplayModeIsInterlaced",
        [NSNumber numberWithBool:(flags & kDisplayModeStretchedFlag) != 0],
            @"kCGDisplayModeIsStretched",
        [NSNumber numberWithBool:(flags & kDisplayModeTelevisionFlag) != 0],
            @"kCGDisplayModeIsTelevisionOutput",
        nil];
}

static NSMutableArray *legacy_modes_for_display(CGDirectDisplayID display)
{
    static NSMutableDictionary *cache;
    if (!cache) cache = [[NSMutableDictionary alloc] init];
    NSNumber *display_key = [NSNumber numberWithUnsignedInt:display];
    NSMutableArray *cached = [cache objectForKey:display_key];
    if (cached) return cached;

    CFArrayRef modern_modes = CGDisplayCopyAllDisplayModes(display, NULL);
    NSMutableArray *legacy_modes = [NSMutableArray array];
    NSMutableSet *seen_modes = [NSMutableSet set];
    if (modern_modes) {
        CFIndex count = CFArrayGetCount(modern_modes);
        for (CFIndex index = 0; index < count; ++index) {
            CGDisplayModeRef mode =
                (CGDisplayModeRef)CFArrayGetValueAtIndex(modern_modes, index);
            if (!CGDisplayModeIsUsableForDesktopGUI(mode)) continue;
            size_t width = CGDisplayModeGetWidth(mode);
            size_t height = CGDisplayModeGetHeight(mode);
            double refresh = CGDisplayModeGetRefreshRate(mode);
            if (refresh < 1.0) refresh = 60.0;
            NSString *identity = [NSString stringWithFormat:@"%zux%zu@%.3f",
                                  width, height, refresh];
            if ([seen_modes containsObject:identity]) continue;
            NSDictionary *legacy = legacy_dictionary_for_display_mode(mode);
            if (legacy) {
                [legacy_modes addObject:legacy];
                [seen_modes addObject:identity];
            }
        }
        CFRelease(modern_modes);
    }

    /* The legacy API always listed the active mode.  On Retina displays the
       active (scaled) mode is usually absent from CGDisplayCopyAllDisplayModes
       without the duplicate-low-resolution option, and a game that discards
       every mode larger than the active one is then left with nothing, so
       make sure it is present. */
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(display);
    if (current) {
        size_t width = CGDisplayModeGetWidth(current);
        size_t height = CGDisplayModeGetHeight(current);
        double refresh = CGDisplayModeGetRefreshRate(current);
        if (refresh < 1.0) refresh = 60.0;
        NSString *identity = [NSString stringWithFormat:@"%zux%zu@%.3f",
                              width, height, refresh];
        if (![seen_modes containsObject:identity]) {
            NSDictionary *legacy = legacy_dictionary_for_display_mode(current);
            if (legacy) [legacy_modes addObject:legacy];
        }
        CFRelease(current);
    }
    [cache setObject:legacy_modes forKey:display_key];
    return legacy_modes;
}

/*
 * A requested size the panel does not list.  The emulated switch renders any
 * mode into a scaled surface (see guest_display_mode), so the request can be
 * granted regardless of what the display driver offers: some panels expose
 * only a handful of modes in their own aspect ratio, none of them the 800x600
 * this engine asks for, and falling back to the native mode makes the game
 * lay out a "fullscreen" window the desktop cannot actually hold.  The mode
 * joins the display's list so CGDisplayCurrentMode reports it after the
 * switch, as the real API would.
 */
static NSDictionary *synthesized_legacy_mode(CGDirectDisplayID display,
                                             size_t width, size_t height)
{
    if (!width || !height) return nil;
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(display);
    if (!current) return nil;
    NSMutableDictionary *legacy =
        [[legacy_dictionary_for_display_mode(current) mutableCopy] autorelease];
    CFRelease(current);
    if (!legacy) return nil;
    [legacy setObject:[NSNumber numberWithUnsignedLongLong:width] forKey:@"Width"];
    [legacy setObject:[NSNumber numberWithUnsignedLongLong:height] forKey:@"Height"];
    [legacy setObject:[NSNumber numberWithUnsignedLongLong:width * 4]
               forKey:@"kCGDisplayBytesPerRow"];
    [legacy_modes_for_display(display) addObject:legacy];
    fprintf(stderr,
            "compat32: display %u lists no %zux%zu mode; synthesized one for the "
            "emulated switch\n", display, width, height);
    return legacy;
}

static NSDictionary *legacy_current_mode_for_display(CGDirectDisplayID display)
{
    if (guest_display_mode.width > 0 && display == preferred_game_display_id()) {
        for (NSDictionary *candidate in legacy_modes_for_display(display)) {
            if ([[candidate objectForKey:@"Width"] doubleValue] == guest_display_mode.width &&
                [[candidate objectForKey:@"Height"] doubleValue] == guest_display_mode.height) {
                return candidate;
            }
        }
    }
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(display);
    if (!current) return nil;
    size_t width = CGDisplayModeGetWidth(current);
    size_t height = CGDisplayModeGetHeight(current);
    double refresh = CGDisplayModeGetRefreshRate(current);
    if (refresh < 1.0) refresh = 60.0;

    NSDictionary *match = nil;
    for (NSDictionary *candidate in legacy_modes_for_display(display)) {
        if ([[candidate objectForKey:@"Width"] unsignedLongLongValue] == width &&
            [[candidate objectForKey:@"Height"] unsignedLongLongValue] == height &&
            fabs([[candidate objectForKey:@"RefreshRate"] doubleValue] - refresh) < 0.01) {
            match = candidate;
            break;
        }
    }
    if (!match) match = legacy_dictionary_for_display_mode(current);
    CFRelease(current);
    return match;
}

static GLenum buffer_binding_for_target(GLenum target)
{
    switch (target) {
    case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
    case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
#ifdef GL_PIXEL_PACK_BUFFER
    case GL_PIXEL_PACK_BUFFER: return GL_PIXEL_PACK_BUFFER_BINDING;
#endif
#ifdef GL_PIXEL_UNPACK_BUFFER
    case GL_PIXEL_UNPACK_BUFFER: return GL_PIXEL_UNPACK_BUFFER_BINDING;
#endif
    default: return 0;
    }
}

static GLuint bound_buffer_for_target(GLenum target)
{
    GLenum binding = buffer_binding_for_target(target);
    if (!binding) return 0;
    GLint buffer = 0;
    glGetIntegerv(binding, &buffer);
    return buffer > 0 ? (GLuint)buffer : 0;
}

static struct guest_buffer_state *buffer_state_for_name(
    CGLContextObj context, GLuint buffer, bool create)
{
    if (!buffer) return NULL;
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    struct guest_buffer_state *empty = NULL;
    for (size_t index = 0;
         index < sizeof(guest_buffer_states) / sizeof(guest_buffer_states[0]);
         ++index) {
        struct guest_buffer_state *state = &guest_buffer_states[index];
        if (state->buffer == buffer && state->share_group == share_group) {
            return state;
        }
        if (!state->buffer && !empty) empty = state;
    }
    if (create && empty) {
        empty->share_group = share_group;
        empty->buffer = buffer;
        /* GL_APPLE_flush_buffer_range specifies TRUE as the default. */
        empty->flush_on_unmap = true;
        return empty;
    }
    if (create && !logged_guest_buffer_state_exhaustion) {
        fprintf(stderr,
                "compat32: guest buffer state table exhausted at swap %llu\n",
                (unsigned long long)objc_bridge_swap_count);
        logged_guest_buffer_state_exhaustion = true;
    }
    return NULL;
}

static struct guest_buffer_mapping *active_buffer_mapping(
    CGLContextObj context, GLenum target, GLuint buffer)
{
    for (size_t index = 0;
         index < sizeof(guest_buffer_mappings) / sizeof(guest_buffer_mappings[0]);
         ++index) {
        struct guest_buffer_mapping *mapping = &guest_buffer_mappings[index];
        if (mapping->active && mapping->context == context &&
            mapping->target == target && mapping->buffer == buffer) return mapping;
    }
    return NULL;
}

static struct guest_buffer_mapping *begin_buffer_mapping(
    CGLContextObj context, GLenum target, GLuint buffer,
    struct guest_buffer_state *state)
{
    struct guest_buffer_mapping *mapping =
        active_buffer_mapping(context, target, buffer);
    if (mapping) return mapping;
    for (size_t index = 0;
         index < sizeof(guest_buffer_mappings) / sizeof(guest_buffer_mappings[0]);
         ++index) {
        if (!guest_buffer_mappings[index].active) {
            mapping = &guest_buffer_mappings[index];
            uint32_t storage = mapping->storage;
            size_t capacity = mapping->capacity;
            memset(mapping, 0, sizeof(*mapping));
            mapping->storage = storage;
            mapping->capacity = capacity;
            mapping->context = context;
            mapping->target = target;
            mapping->buffer = buffer;
            mapping->state = state;
            mapping->active = true;
            return mapping;
        }
    }
    if (!logged_guest_buffer_mapping_exhaustion) {
        fprintf(stderr,
                "compat32: guest mapped-buffer table exhausted at swap %llu\n",
                (unsigned long long)objc_bridge_swap_count);
        logged_guest_buffer_mapping_exhaustion = true;
    }
    return NULL;
}

static void deactivate_buffer_mapping(struct guest_buffer_mapping *mapping)
{
    if (!mapping) return;
    mapping->context = NULL;
    mapping->target = 0;
    mapping->access = 0;
    mapping->access_flags = 0;
    mapping->buffer = 0;
    mapping->buffer_size = 0;
    mapping->mapped_offset = 0;
    mapping->size = 0;
    mapping->state = NULL;
    mapping->range_mapping = false;
    mapping->active = false;
}

static void discard_buffer_state(CGLContextObj context, GLuint buffer)
{
    if (!buffer) return;
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    for (size_t index = 0;
         index < sizeof(guest_buffer_mappings) / sizeof(guest_buffer_mappings[0]);
         ++index) {
        if (guest_buffer_mappings[index].active &&
            guest_buffer_mappings[index].buffer == buffer &&
            guest_buffer_mappings[index].state &&
            guest_buffer_mappings[index].state->share_group == share_group) {
            deactivate_buffer_mapping(&guest_buffer_mappings[index]);
        }
    }
    for (size_t index = 0;
         index < sizeof(guest_buffer_states) / sizeof(guest_buffer_states[0]);
         ++index) {
        struct guest_buffer_state *state = &guest_buffer_states[index];
        if (state->buffer != buffer || state->share_group != share_group) continue;
        memset(state, 0, sizeof(*state));
    }
}

static bool trace_gl_buffer_call(void)
{
    static bool initialized;
    static uint64_t first_swap;
    static unsigned int remaining;
    if (!initialized) {
        const char *value = getenv("LP32_TRACE_GL_BUFFERS");
        if (value) {
            first_swap = strtoull(value, NULL, 0);
            remaining = 1000;
        }
        initialized = true;
    }
    if (!remaining || objc_bridge_swap_count < first_swap) return false;
    --remaining;
    return true;
}

static bool gl_render_trace_initialized;
static uint64_t gl_render_trace_first_swap;
static unsigned int gl_render_trace_remaining;
static unsigned int gl_render_trace_limit = 20000;
static uint64_t gl_render_trace_generation;

enum { kTracedTextureCapacity = 4096 };
struct traced_texture_entry {
    CGLShareGroupObj share_group;
    GLenum target;
    GLuint texture;
    uint64_t generation;
};
static struct traced_texture_entry
    traced_textures[kTracedTextureCapacity];

static void initialize_gl_render_trace(void)
{
    if (!gl_render_trace_initialized) {
        const char *value = getenv("LP32_TRACE_GL_RENDER");
        const char *limit = getenv("LP32_TRACE_GL_RENDER_LIMIT");
        unsigned long parsed_limit = limit ? strtoul(limit, NULL, 0) : 0;
        if (parsed_limit > 0 && parsed_limit <= UINT_MAX) {
            gl_render_trace_limit = (unsigned int)parsed_limit;
        }
        if (value) {
            gl_render_trace_first_swap = strtoull(value, NULL, 0);
            gl_render_trace_remaining = gl_render_trace_limit;
        }
        gl_render_trace_initialized = true;
    }
}

static void arm_pending_gl_trace(void)
{
    initialize_gl_render_trace();
    if (!pending_gl_trace_arm) return;
    pending_gl_trace_arm = 0;
    gl_render_trace_first_swap = objc_bridge_swap_count;
    gl_render_trace_remaining = gl_render_trace_limit;
    ++gl_render_trace_generation;
    if (!gl_render_trace_generation) ++gl_render_trace_generation;
    memset(traced_textures, 0, sizeof(traced_textures));
    gl_trace_printf(
        "compat32: GL render trace armed at swap %llu for %u calls\n",
        (unsigned long long)objc_bridge_swap_count,
        gl_render_trace_remaining);
}

static bool trace_gl_render_call(void)
{
    /* Called from every GL fast handler, tens of thousands of times a frame,
       and almost always with tracing off.  Settle that with one branch. */
    if (__builtin_expect(gl_render_trace_initialized &&
                         !gl_render_trace_remaining && !pending_gl_trace_arm, 1))
        return false;
    initialize_gl_render_trace();
    arm_pending_gl_trace();
    if (!gl_render_trace_remaining ||
        objc_bridge_swap_count < gl_render_trace_first_swap) return false;
    --gl_render_trace_remaining;
    return true;
}

static void trace_gl_frame_boundary(void)
{
    static bool initialized;
    static uint64_t interval;
    if (!initialized) {
        const char *value = getenv("LP32_TRACE_GL_FRAMES");
        if (value) {
            unsigned long long parsed = strtoull(value, NULL, 0);
            interval = parsed > 0 ? parsed : 30;
        }
        initialized = true;
    }
    if (interval &&
        (objc_bridge_swap_count <= 10 || objc_bridge_swap_count % interval == 0)) {
        GLint framebuffer = 0;
        GLint framebuffer_status = GL_FRAMEBUFFER_COMPLETE;
        GLint viewport[4] = {0, 0, 0, 0};
        GLint scissor[4] = {0, 0, 0, 0};
        GLint draw_buffer = 0;
        GLint read_buffer = 0;
        GLint active_texture = 0;
        GLint texture_2d = 0;
        GLboolean color_mask[4] = {0, 0, 0, 0};
        GLboolean depth_mask = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
        glGetIntegerv(GL_READ_BUFFER, &read_buffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_2d);
        glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        if (framebuffer) {
            framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        }
        fprintf(stderr,
                "compat32: GL frame swap=%llu draws=%llu clears=%llu "
                "fboBinds=%llu fboAttach=%llu blits=%llu texBinds=%llu "
                "texUploads=%llu progBinds=%llu params=%llu fbo=%d "
                "status=%04x viewport={%d,%d,%d,%d} scissor={%d,%d,%d,%d} "
                "drawbuf=%04x readbuf=%04x activeTex=%04x tex2d=%d "
                "colorMask=%d%d%d%d depthMask=%d vp=%u fp=%u\n",
                (unsigned long long)objc_bridge_swap_count,
                (unsigned long long)gl_frame_diagnostics.draw_calls,
                (unsigned long long)gl_frame_diagnostics.clear_calls,
                (unsigned long long)gl_frame_diagnostics.framebuffer_binds,
                (unsigned long long)gl_frame_diagnostics.framebuffer_attachments,
                (unsigned long long)gl_frame_diagnostics.framebuffer_blits,
                (unsigned long long)gl_frame_diagnostics.texture_binds,
                (unsigned long long)gl_frame_diagnostics.texture_uploads,
                (unsigned long long)gl_frame_diagnostics.program_binds,
                (unsigned long long)gl_frame_diagnostics.parameter_uploads,
                framebuffer, framebuffer_status,
                viewport[0], viewport[1], viewport[2], viewport[3],
                scissor[0], scissor[1], scissor[2], scissor[3],
                draw_buffer, read_buffer, active_texture, texture_2d,
                color_mask[0], color_mask[1], color_mask[2], color_mask[3],
                depth_mask, trace_vertex_program, trace_fragment_program);
        fflush(stderr);
    }
    memset(&gl_frame_diagnostics, 0, sizeof(gl_frame_diagnostics));
}

/*
 * Reports an ARB program the driver rejected.  Reads only the error position
 * (a glGetIntegerv query), never glGetError, so the game's own error check
 * after glProgramStringARB still sees the failure.
 */
static void report_arb_program_load_failure(GLenum target, const void *source,
                                            GLsizei source_size)
{
    GLint position = -1;
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &position);
    if (position < 0) return;
    static unsigned int reports;
    if (reports >= 16) return;
    ++reports;
    const GLubyte *message = glGetString(GL_PROGRAM_ERROR_STRING_ARB);
    const char *text = source;
    int line = 1;
    for (GLint index = 0; index < position && index < source_size; ++index) {
        if (text[index] == '\n') ++line;
    }
    GLuint identifier = target == GL_VERTEX_PROGRAM_ARB ? trace_vertex_program :
                        target == GL_FRAGMENT_PROGRAM_ARB ? trace_fragment_program : 0;
    fprintf(stderr, "compat32: ARB program rejected target=%04x id=%u bytes=%d "
            "position=%d line=%d: %s\n", target, identifier, source_size,
            position, line, message ? (const char *)message : "(no message)");
    if (position < source_size) {
        const char *start = text + position;
        while (start > text && start[-1] != '\n') --start;
        const char *end = memchr(start, '\n', (size_t)(source_size - (start - text)));
        fprintf(stderr, "compat32:   %.*s\n",
                (int)((end ? end : text + source_size) - start), start);
    }
}

static void maybe_dump_gl_program(GLenum target, GLsizei length,
                                  const void *program)
{
    initialize_gl_trace_output();
    const char *directory = getenv("LP32_DUMP_GL_PROGRAMS");
    if (!directory || !*directory) directory = gl_program_dump_directory;
    if (!directory || !*directory || length < 0 || !program) return;

    if (mkdir(directory, 0700) != 0) {
        struct stat status;
        if (stat(directory, &status) != 0 || !S_ISDIR(status.st_mode)) {
            gl_trace_printf(
                "compat32: cannot create GL program dump directory %s\n",
                directory);
            return;
        }
    }

    GLuint identifier = 0;
    if (target == GL_VERTEX_PROGRAM_ARB) {
        identifier = trace_vertex_program;
    } else if (target == GL_FRAGMENT_PROGRAM_ARB) {
        identifier = trace_fragment_program;
    }

    char path[1024];
    int path_length = snprintf(path, sizeof(path),
                               "%s/program-%04x-%u.arb", directory,
                               target, identifier);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        gl_trace_printf("compat32: GL program dump path is too long\n");
        return;
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        gl_trace_printf("compat32: cannot open GL program dump %s\n", path);
        return;
    }
    size_t expected = (size_t)length;
    size_t written = fwrite(program, 1, expected, file);
    if (fclose(file) != 0 || written != expected) {
        gl_trace_printf("compat32: incomplete GL program dump %s\n", path);
    }
}

static void trace_program_parameter_upload(const char *kind, GLenum target,
                                           GLuint index, GLsizei count,
                                           const GLfloat *values)
{
    ++gl_frame_diagnostics.parameter_uploads;
    if (!trace_gl_render_call()) return;
    bool finite = true;
    size_t value_count = count > 0 ? (size_t)count * 4 : 0;
    size_t first_nonfinite = SIZE_MAX;
    float largest_magnitude = 0.0f;
    for (size_t offset = 0; offset < value_count; ++offset) {
        if (!isfinite(values[offset])) {
            finite = false;
            if (first_nonfinite == SIZE_MAX) first_nonfinite = offset;
        } else if (fabsf(values[offset]) > largest_magnitude) {
            largest_magnitude = fabsf(values[offset]);
        }
    }
    uint32_t nonfinite_bits = 0;
    if (first_nonfinite != SIZE_MAX) {
        memcpy(&nonfinite_bits, values + first_nonfinite,
               sizeof(nonfinite_bits));
    }
    const GLfloat *last = value_count >= 4 ? values + value_count - 4 : NULL;
    gl_trace_printf(
        "compat32: %s swap=%llu target=%04x index=%u count=%d "
        "vp=%u fp=%u finite=%d bad=%zu badbits=%08x maxabs=%.7g "
        "first={%.7g,%.7g,%.7g,%.7g} "
        "last={%.7g,%.7g,%.7g,%.7g}\n",
        kind, (unsigned long long)objc_bridge_swap_count, target, index,
        count, trace_vertex_program, trace_fragment_program, finite,
        first_nonfinite, nonfinite_bits, largest_magnitude,
        value_count >= 4 ? values[0] : 0.0f,
        value_count >= 4 ? values[1] : 0.0f,
        value_count >= 4 ? values[2] : 0.0f,
        value_count >= 4 ? values[3] : 0.0f,
        last ? last[0] : 0.0f, last ? last[1] : 0.0f,
        last ? last[2] : 0.0f, last ? last[3] : 0.0f);
}

/*
 * The game was built around GL_EXT_gpu_program_parameters and deliberately
 * submits whole Cg constant banks in one call.  Preserve that atomic update:
 * splitting a bank into individual ARB calls has produced corrupt legacy
 * program state in Apple's current compatibility renderer.
 *
 * Old Cg parameter banks also contain NaN/Inf sentinels in unused vector
 * lanes.  Those lanes were benign on the period NVIDIA/ATI drivers, but are
 * undefined input to the current backend and can contaminate position or HDR
 * calculations when a generated program unexpectedly consumes one.  Replace
 * only non-finite lanes; ordinary values, including very large finite matrix
 * coefficients, remain bit-for-bit unchanged.
 */
enum { kProgramParameterScratchFloats = 4096 };
static _Thread_local GLfloat
    program_parameter_scratch[kProgramParameterScratchFloats];
static unsigned int program_parameter_sanitize_reports;

static bool preserve_nonfinite_program_parameters(void)
{
    static int initialized;
    static bool preserve;
    if (!initialized) {
        preserve = getenv("LP32_PRESERVE_NONFINITE_GL_PARAMETERS") != NULL;
        initialized = 1;
    }
    return preserve;
}

static bool split_program_parameter_batches(void)
{
    static int initialized;
    static bool split;
    if (!initialized) {
        split = getenv("LP32_SPLIT_GL_PARAMETER_BATCH") != NULL;
        initialized = 1;
    }
    return split;
}

static bool guard_undefined_arb_math(void)
{
    static bool initialized;
    static bool enabled;
    if (!initialized) {
        enabled = getenv("LP32_DISABLE_ARB_MATH_GUARD") == NULL &&
            getenv("LP32_DISABLE_ARB_RSQ_GUARD") == NULL;
        initialized = true;
    }
    return enabled;
}

static const GLfloat *safe_program_parameter_values(
    const GLfloat *values, GLsizei count, size_t *replaced)
{
    *replaced = 0;
    if (!values || count <= 0 || preserve_nonfinite_program_parameters()) {
        return values;
    }

    size_t value_count = (size_t)count * 4;
    if (value_count > kProgramParameterScratchFloats) {
        if (program_parameter_sanitize_reports < 8) {
            fprintf(stderr,
                    "compat32: refusing oversized GL parameter sanitize "
                    "count=%d\n", count);
            ++program_parameter_sanitize_reports;
        }
        return values;
    }

    size_t first_nonfinite = SIZE_MAX;
    for (size_t offset = 0; offset < value_count; ++offset) {
        if (!isfinite(values[offset])) {
            first_nonfinite = offset;
            break;
        }
    }
    if (first_nonfinite == SIZE_MAX) return values;

    memcpy(program_parameter_scratch, values,
           value_count * sizeof(*values));
    for (size_t offset = first_nonfinite; offset < value_count; ++offset) {
        if (!isfinite(program_parameter_scratch[offset])) {
            program_parameter_scratch[offset] = 0.0f;
            ++*replaced;
        }
    }
    return program_parameter_scratch;
}

static void report_sanitized_program_parameters(
    const char *kind, GLenum target, GLuint index, GLsizei count,
    size_t replaced)
{
    if (!replaced || program_parameter_sanitize_reports >= 8) return;
    fprintf(stderr,
            "compat32: sanitized %zu non-finite %s component%s "
            "swap=%llu target=%04x index=%u count=%d vp=%u fp=%u\n",
            replaced, kind, replaced == 1 ? "" : "s",
            (unsigned long long)objc_bridge_swap_count, target, index, count,
            trace_vertex_program, trace_fragment_program);
    ++program_parameter_sanitize_reports;
}

static size_t gl_attribute_component_size(GLenum type)
{
    switch (type) {
        case GL_BYTE:
        case GL_UNSIGNED_BYTE: return 1;
        case GL_SHORT:
        case GL_UNSIGNED_SHORT:
        case GL_HALF_FLOAT: return 2;
        case GL_INT:
        case GL_UNSIGNED_INT:
        case GL_FLOAT:
        case GL_UNSIGNED_INT_2_10_10_10_REV: return 4;
        case GL_DOUBLE: return 8;
        default: return 0;
    }
}

static size_t gl_index_component_size(GLenum type)
{
    switch (type) {
        case GL_UNSIGNED_BYTE: return 1;
        case GL_UNSIGNED_SHORT: return 2;
        case GL_UNSIGNED_INT: return 4;
        default: return 0;
    }
}

static double gl_half_to_double(uint16_t bits)
{
    unsigned int sign = bits >> 15;
    unsigned int exponent = (bits >> 10) & 0x1f;
    unsigned int fraction = bits & 0x3ff;
    double value;

    if (exponent == 0) {
        value = fraction ? ldexp((double)fraction, -24) : 0.0;
    } else if (exponent == 0x1f) {
        value = fraction ? NAN : INFINITY;
    } else {
        value = ldexp(1.0 + (double)fraction / 1024.0,
                      (int)exponent - 15);
    }
    return sign ? -value : value;
}

static uint64_t trace_hash_bytes(uint64_t hash, const void *bytes,
                                 size_t byte_count)
{
    const unsigned char *cursor = bytes;
    for (size_t index = 0; index < byte_count; ++index) {
        hash ^= cursor[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static GLuint read_index_value(const unsigned char *bytes, GLenum type,
                               size_t index)
{
    switch (type) {
        case GL_UNSIGNED_BYTE: return bytes[index];
        case GL_UNSIGNED_SHORT: {
            uint16_t value;
            memcpy(&value, bytes + index * sizeof(value), sizeof(value));
            return value;
        }
        case GL_UNSIGNED_INT: {
            uint32_t value;
            memcpy(&value, bytes + index * sizeof(value), sizeof(value));
            return value;
        }
        default: return 0;
    }
}

struct gl_index_diagnostic {
    GLuint minimum;
    GLuint maximum;
    bool readable;
    bool in_bounds;
};

static struct gl_index_diagnostic trace_index_buffer(
    GLsizei count, GLenum type, uintptr_t offset, GLuint advertised_start,
    GLuint advertised_end, GLint element_buffer)
{
    struct gl_index_diagnostic diagnostic = {
        .minimum = advertised_start,
        .maximum = advertised_end,
        .readable = false,
        .in_bounds = false,
    };
    size_t component_size = gl_index_component_size(type);
    GLint buffer_size = 0;
    if (element_buffer) {
        glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE,
                               &buffer_size);
    }
    bool byte_count_valid = count >= 0 && component_size != 0 &&
        (size_t)count <= SIZE_MAX / component_size;
    size_t byte_count = byte_count_valid ?
        (size_t)count * component_size : 0;
    diagnostic.in_bounds = element_buffer && buffer_size >= 0 &&
        byte_count_valid && offset <= (size_t)buffer_size &&
        byte_count <= (size_t)buffer_size - offset;

    unsigned char *indices = NULL;
    if (diagnostic.in_bounds && byte_count && byte_count <= 8 * 1024 * 1024) {
        indices = malloc(byte_count);
        if (indices) {
            glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)offset,
                               (GLsizeiptr)byte_count, indices);
            diagnostic.minimum = UINT_MAX;
            diagnostic.maximum = 0;
            for (GLsizei index = 0; index < count; ++index) {
                GLuint value = read_index_value(indices, type, (size_t)index);
                if (value < diagnostic.minimum) diagnostic.minimum = value;
                if (value > diagnostic.maximum) diagnostic.maximum = value;
            }
            if (count == 0) diagnostic.minimum = diagnostic.maximum = 0;
            diagnostic.readable = true;
        }
    }
    free(indices);

    gl_trace_printf(
        "compat32: index-state swap=%llu ibo=%d type=%04x offset=%zu "
        "count=%d bytes=%zu bufferSize=%d inBounds=%d readable=%d "
        "actual={%u,%u} advertised={%u,%u} advertisedContains=%d\n",
        (unsigned long long)objc_bridge_swap_count, element_buffer, type,
        (size_t)offset, count, byte_count, buffer_size,
        diagnostic.in_bounds, diagnostic.readable, diagnostic.minimum,
        diagnostic.maximum, advertised_start, advertised_end,
        !diagnostic.readable ||
            (diagnostic.minimum >= advertised_start &&
             diagnostic.maximum <= advertised_end));
    return diagnostic;
}

static void trace_vertex_attributes(GLuint minimum, GLuint maximum)
{
    GLint previous_array_buffer = 0;
    GLint maximum_attributes = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_array_buffer);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maximum_attributes);
    if (maximum_attributes < 0) maximum_attributes = 0;
    if (maximum_attributes > 32) maximum_attributes = 32;

    for (GLuint index = 0; index < (GLuint)maximum_attributes; ++index) {
        GLint enabled = 0;
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        if (!enabled) continue;

        GLint size = 0;
        GLint type = 0;
        GLint normalized = 0;
        GLint stride = 0;
        GLint buffer = 0;
        GLvoid *raw_pointer = NULL;
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                            &normalized);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                            &buffer);
        glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                  &raw_pointer);

        size_t component_size = gl_attribute_component_size((GLenum)type);
        size_t element_size = size > 0 && component_size &&
            (size_t)size <= SIZE_MAX / component_size ?
            (size_t)size * component_size : 0;
        size_t effective_stride = stride > 0 ? (size_t)stride : element_size;
        size_t pointer = (size_t)(uintptr_t)raw_pointer;
        GLint buffer_size = 0;
        if (buffer > 0) {
            glBindBuffer(GL_ARRAY_BUFFER, (GLuint)buffer);
            glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE,
                                   &buffer_size);
        }

        bool range_valid = maximum >= minimum && element_size &&
            effective_stride &&
            maximum <= (SIZE_MAX - pointer - element_size) /
                           effective_stride;
        size_t final_byte = range_valid ?
            pointer + (size_t)maximum * effective_stride + element_size : 0;
        bool in_bounds = buffer > 0 && buffer_size >= 0 && range_valid &&
            final_byte <= (size_t)buffer_size;

        size_t nonfinite = 0;
        double minimum_value = INFINITY;
        double maximum_value = -INFINITY;
        float maximum_magnitude = 0.0f;
        unsigned int minimum_byte = UINT_MAX;
        unsigned int maximum_byte = 0;
        uint64_t content_hash = UINT64_C(1469598103934665603);
        bool scanned = false;
        if (in_bounds && size > 0 && size <= 4) {
            size_t first_byte = pointer + (size_t)minimum * effective_stride;
            size_t scan_size = final_byte - first_byte;
            if (scan_size && scan_size <= 2 * 1024 * 1024) {
                unsigned char *data = malloc(scan_size);
                if (data) {
                    glGetBufferSubData(GL_ARRAY_BUFFER,
                                       (GLintptr)first_byte,
                                       (GLsizeiptr)scan_size, data);
                    for (GLuint vertex = minimum; vertex <= maximum; ++vertex) {
                        size_t relative =
                            (size_t)(vertex - minimum) * effective_stride;
                        content_hash = trace_hash_bytes(
                            content_hash, data + relative, element_size);
                        if (type == GL_FLOAT || type == GL_HALF_FLOAT) {
                            for (GLint component = 0; component < size;
                                 ++component) {
                                double value;
                                if (type == GL_FLOAT) {
                                    float float_value;
                                    memcpy(&float_value,
                                           data + relative +
                                               (size_t)component *
                                                   sizeof(float_value),
                                           sizeof(float_value));
                                    value = float_value;
                                } else {
                                    uint16_t half_value;
                                    memcpy(&half_value,
                                           data + relative +
                                               (size_t)component *
                                                   sizeof(half_value),
                                           sizeof(half_value));
                                    value = gl_half_to_double(half_value);
                                }
                                if (!isfinite(value)) {
                                    ++nonfinite;
                                    continue;
                                }
                                if (value < minimum_value) minimum_value = value;
                                if (value > maximum_value) maximum_value = value;
                                if (fabs(value) > maximum_magnitude) {
                                    maximum_magnitude = (float)fabs(value);
                                }
                            }
                        } else if (type == GL_UNSIGNED_BYTE ||
                                   type == GL_BYTE) {
                            for (size_t component = 0;
                                 component < element_size; ++component) {
                                unsigned int value = data[relative + component];
                                if (value < minimum_byte) minimum_byte = value;
                                if (value > maximum_byte) maximum_byte = value;
                            }
                        }
                        if (vertex == UINT_MAX) break;
                    }
                    scanned = true;
                    free(data);
                }
            }
        }

        gl_trace_printf(
            "compat32: attrib-state swap=%llu index=%u size=%d type=%04x "
            "normalized=%d stride=%d effectiveStride=%zu pointer=%zu "
            "buffer=%d bufferSize=%d vertices={%u,%u} finalByte=%zu "
            "inBounds=%d contentScan=%d nonfinite=%zu min=%.9g max=%.9g "
            "maxabs=%.7g byteRange={%u,%u} hash=%016llx\n",
            (unsigned long long)objc_bridge_swap_count, index, size, type,
            normalized, stride, effective_stride, pointer, buffer,
            buffer_size, minimum, maximum, final_byte, in_bounds, scanned,
            nonfinite, isfinite(minimum_value) ? minimum_value : 0.0,
            isfinite(maximum_value) ? maximum_value : 0.0,
            maximum_magnitude, minimum_byte == UINT_MAX ? 0 : minimum_byte,
            maximum_byte, (unsigned long long)(scanned ? content_hash : 0));
    }
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)previous_array_buffer);
}

static bool mark_texture_for_trace(GLenum target, GLuint texture)
{
    if (!texture || !gl_render_trace_generation) return false;
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    size_t start = ((size_t)texture * 2654435761u + (size_t)target) %
        kTracedTextureCapacity;
    for (size_t probe = 0; probe < kTracedTextureCapacity; ++probe) {
        struct traced_texture_entry *entry =
            &traced_textures[(start + probe) % kTracedTextureCapacity];
        if (entry->generation != gl_render_trace_generation) {
            entry->share_group = share_group;
            entry->target = target;
            entry->texture = texture;
            entry->generation = gl_render_trace_generation;
            return true;
        }
        if (entry->share_group == share_group && entry->target == target &&
            entry->texture == texture) {
            return false;
        }
    }
    return false;
}

static bool texture_filter_needs_mipmaps(GLint filter)
{
    return filter == GL_NEAREST_MIPMAP_NEAREST ||
        filter == GL_LINEAR_MIPMAP_NEAREST ||
        filter == GL_NEAREST_MIPMAP_LINEAR ||
        filter == GL_LINEAR_MIPMAP_LINEAR;
}

static void trace_texture_2d_content(GLint unit, GLuint texture,
                                     GLint min_filter)
{
    if (!mark_texture_for_trace(GL_TEXTURE_2D, texture)) return;

    GLint base_level = 0;
    GLint maximum_level = 0;
    GLint wrap_s = 0;
    GLint wrap_t = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base_level);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maximum_level);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap_s);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrap_t);

    GLint base_width = 0;
    GLint base_height = 0;
    GLint base_format = 0;
    GLint base_compressed = 0;
    GLint compressed_size = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, base_level, GL_TEXTURE_WIDTH,
                             &base_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, base_level, GL_TEXTURE_HEIGHT,
                             &base_height);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, base_level,
                             GL_TEXTURE_INTERNAL_FORMAT, &base_format);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, base_level, GL_TEXTURE_COMPRESSED,
                             &base_compressed);
    if (base_compressed) {
        glGetTexLevelParameteriv(GL_TEXTURE_2D, base_level,
                                 GL_TEXTURE_COMPRESSED_IMAGE_SIZE,
                                 &compressed_size);
    }

    GLint required_last = base_level;
    if (texture_filter_needs_mipmaps(min_filter)) {
        GLint dimension = base_width > base_height ? base_width : base_height;
        while (dimension > 1 && required_last < maximum_level) {
            dimension >>= 1;
            ++required_last;
        }
    }
    GLint expected_width = base_width;
    GLint expected_height = base_height;
    GLint first_missing = -1;
    GLint first_wrong_size = -1;
    unsigned int levels_present = 0;
    for (GLint level = base_level; level <= required_last; ++level) {
        GLint width = 0;
        GLint height = 0;
        GLint format = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH,
                                 &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT,
                                 &height);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level,
                                 GL_TEXTURE_INTERNAL_FORMAT, &format);
        if (width <= 0 || height <= 0 || !format) {
            if (first_missing < 0) first_missing = level;
        } else {
            ++levels_present;
            if ((width != expected_width || height != expected_height ||
                 format != base_format) && first_wrong_size < 0) {
                first_wrong_size = level;
            }
        }
        if (expected_width > 1) expected_width >>= 1;
        if (expected_height > 1) expected_height >>= 1;
    }

    uint64_t hash = 0;
    if (base_compressed && compressed_size > 0 &&
        compressed_size <= 16 * 1024 * 1024) {
        void *bytes = malloc((size_t)compressed_size);
        if (bytes) {
            GLint previous_pack_buffer = 0;
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,
                          &previous_pack_buffer);
            if (previous_pack_buffer) {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            }
            glGetCompressedTexImage(GL_TEXTURE_2D, base_level, bytes);
            if (previous_pack_buffer) {
                glBindBuffer(GL_PIXEL_PACK_BUFFER,
                             (GLuint)previous_pack_buffer);
            }
            hash = trace_hash_bytes(UINT64_C(1469598103934665603), bytes,
                                    (size_t)compressed_size);
            free(bytes);
        }
    }
    gl_trace_printf(
        "compat32: texture-content swap=%llu unit=%d target=0de1 tex=%u "
        "base=%d max=%d requiredLast=%d levelsPresent=%u missing=%d "
        "wrongSize=%d baseSize=%dx%d format=%04x compressed=%d "
        "compressedBytes=%d min=%04x wrap={%04x,%04x} hash=%016llx\n",
        (unsigned long long)objc_bridge_swap_count, unit, texture,
        base_level, maximum_level, required_last, levels_present,
        first_missing, first_wrong_size, base_width, base_height, base_format,
        base_compressed, compressed_size, min_filter, wrap_s, wrap_t,
        (unsigned long long)hash);
}

static void trace_texture_cube_content(GLint unit, GLuint texture,
                                       GLint min_filter)
{
    if (!mark_texture_for_trace(GL_TEXTURE_CUBE_MAP, texture)) return;

    GLint base_level = 0;
    GLint maximum_level = 0;
    GLint wrap_s = 0;
    GLint wrap_t = 0;
    glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL,
                        &base_level);
    glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL,
                        &maximum_level);
    glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, &wrap_s);
    glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, &wrap_t);

    GLint base_width = 0;
    GLint base_height = 0;
    GLint base_format = 0;
    GLint base_compressed = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, base_level,
                             GL_TEXTURE_WIDTH, &base_width);
    glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, base_level,
                             GL_TEXTURE_HEIGHT, &base_height);
    glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, base_level,
                             GL_TEXTURE_INTERNAL_FORMAT, &base_format);
    glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, base_level,
                             GL_TEXTURE_COMPRESSED, &base_compressed);

    GLint required_last = base_level;
    if (texture_filter_needs_mipmaps(min_filter)) {
        GLint dimension = base_width > base_height ? base_width : base_height;
        while (dimension > 1 && required_last < maximum_level) {
            dimension >>= 1;
            ++required_last;
        }
    }
    GLint expected_width = base_width;
    GLint expected_height = base_height;
    GLint first_missing = -1;
    GLint first_wrong_size = -1;
    unsigned int levels_present = 0;
    for (GLint level = base_level; level <= required_last; ++level) {
        bool complete_level = true;
        for (unsigned int face = 0; face < 6; ++face) {
            GLenum face_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
            GLint width = 0;
            GLint height = 0;
            GLint format = 0;
            glGetTexLevelParameteriv(face_target, level, GL_TEXTURE_WIDTH,
                                     &width);
            glGetTexLevelParameteriv(face_target, level, GL_TEXTURE_HEIGHT,
                                     &height);
            glGetTexLevelParameteriv(face_target, level,
                                     GL_TEXTURE_INTERNAL_FORMAT, &format);
            if (width <= 0 || height <= 0 || !format) {
                if (first_missing < 0) first_missing = level;
                complete_level = false;
            } else if (width != expected_width ||
                       height != expected_height || format != base_format) {
                if (first_wrong_size < 0) first_wrong_size = level;
                complete_level = false;
            }
        }
        if (complete_level) ++levels_present;
        if (expected_width > 1) expected_width >>= 1;
        if (expected_height > 1) expected_height >>= 1;
    }

    uint64_t hash = UINT64_C(1469598103934665603);
    GLint compressed_size = 0;
    if (base_compressed) {
        GLint previous_pack_buffer = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous_pack_buffer);
        if (previous_pack_buffer) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        for (unsigned int face = 0; face < 6; ++face) {
            GLenum face_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
            GLint face_size = 0;
            glGetTexLevelParameteriv(face_target, base_level,
                                     GL_TEXTURE_COMPRESSED_IMAGE_SIZE,
                                     &face_size);
            if (face_size <= 0 || face_size > 16 * 1024 * 1024) continue;
            void *bytes = malloc((size_t)face_size);
            if (!bytes) continue;
            glGetCompressedTexImage(face_target, base_level, bytes);
            hash = trace_hash_bytes(hash, bytes, (size_t)face_size);
            compressed_size += face_size;
            free(bytes);
        }
        if (previous_pack_buffer) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)previous_pack_buffer);
        }
    } else {
        hash = 0;
    }

    gl_trace_printf(
        "compat32: texture-content swap=%llu unit=%d target=8513 tex=%u "
        "base=%d max=%d requiredLast=%d levelsPresent=%u missing=%d "
        "wrongSize=%d baseSize=%dx%d format=%04x compressed=%d "
        "compressedBytes=%d min=%04x wrap={%04x,%04x} hash=%016llx\n",
        (unsigned long long)objc_bridge_swap_count, unit, texture,
        base_level, maximum_level, required_last, levels_present,
        first_missing, first_wrong_size, base_width, base_height, base_format,
        base_compressed, compressed_size, min_filter, wrap_s, wrap_t,
        (unsigned long long)hash);
}

enum { kMipmapRepairCapacity = 8192 };
struct mipmap_repair_entry {
    CGLShareGroupObj share_group;
    GLenum target;
    GLuint texture;
    GLint requested_maximum;
    GLint compatibility_maximum;
    /* State the last full scan saw; lets a repeated glTexParameteri with
       the same value skip the rescan (see repair_mipmap_range_for_parameter). */
    uint64_t validated_generation;
    GLint validated_min_filter;
    GLint validated_base_level;
    bool occupied;
    bool clamped;
};
static struct mipmap_repair_entry mipmap_repairs[kMipmapRepairCapacity];
static pthread_mutex_t mipmap_repair_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int mipmap_repair_reports;
/* Bumped by every call that can add or resize a mip level.  Starts at 1 so a
   zeroed entry never matches. */
static uint64_t texture_level_generation = 1;

static struct mipmap_repair_entry *mipmap_repair_entry_for(
    CGLShareGroupObj share_group, GLenum target, GLuint texture, bool create)
{
    size_t start = ((size_t)texture * 2654435761u +
                    (size_t)target * 40503u +
                    ((uintptr_t)share_group >> 4)) % kMipmapRepairCapacity;
    for (size_t probe = 0; probe < kMipmapRepairCapacity; ++probe) {
        struct mipmap_repair_entry *entry =
            &mipmap_repairs[(start + probe) % kMipmapRepairCapacity];
        if (!entry->occupied) {
            if (!create) return NULL;
            entry->share_group = share_group;
            entry->target = target;
            entry->texture = texture;
            entry->requested_maximum = 0;
            entry->compatibility_maximum = 0;
            entry->occupied = true;
            entry->clamped = false;
            return entry;
        }
        if (entry->share_group == share_group && entry->target == target &&
            entry->texture == texture) {
            return entry;
        }
    }
    return NULL;
}

static void forget_mipmap_repairs(GLsizei count, const GLuint *textures)
{
    if (count <= 0 || !textures) return;
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    pthread_mutex_lock(&mipmap_repair_mutex);
    for (GLsizei index = 0; index < count; ++index) {
        for (size_t slot = 0; slot < kMipmapRepairCapacity; ++slot) {
            struct mipmap_repair_entry *entry = &mipmap_repairs[slot];
            if (entry->occupied && entry->share_group == share_group &&
                entry->texture == textures[index]) {
                /* Keep the open-addressing slot occupied so later colliding
                 * entries remain discoverable; a reused GL name reuses it. */
                entry->requested_maximum = 0;
                entry->compatibility_maximum = 0;
                entry->clamped = false;
                entry->validated_generation = 0;
            }
        }
    }
    pthread_mutex_unlock(&mipmap_repair_mutex);
}

/*
 * Legacy NVIDIA/ATI drivers sampled the mip levels that existed even when a
 * texture's declared GL_TEXTURE_MAX_LEVEL extended one level past its loaded
 * image chain.  Apple's current Metal-backed OpenGL correctly treats that
 * texture as incomplete and returns the fallback sample.  In this game that
 * turns alpha-cutout rain, foliage, water and sky cards into solid neon
 * geometry.  Keep the engine's requested range, but temporarily clamp it to
 * the largest contiguous, correctly-sized chain.  As streaming uploads add
 * levels, this function expands the range again and ultimately restores the
 * requested maximum.
 */
static GLenum canonical_mipmap_target(GLenum target)
{
    if (target == GL_TEXTURE_2D) return GL_TEXTURE_2D;
    if (target == GL_TEXTURE_CUBE_MAP ||
        (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
         target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)) {
        return GL_TEXTURE_CUBE_MAP;
    }
    return 0;
}

static void repair_bound_texture_mipmap_range(GLenum upload_target)
{
    GLenum target = canonical_mipmap_target(upload_target);
    static int repair_disabled = -1;
    if (repair_disabled < 0) {
        repair_disabled = getenv("LP32_DISABLE_MIPMAP_REPAIR") != NULL;
    }
    if (!target || repair_disabled) return;

    /* Read before the scan: a level added by another context mid-scan then
       leaves the entry stale rather than wrongly current. */
    uint64_t level_generation =
        __atomic_load_n(&texture_level_generation, __ATOMIC_ACQUIRE);
    GLint texture = 0;
    GLint min_filter = 0;
    GLint base_level = 0;
    GLint current_maximum = 0;
    glGetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D :
                  GL_TEXTURE_BINDING_CUBE_MAP, &texture);
    if (texture <= 0) return;
    glGetTexParameteriv(target, GL_TEXTURE_MIN_FILTER, &min_filter);
    if (!texture_filter_needs_mipmaps(min_filter)) return;
    glGetTexParameteriv(target, GL_TEXTURE_BASE_LEVEL, &base_level);
    glGetTexParameteriv(target, GL_TEXTURE_MAX_LEVEL, &current_maximum);

    GLenum image_targets[6] = {GL_TEXTURE_2D};
    size_t image_target_count = 1;
    if (target == GL_TEXTURE_CUBE_MAP) {
        image_target_count = 6;
        for (size_t face = 0; face < image_target_count; ++face) {
            image_targets[face] = GL_TEXTURE_CUBE_MAP_POSITIVE_X +
                                  (GLenum)face;
        }
    }

    GLint base_width = 0;
    GLint base_height = 0;
    GLint base_format = 0;
    glGetTexLevelParameteriv(image_targets[0], base_level, GL_TEXTURE_WIDTH,
                             &base_width);
    glGetTexLevelParameteriv(image_targets[0], base_level, GL_TEXTURE_HEIGHT,
                             &base_height);
    glGetTexLevelParameteriv(image_targets[0], base_level,
                             GL_TEXTURE_INTERNAL_FORMAT, &base_format);
    if (base_width <= 0 || base_height <= 0 || !base_format) return;

    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    pthread_mutex_lock(&mipmap_repair_mutex);
    struct mipmap_repair_entry *entry = mipmap_repair_entry_for(
        share_group, target, (GLuint)texture, true);
    if (!entry) {
        pthread_mutex_unlock(&mipmap_repair_mutex);
        return;
    }
    GLint requested_maximum = current_maximum;
    if (entry->clamped &&
        current_maximum == entry->compatibility_maximum) {
        requested_maximum = entry->requested_maximum;
    }
    pthread_mutex_unlock(&mipmap_repair_mutex);

    GLint natural_last = base_level;
    GLint largest_dimension =
        base_width > base_height ? base_width : base_height;
    while (largest_dimension > 1 && natural_last < INT_MAX) {
        largest_dimension >>= 1;
        ++natural_last;
    }
    GLint required_last = requested_maximum < natural_last ?
        requested_maximum : natural_last;
    if (required_last < base_level) return;

    GLint expected_width = base_width;
    GLint expected_height = base_height;
    GLint last_contiguous = base_level - 1;
    for (GLint level = base_level; level <= required_last; ++level) {
        bool complete_level = true;
        for (size_t face = 0; face < image_target_count; ++face) {
            GLint width = 0;
            GLint height = 0;
            GLint format = 0;
            glGetTexLevelParameteriv(image_targets[face], level,
                                     GL_TEXTURE_WIDTH, &width);
            glGetTexLevelParameteriv(image_targets[face], level,
                                     GL_TEXTURE_HEIGHT, &height);
            glGetTexLevelParameteriv(image_targets[face], level,
                                     GL_TEXTURE_INTERNAL_FORMAT, &format);
            if (width != expected_width || height != expected_height ||
                format != base_format) {
                complete_level = false;
                break;
            }
        }
        if (!complete_level) break;
        last_contiguous = level;
        if (expected_width > 1) expected_width >>= 1;
        if (expected_height > 1) expected_height >>= 1;
    }
    if (last_contiguous < base_level) return;

    GLint compatibility_maximum = last_contiguous < required_last ?
        last_contiguous : requested_maximum;
    if (compatibility_maximum != current_maximum) {
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, compatibility_maximum);
    }

    pthread_mutex_lock(&mipmap_repair_mutex);
    entry = mipmap_repair_entry_for(share_group, target, (GLuint)texture,
                                    true);
    if (entry) {
        entry->requested_maximum = requested_maximum;
        entry->compatibility_maximum = compatibility_maximum;
        entry->clamped = compatibility_maximum != requested_maximum;
        entry->validated_generation = level_generation;
        entry->validated_min_filter = min_filter;
        entry->validated_base_level = base_level;
    }
    bool report = compatibility_maximum != requested_maximum &&
        mipmap_repair_reports < 32;
    if (report) ++mipmap_repair_reports;
    pthread_mutex_unlock(&mipmap_repair_mutex);
    if (report) {
        gl_trace_printf(
            "compat32: repaired incomplete mip chain target=%04x tex=%d base=%d "
            "requestedMax=%d availableMax=%d size=%dx%d format=%04x\n",
            target, texture, base_level, requested_maximum,
            compatibility_maximum, base_width, base_height, base_format);
    }
}

static void note_texture_level_change(void)
{
    __atomic_fetch_add(&texture_level_generation, 1, __ATOMIC_RELEASE);
}

/*
 * glTexParameteri(MIN_FILTER / BASE_LEVEL / MAX_LEVEL) runs about 1,500 times a
 * frame, mostly re-stating the value a texture already has.  When the last
 * full scan of this texture saw the same value and no level has been created
 * anywhere since, the scan would reach the same answer, so skip it.  A
 * MAX_LEVEL write is only skippable while the texture is unclamped: on a
 * clamped one the game's write just undid the clamp and the scan must redo it.
 */
static void repair_mipmap_range_for_parameter(GLenum upload_target,
                                              GLenum parameter, GLint value)
{
    GLenum target = canonical_mipmap_target(upload_target);
    if (!target) return;
    GLint texture = 0;
    glGetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D :
                  GL_TEXTURE_BINDING_CUBE_MAP, &texture);
    if (texture <= 0) return;
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    uint64_t level_generation =
        __atomic_load_n(&texture_level_generation, __ATOMIC_ACQUIRE);

    pthread_mutex_lock(&mipmap_repair_mutex);
    const struct mipmap_repair_entry *entry = mipmap_repair_entry_for(
        share_group, target, (GLuint)texture, false);
    bool unchanged = false;
    if (entry && entry->validated_generation == level_generation) {
        if (parameter == GL_TEXTURE_MIN_FILTER) {
            unchanged = value == entry->validated_min_filter;
        } else if (parameter == GL_TEXTURE_BASE_LEVEL) {
            unchanged = value == entry->validated_base_level;
        } else if (parameter == GL_TEXTURE_MAX_LEVEL) {
            unchanged = !entry->clamped && value == entry->requested_maximum;
        }
    }
    pthread_mutex_unlock(&mipmap_repair_mutex);
    if (unchanged) return;
    repair_bound_texture_mipmap_range(upload_target);
}

static void trace_texture_units(void)
{
    GLint previous_active_texture = GL_TEXTURE0;
    GLint maximum_units = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maximum_units);
    if (maximum_units < 0) maximum_units = 0;
    if (maximum_units > 16) maximum_units = 16;

    for (GLint unit = 0; unit < maximum_units; ++unit) {
        glActiveTexture((GLenum)(GL_TEXTURE0 + unit));
        GLint texture_2d = 0;
        GLint texture_3d = 0;
        GLint texture_cube = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_2d);
        glGetIntegerv(GL_TEXTURE_BINDING_3D, &texture_3d);
        glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &texture_cube);
        if (!texture_2d && !texture_3d && !texture_cube) continue;

        GLint width_2d = 0;
        GLint height_2d = 0;
        GLint format_2d = 0;
        GLint min_filter_2d = 0;
        if (texture_2d) {
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
                                     &width_2d);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,
                                     &height_2d);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0,
                                     GL_TEXTURE_INTERNAL_FORMAT, &format_2d);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                &min_filter_2d);
            trace_texture_2d_content(unit, (GLuint)texture_2d,
                                     min_filter_2d);
        }
        GLint width_3d = 0;
        GLint height_3d = 0;
        GLint depth_3d = 0;
        GLint format_3d = 0;
        if (texture_3d) {
            glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_WIDTH,
                                     &width_3d);
            glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_HEIGHT,
                                     &height_3d);
            glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_DEPTH,
                                     &depth_3d);
            glGetTexLevelParameteriv(GL_TEXTURE_3D, 0,
                                     GL_TEXTURE_INTERNAL_FORMAT, &format_3d);
        }
        GLint width_cube = 0;
        GLint height_cube = 0;
        GLint format_cube = 0;
        GLint min_filter_cube = 0;
        if (texture_cube) {
            glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0,
                                     GL_TEXTURE_WIDTH, &width_cube);
            glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0,
                                     GL_TEXTURE_HEIGHT, &height_cube);
            glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0,
                                     GL_TEXTURE_INTERNAL_FORMAT,
                                     &format_cube);
            glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                                &min_filter_cube);
            trace_texture_cube_content(unit, (GLuint)texture_cube,
                                       min_filter_cube);
        }
        gl_trace_printf(
            "compat32: texture-state swap=%llu unit=%d "
            "tex2d=%d size2d=%dx%d format2d=%04x min2d=%04x enabled2d=%d "
            "tex3d=%d size3d=%dx%dx%d format3d=%04x enabled3d=%d "
            "cube=%d sizeCube=%dx%d formatCube=%04x enabledCube=%d "
            "minCube=%04x\n",
            (unsigned long long)objc_bridge_swap_count, unit,
            texture_2d, width_2d, height_2d, format_2d, min_filter_2d,
            glIsEnabled(GL_TEXTURE_2D), texture_3d, width_3d, height_3d,
            depth_3d, format_3d, glIsEnabled(GL_TEXTURE_3D), texture_cube,
            width_cube, height_cube, format_cube,
            glIsEnabled(GL_TEXTURE_CUBE_MAP), min_filter_cube);
    }
    glActiveTexture((GLenum)previous_active_texture);
}

static void trace_draw_call(const char *kind, GLenum mode, GLint first,
                            GLuint advertised_start, GLuint advertised_end,
                            GLsizei count, GLenum index_type,
                            uintptr_t index_pointer, bool indexed)
{
    ++gl_frame_diagnostics.draw_calls;
    if (!trace_gl_render_call()) return;
    GLint framebuffer = 0;
    GLint array_buffer = 0;
    GLint element_buffer = 0;
    GLint framebuffer_status = GL_FRAMEBUFFER_COMPLETE;
    GLint blend_source = 0;
    GLint blend_destination = 0;
    GLint blend_equation = 0;
    GLint alpha_function = 0;
    GLfloat alpha_reference = 0.0f;
    GLint depth_function = 0;
    GLint cull_mode = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer);
    glGetIntegerv(GL_BLEND_SRC, &blend_source);
    glGetIntegerv(GL_BLEND_DST, &blend_destination);
    glGetIntegerv(GL_BLEND_EQUATION, &blend_equation);
    glGetIntegerv(GL_ALPHA_TEST_FUNC, &alpha_function);
    glGetFloatv(GL_ALPHA_TEST_REF, &alpha_reference);
    glGetIntegerv(GL_DEPTH_FUNC, &depth_function);
    glGetIntegerv(GL_CULL_FACE_MODE, &cull_mode);
    if (framebuffer) {
        framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    }

    GLint maximum_draw_buffers = 0;
    GLint draw_buffers[4] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE};
    GLint attachment_types[4] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE};
    GLint attachment_names[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_MAX_DRAW_BUFFERS_ARB, &maximum_draw_buffers);
    if (maximum_draw_buffers < 0) maximum_draw_buffers = 0;
    if (maximum_draw_buffers > 4) maximum_draw_buffers = 4;
    for (GLint index = 0; index < maximum_draw_buffers; ++index) {
        glGetIntegerv(GL_DRAW_BUFFER0_ARB + index, &draw_buffers[index]);
        if (!framebuffer ||
            draw_buffers[index] < GL_COLOR_ATTACHMENT0 ||
            draw_buffers[index] > GL_COLOR_ATTACHMENT15) {
            continue;
        }
        glGetFramebufferAttachmentParameteriv(
            GL_FRAMEBUFFER, (GLenum)draw_buffers[index],
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &attachment_types[index]);
        glGetFramebufferAttachmentParameteriv(
            GL_FRAMEBUFFER, (GLenum)draw_buffers[index],
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &attachment_names[index]);
    }

    GLuint minimum = first >= 0 ? (GLuint)first : 0;
    GLuint maximum = count > 0 && first >= 0 &&
        (GLuint)first <= UINT_MAX - (GLuint)(count - 1) ?
        (GLuint)first + (GLuint)(count - 1) : minimum;
    gl_trace_printf(
        "compat32: %s swap=%llu mode=%04x first=%d count=%d fbo=%d "
        "fboStatus=%04x vbo=%d ibo=%d vp=%u fp=%u vpEnabled=%d "
        "fpEnabled=%d blend=%d blendFunc={%04x,%04x} blendEq=%04x "
        "alphaTest=%d alphaFunc=%04x alphaRef=%.7g depthTest=%d "
        "depthFunc=%04x cull=%d cullMode=%04x\n",
        kind, (unsigned long long)objc_bridge_swap_count, mode, first, count,
        framebuffer, framebuffer_status, array_buffer, element_buffer,
        trace_vertex_program, trace_fragment_program,
        glIsEnabled(GL_VERTEX_PROGRAM_ARB),
        glIsEnabled(GL_FRAGMENT_PROGRAM_ARB), glIsEnabled(GL_BLEND),
        blend_source, blend_destination, blend_equation,
        glIsEnabled(GL_ALPHA_TEST), alpha_function, alpha_reference,
        glIsEnabled(GL_DEPTH_TEST), depth_function,
        glIsEnabled(GL_CULL_FACE), cull_mode);
    gl_trace_printf(
        "compat32: framebuffer-state swap=%llu fbo=%d "
        "drawBuffers={%04x,%04x,%04x,%04x} "
        "attachmentTypes={%04x,%04x,%04x,%04x} "
        "attachmentNames={%d,%d,%d,%d}\n",
        (unsigned long long)objc_bridge_swap_count, framebuffer,
        draw_buffers[0], draw_buffers[1], draw_buffers[2], draw_buffers[3],
        attachment_types[0], attachment_types[1], attachment_types[2],
        attachment_types[3], attachment_names[0], attachment_names[1],
        attachment_names[2], attachment_names[3]);

    if (indexed) {
        struct gl_index_diagnostic diagnostic = trace_index_buffer(
            count, index_type, index_pointer, advertised_start,
            advertised_end, element_buffer);
        minimum = diagnostic.minimum;
        maximum = diagnostic.maximum;
    }
    trace_vertex_attributes(minimum, maximum);
    trace_texture_units();
}

/*
 * Diagnostic pixel probe: LP32_TRACE_PIXEL=fx,fy (fractions of the viewport,
 * origin bottom-left) reads that pixel back after every draw of an armed
 * render trace and logs each change together with the programs that made it,
 * so the draw that paints a given screen region can be identified.
 */
static void probe_trace_pixel(const char *kind)
{
    static int state = -1;
    static float fraction_x, fraction_y;
    static GLubyte last[4];
    if (state < 0) {
        const char *value = getenv("LP32_TRACE_PIXEL");
        state = 0;
        if (value && sscanf(value, "%f,%f", &fraction_x, &fraction_y) == 2) {
            state = 1;
        }
    }
    if (state != 1 || !gl_render_trace_remaining) return;
    GLint viewport[4] = {0, 0, 0, 0};
    GLint framebuffer = 0;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    if (viewport[2] < 256 || viewport[3] < 256) return;
    /* Negative fractions select a grid over the whole viewport instead. */
    if (fraction_x < 0.0f) {
        enum { GRID = 12 };
        static GLubyte grid_last[GRID * GRID][4];
        for (int index = 0; index < GRID * GRID; ++index) {
            GLint x = viewport[0] + (GLint)((index % GRID + 0.5f) *
                                            (float)viewport[2] / GRID);
            GLint y = viewport[1] + (GLint)((index / GRID + 0.5f) *
                                            (float)viewport[3] / GRID);
            GLubyte pixel[4] = {0, 0, 0, 0};
            glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            if (memcmp(pixel, grid_last[index], sizeof(pixel)) == 0) continue;
            memcpy(grid_last[index], pixel, sizeof(pixel));
            gl_trace_printf(
                "compat32: pixel-probe swap=%llu after=%s fbo=%d point=%d "
                "at=(%d,%d) rgba={%u,%u,%u,%u} vp=%u fp=%u\n",
                (unsigned long long)objc_bridge_swap_count, kind, framebuffer,
                index, x, y, pixel[0], pixel[1], pixel[2], pixel[3],
                trace_vertex_program, trace_fragment_program);
        }
        return;
    }
    GLint x = viewport[0] + (GLint)(fraction_x * (float)viewport[2]);
    GLint y = viewport[1] + (GLint)(fraction_y * (float)viewport[3]);
    GLubyte pixel[4] = {0, 0, 0, 0};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (memcmp(pixel, last, sizeof(pixel)) == 0) return;
    memcpy(last, pixel, sizeof(pixel));
    gl_trace_printf(
        "compat32: pixel-probe swap=%llu after=%s fbo=%d viewport={%d,%d,%d,%d} "
        "at=(%d,%d) rgba={%u,%u,%u,%u} vp=%u fp=%u\n",
        (unsigned long long)objc_bridge_swap_count, kind, framebuffer,
        viewport[0], viewport[1], viewport[2], viewport[3], x, y,
        pixel[0], pixel[1], pixel[2], pixel[3], trace_vertex_program,
        trace_fragment_program);
}

static uint32_t guest_gl_string(GLenum name)
{
    const GLubyte *raw_value = glGetString(name);
    if (!raw_value) return 0;
    const char *value = (const char *)raw_value;
    const char *buffer_mode = getenv("LP32_GL_BUFFER_MODE");
    bool prefer_subdata = getenv("LP32_PREFER_BUFFER_SUBDATA") != NULL ||
        (buffer_mode && strcmp(buffer_mode, "subdata") == 0);
    bool prefer_map_range = buffer_mode && strcmp(buffer_mode, "maprange") == 0;
    if (name != GL_EXTENSIONS || (!prefer_subdata && !prefer_map_range)) {
        return compat_runtime32_copy_cstring(value);
    }

    static const char apple_extension[] = "GL_APPLE_flush_buffer_range";
    static const char range_extension[] = "GL_ARB_map_buffer_range";
    size_t length = strlen(value);
    char *filtered = malloc(length + 1);
    if (!filtered) return 0;
    size_t output = 0;
    const char *cursor = value;
    while (*cursor) {
        while (*cursor == ' ') ++cursor;
        const char *end = cursor;
        while (*end && *end != ' ') ++end;
        size_t token_length = (size_t)(end - cursor);
        bool omit_apple = token_length == sizeof(apple_extension) - 1 &&
            memcmp(cursor, apple_extension, token_length) == 0;
        bool omit_range = prefer_subdata &&
            token_length == sizeof(range_extension) - 1 &&
            memcmp(cursor, range_extension, token_length) == 0;
        if (token_length && !omit_apple && !omit_range) {
            if (output) filtered[output++] = ' ';
            memcpy(filtered + output, cursor, token_length);
            output += token_length;
        }
        cursor = end;
    }
    filtered[output] = '\0';
    uint32_t guest_value = compat_runtime32_copy_cstring(filtered);
    free(filtered);
    static bool logged;
    if (!logged) {
        fprintf(stderr,
                "compat32: GL buffer backend override=%s; hiding %s%s\n",
                prefer_subdata ? "subdata" : "maprange",
                apple_extension,
                prefer_subdata ? " and GL_ARB_map_buffer_range" : "");
        logged = true;
    }
    return guest_value;
}

static int objc_bridge32_dispatch_body(const char *import_name,
                                       size_t import_length,
                                       const uint32_t *arguments,
                                       uint64_t *result);

/*
 * Direct handlers for the GL entry points that make up most of the 10-30
 * thousand imports per frame.  The runtime memoizes them per import id after
 * the first chained dispatch, so later calls skip the name chains in
 * dispatch_named_import and objc_bridge32_dispatch_body entirely.  The chain
 * cases below call these same functions; keep them the only implementation.
 */
#define FAST_GL(name) \
    static uint64_t fast_##name(const uint32_t *arguments, \
                                uint32_t return_address)

FAST_GL(glVertexAttribPointer)
{
    (void)return_address;
    if (trace_gl_render_call()) {
        GLint array_buffer = 0;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
        gl_trace_printf(
            "compat32: glVertexAttribPointer swap=%llu index=%u "
            "size=%d type=%04x normalized=%u stride=%d pointer=%08x "
            "buffer=%d\n",
            (unsigned long long)objc_bridge_swap_count, arguments[0],
            (GLint)arguments[1], arguments[2], arguments[3],
            (GLsizei)arguments[4], arguments[5], array_buffer);
    }
    glVertexAttribPointer(arguments[0], (GLint)arguments[1], arguments[2],
                          (GLboolean)arguments[3], (GLsizei)arguments[4],
                          (const void *)(uintptr_t)arguments[5]);
    return 0;
}

FAST_GL(glEnableVertexAttribArray)
{
    (void)return_address;
    if (trace_gl_render_call()) {
        gl_trace_printf(
            "compat32: glEnableVertexAttribArray swap=%llu index=%u\n",
            (unsigned long long)objc_bridge_swap_count, arguments[0]);
    }
    glEnableVertexAttribArray(arguments[0]);
    return 0;
}

FAST_GL(glDisableVertexAttribArray)
{
    (void)return_address;
    if (trace_gl_render_call()) {
        gl_trace_printf(
            "compat32: glDisableVertexAttribArray swap=%llu index=%u\n",
            (unsigned long long)objc_bridge_swap_count, arguments[0]);
    }
    glDisableVertexAttribArray(arguments[0]);
    return 0;
}

FAST_GL(glDrawRangeElements)
{
    (void)return_address;
    struct sampler_fallback_restore sampler_restore;
    repair_unbound_fragment_samplers(&sampler_restore);
    trace_draw_call("glDrawRangeElements", arguments[0], 0,
                    arguments[1], arguments[2], (GLsizei)arguments[3],
                    arguments[4], (uintptr_t)arguments[5], true);
    glDrawRangeElements(arguments[0], arguments[1], arguments[2],
                        (GLsizei)arguments[3], arguments[4],
                        (const void *)(uintptr_t)arguments[5]);
    probe_trace_pixel("glDrawRangeElements");
    restore_unbound_fragment_samplers(&sampler_restore);
    return 0;
}

FAST_GL(glDrawArrays)
{
    (void)return_address;
    struct sampler_fallback_restore sampler_restore;
    repair_unbound_fragment_samplers(&sampler_restore);
    trace_draw_call("glDrawArrays", arguments[0], (GLint)arguments[1],
                    0, 0, (GLsizei)arguments[2], 0, 0, false);
    glDrawArrays(arguments[0], arguments[1], arguments[2]);
    probe_trace_pixel("glDrawArrays");
    restore_unbound_fragment_samplers(&sampler_restore);
    return 0;
}

FAST_GL(glProgramEnvParameters4fvEXT)
{
    (void)return_address;
    const GLfloat *values = (const void *)(uintptr_t)arguments[3];
    GLsizei count = (GLsizei)arguments[2];
    trace_program_parameter_upload(
        "glProgramEnvParameters4fvEXT", arguments[0], arguments[1],
        count, values);
    size_t replaced = 0;
    const GLfloat *upload =
        safe_program_parameter_values(values, count, &replaced);
    report_sanitized_program_parameters(
        "environment-batch", arguments[0], arguments[1], count, replaced);
    if (split_program_parameter_batches()) {
        for (GLsizei index = 0; index < count; ++index) {
            glProgramEnvParameter4fvARB(
                arguments[0], arguments[1] + index, upload + index * 4);
        }
    } else {
        glProgramEnvParameters4fvEXT(arguments[0], arguments[1], count,
                                     upload);
    }
    return 0;
}

FAST_GL(glProgramLocalParameters4fvEXT)
{
    (void)return_address;
    const GLfloat *values = (const void *)(uintptr_t)arguments[3];
    GLsizei count = (GLsizei)arguments[2];
    trace_program_parameter_upload(
        "glProgramLocalParameters4fvEXT", arguments[0], arguments[1],
        count, values);
    size_t replaced = 0;
    const GLfloat *upload =
        safe_program_parameter_values(values, count, &replaced);
    report_sanitized_program_parameters(
        "local-batch", arguments[0], arguments[1], count, replaced);
    if (split_program_parameter_batches()) {
        for (GLsizei index = 0; index < count; ++index) {
            glProgramLocalParameter4fvARB(
                arguments[0], arguments[1] + index, upload + index * 4);
        }
    } else {
        glProgramLocalParameters4fvEXT(arguments[0], arguments[1], count,
                                       upload);
    }
    return 0;
}

FAST_GL(glProgramEnvParameter4fvARB)
{
    (void)return_address;
    const GLfloat *values = (const void *)(uintptr_t)arguments[2];
    trace_program_parameter_upload(
        "glProgramEnvParameter4fvARB", arguments[0], arguments[1], 1, values);
    size_t replaced = 0;
    const GLfloat *upload = safe_program_parameter_values(values, 1, &replaced);
    report_sanitized_program_parameters(
        "environment-parameter", arguments[0], arguments[1], 1, replaced);
    glProgramEnvParameter4fvARB(arguments[0], arguments[1], upload);
    return 0;
}

FAST_GL(glProgramLocalParameter4fvARB)
{
    (void)return_address;
    const GLfloat *values = (const void *)(uintptr_t)arguments[2];
    trace_program_parameter_upload(
        "glProgramLocalParameter4fvARB", arguments[0], arguments[1], 1,
        values);
    size_t replaced = 0;
    const GLfloat *upload = safe_program_parameter_values(values, 1, &replaced);
    report_sanitized_program_parameters(
        "local-parameter", arguments[0], arguments[1], 1, replaced);
    glProgramLocalParameter4fvARB(arguments[0], arguments[1], upload);
    return 0;
}

FAST_GL(glBindProgramARB)
{
    (void)return_address;
    ++gl_frame_diagnostics.program_binds;
    if (trace_gl_render_call()) {
        fprintf(stderr,
                "compat32: glBindProgramARB swap=%llu target=%04x "
                "program=%u\n",
                (unsigned long long)objc_bridge_swap_count,
                arguments[0], arguments[1]);
    }
    glBindProgramARB(arguments[0], arguments[1]);
    if (arguments[0] == GL_VERTEX_PROGRAM_ARB) {
        trace_vertex_program = arguments[1];
    } else if (arguments[0] == GL_FRAGMENT_PROGRAM_ARB) {
        trace_fragment_program = arguments[1];
    }
    return 0;
}

FAST_GL(glBindBuffer)
{
    (void)return_address;
    if (trace_gl_render_call()) {
        gl_trace_printf(
            "compat32: glBindBuffer swap=%llu target=%04x buffer=%u\n",
            (unsigned long long)objc_bridge_swap_count,
            arguments[0], arguments[1]);
    }
    glBindBuffer(arguments[0], arguments[1]);
    return 0;
}

FAST_GL(glGetUniformLocation)
{
    (void)return_address;
    return (uint32_t)glGetUniformLocation(arguments[0],
                                          (const GLchar *)(uintptr_t)arguments[1]);
}

FAST_GL(glActiveTexture)
{
    (void)return_address;
    if (trace_gl_render_call()) {
        gl_trace_printf(
            "compat32: glActiveTexture swap=%llu unit=%04x\n",
            (unsigned long long)objc_bridge_swap_count, arguments[0]);
    }
    glActiveTexture(arguments[0]);
    return 0;
}

FAST_GL(glClientActiveTexture)
{
    (void)return_address;
    glClientActiveTexture(arguments[0]);
    return 0;
}

FAST_GL(glBlendEquation)
{
    (void)return_address;
    glBlendEquation(arguments[0]);
    return 0;
}

FAST_GL(glBlendFuncSeparate)
{
    (void)return_address;
    glBlendFuncSeparate(arguments[0], arguments[1], arguments[2],
                        arguments[3]);
    return 0;
}

FAST_GL(glEnable)
{
    (void)return_address;
    if (trace_gl_render_call()) {
        fprintf(stderr, "compat32: glEnable swap=%llu cap=%04x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0]);
    }
    glEnable(arguments[0]);
    return 0;
}

FAST_GL(glDisable)
{
    (void)return_address;
    if (trace_gl_render_call()) {
        fprintf(stderr, "compat32: glDisable swap=%llu cap=%04x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0]);
    }
    glDisable(arguments[0]);
    return 0;
}

FAST_GL(glBindTexture)
{
    (void)return_address;
    ++gl_frame_diagnostics.texture_binds;
    if (trace_gl_render_call()) {
        gl_trace_printf(
            "compat32: glBindTexture swap=%llu target=%04x texture=%u\n",
            (unsigned long long)objc_bridge_swap_count,
            arguments[0], arguments[1]);
    }
    glBindTexture(arguments[0], arguments[1]);
    return 0;
}

FAST_GL(glTexParameteri)
{
    (void)return_address;
    glTexParameteri(arguments[0], arguments[1], arguments[2]);
    if (arguments[1] == GL_TEXTURE_MIN_FILTER ||
        arguments[1] == GL_TEXTURE_BASE_LEVEL ||
        arguments[1] == GL_TEXTURE_MAX_LEVEL) {
        repair_mipmap_range_for_parameter(arguments[0], arguments[1],
                                          (GLint)arguments[2]);
    }
    return 0;
}

FAST_GL(glTexEnvf)
{
    (void)return_address;
    float value;
    memcpy(&value, arguments + 2, sizeof(value));
    glTexEnvf(arguments[0], arguments[1], value);
    return 0;
}

FAST_GL(glAlphaFunc)
{
    (void)return_address;
    float value;
    memcpy(&value, arguments + 1, sizeof(value));
    glAlphaFunc(arguments[0], value);
    return 0;
}

FAST_GL(glPolygonOffset)
{
    (void)return_address;
    const float *v = (const void *)arguments;
    glPolygonOffset(v[0], v[1]);
    return 0;
}

FAST_GL(glBlendFunc)  { (void)return_address; glBlendFunc(arguments[0], arguments[1]); return 0; }
FAST_GL(glColorMask)  { (void)return_address; glColorMask(arguments[0], arguments[1], arguments[2], arguments[3]); return 0; }
FAST_GL(glCullFace)   { (void)return_address; glCullFace(arguments[0]); return 0; }
FAST_GL(glDepthFunc)  { (void)return_address; glDepthFunc(arguments[0]); return 0; }
FAST_GL(glDepthMask)  { (void)return_address; glDepthMask(arguments[0]); return 0; }
FAST_GL(glFrontFace)  { (void)return_address; glFrontFace(arguments[0]); return 0; }
FAST_GL(glScissor)    { (void)return_address; glScissor(arguments[0], arguments[1], arguments[2], arguments[3]); return 0; }
FAST_GL(glStencilFunc){ (void)return_address; glStencilFunc(arguments[0], arguments[1], arguments[2]); return 0; }
FAST_GL(glStencilMask){ (void)return_address; glStencilMask(arguments[0]); return 0; }
FAST_GL(glStencilOp)  { (void)return_address; glStencilOp(arguments[0], arguments[1], arguments[2]); return 0; }
FAST_GL(glGetIntegerv){ (void)return_address; glGetIntegerv(arguments[0], (GLint *)(uintptr_t)arguments[1]); return 0; }
FAST_GL(glGetFloatv)  { (void)return_address; glGetFloatv(arguments[0], (GLfloat *)(uintptr_t)arguments[1]); return 0; }
/* Hot per-draw entry points on Bejeweled 3's board (client-array sprites).
   Kept on the fast path so the thousands of calls per frame skip the name
   chains -- these were choppy going through the slow dispatcher. */
FAST_GL(glDrawElements) { (void)return_address; glDrawElements(arguments[0], (GLsizei)arguments[1], arguments[2], (const void *)(uintptr_t)arguments[3]); return 0; }
FAST_GL(glVertexPointer) { (void)return_address; glVertexPointer((GLint)arguments[0], arguments[1], (GLsizei)arguments[2], (const void *)(uintptr_t)arguments[3]); return 0; }
FAST_GL(glColorPointer) { (void)return_address; glColorPointer((GLint)arguments[0], arguments[1], (GLsizei)arguments[2], (const void *)(uintptr_t)arguments[3]); return 0; }
FAST_GL(glTexCoordPointer) { (void)return_address; glTexCoordPointer((GLint)arguments[0], arguments[1], (GLsizei)arguments[2], (const void *)(uintptr_t)arguments[3]); return 0; }
FAST_GL(glNormalPointer) { (void)return_address; glNormalPointer(arguments[0], (GLsizei)arguments[1], (const void *)(uintptr_t)arguments[2]); return 0; }
FAST_GL(glEnableClientState) { (void)return_address; glEnableClientState(arguments[0]); return 0; }
/* The immediate-mode entry points, which Bejeweled 2 and Zuma draw everything
   with: one glBegin, four each of colour/texcoord/vertex, one glEnd per quad.
   A board's worth of sprites is thousands of calls a frame and a full-screen
   effect -- Bejeweled 2's between-level warp -- is tens of thousands, every
   one of which otherwise walks the whole name chain down to
   peggle_graphics_dispatch before doing a few nanoseconds of work.  That cost
   is invisible to LP32_SLOW_IMPORT_MS because no single call is slow; it only
   shows up as the frame rate collapsing when the geometry count jumps. */
FAST_GL(glBegin)      { (void)return_address; glBegin(arguments[0]); return 0; }
FAST_GL(glEnd)        { (void)return_address; (void)arguments; glEnd(); return 0; }
FAST_GL(glVertex2i)   { (void)return_address; glVertex2i((GLint)arguments[0], (GLint)arguments[1]); return 0; }
FAST_GL(glVertex2f)   { (void)return_address; glVertex2f(guest_float_argument(arguments[0]), guest_float_argument(arguments[1])); return 0; }
FAST_GL(glTexCoord2f) { (void)return_address; glTexCoord2f(guest_float_argument(arguments[0]), guest_float_argument(arguments[1])); return 0; }
FAST_GL(glColor4ub)   { (void)return_address; glColor4ub((GLubyte)arguments[0], (GLubyte)arguments[1], (GLubyte)arguments[2], (GLubyte)arguments[3]); return 0; }
FAST_GL(glColor4ubv)  { (void)return_address; glColor4ubv((const GLubyte *)(uintptr_t)arguments[0]); return 0; }
/* Bejeweled 2 checks glGetError once per quad, so it is as hot as the vertex
   calls; the driver round trip is the game's own cost but the dispatch around
   it need not be. */
FAST_GL(glGetError)
{
    (void)return_address;
    (void)arguments;
    GLenum error = glGetError();
    if (error != GL_NO_ERROR && trace_gl_render_call()) {
        fprintf(stderr, "compat32: glGetError swap=%llu error=%04x\n",
                (unsigned long long)objc_bridge_swap_count, error);
    }
    return error;
}
FAST_GL(glDisableClientState) { (void)return_address; glDisableClientState(arguments[0]); return 0; }
FAST_GL(glLoadMatrixf) { (void)return_address; glLoadMatrixf((const GLfloat *)(uintptr_t)arguments[0]); return 0; }
/* Shader uniforms are updated every frame by Bejeweled 3's flame/ripple GLSL
   effects; keep them on the fast path too. */
FAST_GL(glUseProgram) { (void)return_address; glUseProgram(arguments[0]); return 0; }
FAST_GL(glUniform1i) { (void)return_address; glUniform1i((GLint)arguments[0], (GLint)arguments[1]); return 0; }
FAST_GL(glUniform4fv) { (void)return_address; glUniform4fv((GLint)arguments[0], (GLsizei)arguments[1], (const GLfloat *)(uintptr_t)arguments[2]); return 0; }
FAST_GL(glUniformMatrix4fv) { (void)return_address; glUniformMatrix4fv((GLint)arguments[0], (GLsizei)arguments[1], (GLboolean)arguments[2], (const GLfloat *)(uintptr_t)arguments[3]); return 0; }
FAST_GL(glBindFramebufferEXT) { (void)return_address; glBindFramebufferEXT(arguments[0], arguments[1]); return 0; }

#undef FAST_GL

lp32_fast_import_fn objc_bridge32_fast_import(const char *import_name)
{
    static const struct {
        const char *name;
        lp32_fast_import_fn handler;
    } table[] = {
        {"glVertexAttribPointer", fast_glVertexAttribPointer},
        {"glVertexAttribPointerARB", fast_glVertexAttribPointer},
        {"glEnableVertexAttribArray", fast_glEnableVertexAttribArray},
        {"glEnableVertexAttribArrayARB", fast_glEnableVertexAttribArray},
        {"glDisableVertexAttribArray", fast_glDisableVertexAttribArray},
        {"glDisableVertexAttribArrayARB", fast_glDisableVertexAttribArray},
        {"glDrawRangeElements", fast_glDrawRangeElements},
        {"glDrawRangeElementsEXT", fast_glDrawRangeElements},
        {"_glDrawArrays", fast_glDrawArrays},
        {"glProgramEnvParameters4fvEXT", fast_glProgramEnvParameters4fvEXT},
        {"glProgramLocalParameters4fvEXT", fast_glProgramLocalParameters4fvEXT},
        {"glProgramEnvParameter4fvARB", fast_glProgramEnvParameter4fvARB},
        {"glProgramLocalParameter4fvARB", fast_glProgramLocalParameter4fvARB},
        {"glBindProgramARB", fast_glBindProgramARB},
        {"glBindBuffer", fast_glBindBuffer},
        {"glBindBufferARB", fast_glBindBuffer},
        {"glActiveTexture", fast_glActiveTexture},
        {"glActiveTextureARB", fast_glActiveTexture},
        {"glClientActiveTexture", fast_glClientActiveTexture},
        {"glClientActiveTextureARB", fast_glClientActiveTexture},
        /* Bejeweled 3 imports the underscore symbols directly (Peggle
           resolves these by name at run time); each explosion frame issues
           thousands of them. */
        {"_glActiveTexture", fast_glActiveTexture},
        {"_glClientActiveTexture", fast_glClientActiveTexture},
        {"_glGetUniformLocation", fast_glGetUniformLocation},
        {"glBlendEquation", fast_glBlendEquation},
        {"glBlendEquationEXT", fast_glBlendEquation},
        {"glBlendFuncSeparate", fast_glBlendFuncSeparate},
        {"glBlendFuncSeparateEXT", fast_glBlendFuncSeparate},
        {"_glEnable", fast_glEnable},
        {"_glDisable", fast_glDisable},
        {"_glBindTexture", fast_glBindTexture},
        {"_glTexParameteri", fast_glTexParameteri},
        {"_glTexEnvf", fast_glTexEnvf},
        {"_glAlphaFunc", fast_glAlphaFunc},
        {"_glPolygonOffset", fast_glPolygonOffset},
        {"_glBlendFunc", fast_glBlendFunc},
        {"_glColorMask", fast_glColorMask},
        {"_glCullFace", fast_glCullFace},
        {"_glDepthFunc", fast_glDepthFunc},
        {"_glDepthMask", fast_glDepthMask},
        {"_glFrontFace", fast_glFrontFace},
        {"_glScissor", fast_glScissor},
        {"_glStencilFunc", fast_glStencilFunc},
        {"_glStencilMask", fast_glStencilMask},
        {"_glStencilOp", fast_glStencilOp},
        {"_glGetIntegerv", fast_glGetIntegerv},
        {"_glGetFloatv", fast_glGetFloatv},
        {"_glDrawElements", fast_glDrawElements},
        {"_glVertexPointer", fast_glVertexPointer},
        {"_glColorPointer", fast_glColorPointer},
        {"_glTexCoordPointer", fast_glTexCoordPointer},
        {"_glNormalPointer", fast_glNormalPointer},
        {"_glEnableClientState", fast_glEnableClientState},
        {"_glBegin", fast_glBegin},
        {"_glEnd", fast_glEnd},
        {"_glVertex2i", fast_glVertex2i},
        {"_glVertex2f", fast_glVertex2f},
        {"_glTexCoord2f", fast_glTexCoord2f},
        {"_glColor4ub", fast_glColor4ub},
        {"_glColor4ubv", fast_glColor4ubv},
        {"_glGetError", fast_glGetError},
        {"_glDisableClientState", fast_glDisableClientState},
        {"_glLoadMatrixf", fast_glLoadMatrixf},
        {"_glUseProgram", fast_glUseProgram},
        {"_glUniform1i", fast_glUniform1i},
        {"_glUniform4fv", fast_glUniform4fv},
        {"_glUniformMatrix4fv", fast_glUniformMatrix4fv},
        {"_glBindFramebufferEXT", fast_glBindFramebufferEXT},
    };
    for (size_t index = 0; index < sizeof(table) / sizeof(table[0]); ++index) {
        if (strcmp(import_name, table[index].name) == 0) {
            return table[index].handler;
        }
    }
    return NULL;
}

#include "peggle_mac.inc"
#include "objc_guest_class.inc"

int objc_bridge32_dispatch(const char *import_name, const uint32_t *arguments,
                           uint64_t *result)
{
    /*
     * The guest creates and drains its own NSAutoreleasePool objects, but
     * those pools cannot reliably describe the lifetime of temporary host
     * objects created inside a single ABI bridge call.  In particular,
     * invoke_simple_message creates an autoreleased NSInvocation (and a
     * return-data allocation) for every objc_msgSend.  During rendering that
     * retained hundreds of thousands of invocations in the outer AppKit pool
     * and grew the native heap by roughly 100 MiB in a few minutes.
     *
     * Returned Objective-C objects are registered in proxy_for_object before
     * leaving this scope, which retains them.  It is therefore safe, and much
     * closer to an FFI call boundary, to drain all other host temporaries at
     * the end of each dispatch.  @autoreleasepool cleanup also runs on every
     * early return from the body.
     *
     * OpenGL entry points are the bulk of the per-frame traffic (10-30
     * thousand calls) and never create Objective-C temporaries, so they skip
     * the pool push/pop entirely.
     */
    @autoreleasepool { if (peggle_mac_dispatch(import_name, arguments, result)) return 1; }
    const size_t import_length = strlen(import_name);
    bool is_gl = (import_name[0] == 'g' && import_name[1] == 'l') ||
                 (import_name[0] == '_' && import_name[1] == 'g' &&
                  import_name[2] == 'l');
    if (is_gl) {
        return objc_bridge32_dispatch_body(import_name, import_length,
                                           arguments, result);
    }
    @autoreleasepool {
        return objc_bridge32_dispatch_body(import_name, import_length,
                                           arguments, result);
    }
}

void objc_bridge32_open_url(const char *url)
{
    if (!url || !url[0]) return;
    @autoreleasepool {
        NSString *text = [[NSString stringWithUTF8String:url]
            stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        NSURL *target = [text hasPrefix:@"/"] ? [NSURL fileURLWithPath:text]
                                              : [NSURL URLWithString:text];
        fprintf(stderr, "compat32: open %s\n", url);
        if (target) [[NSWorkspace sharedWorkspace] openURL:target];
    }
}

static int objc_bridge32_dispatch_body(const char *import_name,
                                       size_t import_length,
                                       const uint32_t *arguments,
                                       uint64_t *result)
{
    if (LP32_NAME_IS(import_name, import_length, "_CGMainDisplayID")) {
        *result = preferred_game_display_id();
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayIDToOpenGLDisplayMask")) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        *result = CGDisplayIDToOpenGLDisplayMask(arguments[0]);
#pragma clang diagnostic pop
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayAvailableModes")) {
        materialize_automatic_display_mode();
        NSArray *modes = legacy_modes_for_display(arguments[0]);
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: display %u has %lu legacy modes\n",
                    arguments[0], (unsigned long)[modes count]);
        }
        *result = proxy_for_object(modes);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayCurrentMode")) {
        materialize_automatic_display_mode();
        NSDictionary *mode = legacy_current_mode_for_display(arguments[0]);
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: current legacy display mode: %s\n",
                    [[mode description] UTF8String]);
        }
        *result = proxy_for_object(mode);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGWindowLevelForKey")) {
        *result = (uint32_t)CGWindowLevelForKey((CGWindowLevelKey)arguments[0]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayIOServicePort")) {
        /* Removed by Apple; callers use zero as "no registry service". */
        *result = 0;
        return 1;
    }
    /*
     * Display capture and mode switching. The
     * bridge presents the game in a window that already covers the preferred
     * display, and modern macOS has no exclusive fullscreen or CGL fullscreen
     * drawables, so capture/fade succeed without doing anything, the "best
     * mode" is whatever legacy mode matches the request on that display (or
     * one synthesized for it, since the emulation can render any size), and
     * a mode switch becomes the emulated mode (see guest_display_mode) that
     * the GL surface is rendered at and that later queries report back.
     */
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplaySwitchToMode")) {
        NSDictionary *mode = object_for_argument(arguments[1]);
        NSSize requested = NSMakeSize([[mode objectForKey:@"Width"] doubleValue],
                                      [[mode objectForKey:@"Height"] doubleValue]);
        if (arguments[0] == preferred_game_display_id() && mode) {
            set_guest_display_mode(requested);
            [presenting_view applyGuestDisplayMode];
        }
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: _CGDisplaySwitchToMode %.0fx%.0f (emulated)\n",
                    requested.width, requested.height);
        }
        *result = kCGErrorSuccess;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayRelease") ||
        LP32_NAME_IS(import_name, import_length, "_CGReleaseAllDisplays")) {
        /* Releasing the captured display(s) restores the desktop mode.  The
           game calls the AllDisplays variant when leaving fullscreen (e.g. on
           ESC); leaving it unhandled trapped and then crashed (issue #3). */
        set_guest_display_mode(NSZeroSize);
        [presenting_view applyGuestDisplayMode];
        *result = kCGErrorSuccess;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayCapture") ||
        LP32_NAME_IS(import_name, import_length, "_CGCaptureAllDisplays") ||
        LP32_NAME_IS(import_name, import_length, "_CGCaptureAllDisplaysWithOptions") ||
        LP32_NAME_IS(import_name, import_length, "_CGDisplayHideCursor") ||
        LP32_NAME_IS(import_name, import_length, "_CGDisplayShowCursor") ||
        LP32_NAME_IS(import_name, import_length, "_CGDisplayFade") ||
        LP32_NAME_IS(import_name, import_length, "_CGReleaseDisplayFadeReservation")) {
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: %s ignored (windowed presentation)\n",
                    import_name);
        }
        *result = kCGErrorSuccess;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGAcquireDisplayFadeReservation")) {
        uint32_t *token = (void *)(uintptr_t)arguments[1];
        if (token) *token = 1;
        *result = kCGErrorSuccess;
        return 1;
    }
    /* Gamma ramp: the game saves the display transfer table, then drives fades
       by setting it and restores it on exit.  Windowed presentation has no
       display gamma to touch, so report an identity ramp and swallow the sets;
       fades still run in the game's own compositing. */
    if (LP32_NAME_IS(import_name, import_length, "_CGGetDisplayTransferByTable")) {
        uint32_t capacity = arguments[1];
        float *red = (void *)(uintptr_t)arguments[2];
        float *green = (void *)(uintptr_t)arguments[3];
        float *blue = (void *)(uintptr_t)arguments[4];
        uint32_t *sample_count = (void *)(uintptr_t)arguments[5];
        for (uint32_t i = 0; i < capacity; ++i) {
            float v = capacity > 1 ? (float)i / (float)(capacity - 1) : 0.0f;
            if (red) red[i] = v;
            if (green) green[i] = v;
            if (blue) blue[i] = v;
        }
        if (sample_count) *sample_count = capacity;
        *result = kCGErrorSuccess;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGSetDisplayTransferByTable") ||
        LP32_NAME_IS(import_name, import_length, "_CGSetDisplayTransferByFormula") ||
        LP32_NAME_IS(import_name, import_length, "_CGDisplayRestoreColorSyncSettings")) {
        *result = kCGErrorSuccess;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayIsCaptured")) {
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGShieldingWindowLevel")) {
        *result = (uint32_t)CGShieldingWindowLevel();
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayBestModeForParameters")) {
        materialize_automatic_display_mode();
        uint32_t width = arguments[2];
        uint32_t height = arguments[3];
        uint32_t *exact_match = (void *)(uintptr_t)arguments[4];
        NSDictionary *best = nil;
        /* First launch (no pcconfig.txt yet): the binary's built-in 1024x768
           only stood in for the resolution an old launcher dialog chose, and
           that dialog is bypassed, so answer "no such mode"; the game then
           adopts the display's current mode and writes it to pcconfig.txt,
           as the launcher would have proposed.  Later launches honour the
           file (and the in-game Options). */
        static bool applied_first_launch_default;
        bool first_launch = compat_runtime32_game_config_missing() &&
            !applied_first_launch_default && !getenv("LP32_KEEP_DEFAULT_RESOLUTION");
        if (first_launch) {
            applied_first_launch_default = true;
            NSSize native = preferred_display_native_size();
            /* The game re-validates its ScreenWidth/ScreenHeight against the
               mode list after this call, so move those to the native size as
               well or it would go back to the placeholder. */
            const struct lp32_display_layout *display = lp32_profile()->display;
            if (display && display->screen_width && display->screen_height) {
                *(volatile int32_t *)(uintptr_t)display->screen_width =
                    (int32_t)native.width;
                *(volatile int32_t *)(uintptr_t)display->screen_height =
                    (int32_t)native.height;
            }
            fprintf(stderr,
                    "compat32: no pcconfig.txt; starting at the display resolution "
                    "%.0fx%.0f instead of %ux%u\n", native.width, native.height,
                    width, height);
        }
        for (NSDictionary *candidate in legacy_modes_for_display(arguments[0])) {
            if (first_launch) break;
            if ([[candidate objectForKey:@"Width"] unsignedIntValue] == width &&
                [[candidate objectForKey:@"Height"] unsignedIntValue] == height) {
                best = candidate;
                break;
            }
        }
        if (!best && !first_launch) {
            best = synthesized_legacy_mode(arguments[0], width, height);
        }
        if (exact_match) *exact_match = best ? 1 : 0;
        if (!best) best = legacy_current_mode_for_display(arguments[0]);
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr,
                    "compat32: CGDisplayBestModeForParameters %ux%u@%ubpp -> %s\n",
                    width, height, arguments[1], [[best description] UTF8String]);
        }
        *result = proxy_for_object(best);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLChoosePixelFormat")) {
        /* Attributes are an int-terminated list; strip kCGLPFAFullScreen
           (no longer honoured) and let CGL pick the rest. */
        const int32_t *guest_attributes = (const void *)(uintptr_t)arguments[0];
        CGLPixelFormatAttribute attributes[64];
        size_t count = 0;
        while (guest_attributes && guest_attributes[count] && count < 62) {
            attributes[count] = (CGLPixelFormatAttribute)guest_attributes[count];
            ++count;
            /* Attributes taking a value: skip the value too. */
            switch (attributes[count - 1]) {
                case kCGLPFAAuxBuffers: case kCGLPFAColorSize:
                case kCGLPFAAlphaSize: case kCGLPFADepthSize:
                case kCGLPFAStencilSize: case kCGLPFAAccumSize:
                case kCGLPFASampleBuffers: case kCGLPFASamples:
                case kCGLPFARendererID: case kCGLPFADisplayMask:
                case kCGLPFAOpenGLProfile: case kCGLPFAVirtualScreenCount:
                    attributes[count] = (CGLPixelFormatAttribute)guest_attributes[count];
                    ++count;
                    break;
                default:
                    break;
            }
        }
        attributes[count] = 0;
        size_t write = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        for (size_t read = 0; read < count; ++read) {
            if (attributes[read] == kCGLPFAFullScreen) continue;
            attributes[write++] = attributes[read];
        }
#pragma clang diagnostic pop
        attributes[write] = 0;
        CGLPixelFormatObj pixel_format = NULL;
        GLint virtual_screens = 0;
        CGLError error = CGLChoosePixelFormat(attributes, &pixel_format,
                                              &virtual_screens);
        uint32_t *guest_pixel_format = (void *)(uintptr_t)arguments[1];
        int32_t *guest_count = (void *)(uintptr_t)arguments[2];
        if (guest_pixel_format) {
            *guest_pixel_format = pixel_format ?
                proxy_for_object([NSValue valueWithPointer:pixel_format]) : 0;
        }
        if (guest_count) *guest_count = virtual_screens;
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: CGLChoosePixelFormat error=%d screens=%d\n",
                    error, virtual_screens);
        }
        *result = (uint32_t)error;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLCreateContext")) {
        NSValue *format_wrapper = object_for_argument(arguments[0]);
        NSValue *share_wrapper = object_for_argument(arguments[1]);
        CGLPixelFormatObj pixel_format =
            format_wrapper ? [format_wrapper pointerValue] : NULL;
        CGLContextObj share = share_wrapper ? [share_wrapper pointerValue] : NULL;
        CGLContextObj context = NULL;
        CGLError error = pixel_format ?
            CGLCreateContext(pixel_format, share, &context) : kCGLBadPixelFormat;
        uint32_t *guest_context = (void *)(uintptr_t)arguments[2];
        if (guest_context) {
            *guest_context = context ?
                proxy_for_object([NSValue valueWithPointer:context]) : 0;
        }
        *result = (uint32_t)error;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLDestroyContext")) {
        NSValue *wrapper = object_for_argument(arguments[0]);
        CGLContextObj context = wrapper ? [wrapper pointerValue] : NULL;
        *result = context ? (uint32_t)CGLDestroyContext(context) : kCGLBadContext;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLDestroyPixelFormat")) {
        NSValue *wrapper = object_for_argument(arguments[0]);
        CGLPixelFormatObj pixel_format = wrapper ? [wrapper pointerValue] : NULL;
        *result = pixel_format ?
            (uint32_t)CGLDestroyPixelFormat(pixel_format) : kCGLBadPixelFormat;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLSetFullScreen") ||
        LP32_NAME_IS(import_name, import_length, "_CGLClearDrawable")) {
        /* The drawable is always the window's NSOpenGLView. */
        *result = kCGLNoError;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLQueryRendererInfo")) {
        CGLRendererInfoObj info = NULL;
        GLint count = 0;
        CGLError error = CGLQueryRendererInfo(arguments[0], &info, &count);
        uint32_t *guest_info = (void *)(uintptr_t)arguments[1];
        int32_t *guest_count = (void *)(uintptr_t)arguments[2];
        if (guest_info) {
            *guest_info = info ? proxy_for_object([NSValue valueWithPointer:info]) : 0;
        }
        if (guest_count) *guest_count = count;
        *result = (uint32_t)error;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLDescribeRenderer")) {
        NSValue *wrapper = object_for_argument(arguments[0]);
        CGLRendererInfoObj info = wrapper ? [wrapper pointerValue] : NULL;
        GLint value = 0;
        CGLError error = info ?
            CGLDescribeRenderer(info, (GLint)arguments[1],
                                (CGLRendererProperty)arguments[2], &value) :
            kCGLBadRendererInfo;
        /* Old titles size texture budgets from kCGLRPVideoMemory in bytes,
           which overflowed 32 bits long ago; report a comfortable 1 GiB. */
        if (error == kCGLNoError &&
            (arguments[2] == kCGLRPVideoMemoryMegabytes ||
             arguments[2] == kCGLRPTextureMemoryMegabytes)) {
            if (value > 1024) value = 1024;
        }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (arguments[2] == kCGLRPVideoMemory || arguments[2] == kCGLRPTextureMemory) {
            value = 1024 * 1024 * 1024;
            error = kCGLNoError;
        }
#pragma clang diagnostic pop
        int32_t *guest_value = (void *)(uintptr_t)arguments[3];
        if (guest_value) *guest_value = value;
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: CGLDescribeRenderer renderer=%d prop=%u -> %d (err=%d)\n",
                    (int)arguments[1], arguments[2], value, error);
        }
        *result = (uint32_t)error;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLDestroyRendererInfo")) {
        NSValue *wrapper = object_for_argument(arguments[0]);
        CGLRendererInfoObj info = wrapper ? [wrapper pointerValue] : NULL;
        *result = info ? (uint32_t)CGLDestroyRendererInfo(info) : kCGLBadRendererInfo;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLGetCurrentContext")) {
        CGLContextObj context = CGLGetCurrentContext();
        *result = context ? proxy_for_object([NSValue valueWithPointer:context]) : 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLSetCurrentContext") ||
        LP32_NAME_IS(import_name, import_length, "_CGLLockContext") ||
        LP32_NAME_IS(import_name, import_length, "_CGLUnlockContext") ||
        LP32_NAME_IS(import_name, import_length, "_CGLSetParameter")) {
        NSValue *wrapper = object_for_argument(arguments[0]);
        CGLContextObj context = wrapper ? [wrapper pointerValue] : NULL;
        CGLError error;
        if (LP32_NAME_IS(import_name, import_length, "_CGLSetCurrentContext")) {
            error = CGLSetCurrentContext(context);
        } else if (LP32_NAME_IS(import_name, import_length, "_CGLLockContext")) {
            error = CGLLockContext(context);
        } else if (LP32_NAME_IS(import_name, import_length, "_CGLUnlockContext")) {
            error = CGLUnlockContext(context);
        } else {
            const GLint *values = (const GLint *)(uintptr_t)arguments[2];
            GLint override_value = 0;
            if (arguments[1] == kCGLCPSwapInterval && values &&
                frame_pacer_takes_over_swap_interval(*values)) {
                values = &override_value;
            }
            error = CGLSetParameter(context, arguments[1], values);
        }
        *result = (uint32_t)error;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glGetError")) {
        *result = fast_glGetError(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glActiveTexture") ||
        LP32_NAME_IS(import_name, import_length, "glActiveTextureARB")) {
        *result = fast_glActiveTexture(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glClientActiveTexture") ||
        LP32_NAME_IS(import_name, import_length, "glClientActiveTextureARB")) {
        *result = fast_glClientActiveTexture(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGenBuffers") ||
        LP32_NAME_IS(import_name, import_length, "glGenBuffersARB")) {
        glGenBuffers(arguments[0], (GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDeleteBuffers") ||
        LP32_NAME_IS(import_name, import_length, "glDeleteBuffersARB")) {
        const GLuint *buffers = (const void *)(uintptr_t)arguments[1];
        CGLContextObj context = CGLGetCurrentContext();
        for (GLsizei index = 0; index < (GLsizei)arguments[0]; ++index) {
            discard_buffer_state(context, buffers[index]);
        }
        glDeleteBuffers(arguments[0], buffers);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBindBuffer") ||
        LP32_NAME_IS(import_name, import_length, "glBindBufferARB")) {
        *result = fast_glBindBuffer(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBufferData") ||
        LP32_NAME_IS(import_name, import_length, "glBufferDataARB")) {
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glBufferData swap=%llu target=%04x size=%d "
                "data=%08x usage=%04x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                (int32_t)arguments[1], arguments[2], arguments[3]);
        }
        glBufferData(arguments[0], (GLsizeiptr)(int32_t)arguments[1],
                     (const void *)(uintptr_t)arguments[2], arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBufferSubData") ||
        LP32_NAME_IS(import_name, import_length, "glBufferSubDataARB")) {
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glBufferSubData swap=%llu target=%04x offset=%d "
                "size=%d data=%08x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                (int32_t)arguments[1], (int32_t)arguments[2], arguments[3]);
        }
        glBufferSubData(arguments[0], (GLintptr)(int32_t)arguments[1],
                        (GLsizeiptr)(int32_t)arguments[2],
                        (const void *)(uintptr_t)arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBufferParameteriAPPLE")) {
        CGLContextObj context = CGLGetCurrentContext();
        GLuint buffer = bound_buffer_for_target(arguments[0]);
        struct guest_buffer_state *state =
            buffer_state_for_name(context, buffer, true);
        if (state && arguments[1] == GL_BUFFER_FLUSHING_UNMAP_APPLE) {
            state->flush_on_unmap = arguments[2] != 0;
        }
        if (trace_gl_buffer_call()) {
            fprintf(stderr,
                    "compat32: glBufferParameteriAPPLE swap=%llu "
                    "target=%04x buffer=%u pname=%04x value=%u "
                    "flush_unmap=%d\n",
                    (unsigned long long)objc_bridge_swap_count, arguments[0],
                    buffer, arguments[1], arguments[2],
                    state ? state->flush_on_unmap : -1);
        }
        glBufferParameteriAPPLE(arguments[0], arguments[1], arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glMapBuffer") ||
        LP32_NAME_IS(import_name, import_length, "glMapBufferARB")) {
        GLint byte_count = 0;
        glGetBufferParameteriv(arguments[0], GL_BUFFER_SIZE, &byte_count);
        CGLContextObj context = CGLGetCurrentContext();
        GLuint buffer = bound_buffer_for_target(arguments[0]);
        struct guest_buffer_state *state =
            buffer_state_for_name(context, buffer, true);
        struct guest_buffer_mapping *mapping = state ? begin_buffer_mapping(
            context, arguments[0], buffer, state) : NULL;
        if (!mapping || byte_count < 0) {
            deactivate_buffer_mapping(mapping);
            *result = 0;
            return 1;
        }
        size_t size = (size_t)byte_count;
        if (size > mapping->capacity) {
            uint32_t storage = compat_runtime32_reallocate(
                mapping->storage, size ? size : 1);
            if (!storage) {
                deactivate_buffer_mapping(mapping);
                *result = 0;
                return 1;
            }
            mapping->storage = storage;
            mapping->capacity = size;
        }
        mapping->access = arguments[1];
        mapping->access_flags = 0;
        mapping->buffer_size = size;
        mapping->mapped_offset = 0;
        mapping->size = size;
        mapping->range_mapping = false;
        if (trace_gl_buffer_call()) {
            fprintf(stderr,
                    "compat32: glMapBuffer swap=%llu target=%04x "
                    "buffer=%u access=%04x size=%zu staging=%08x "
                    "flush_unmap=%d\n",
                    (unsigned long long)objc_bridge_swap_count,
                    mapping->target, buffer, mapping->access, mapping->size,
                    mapping->storage, state->flush_on_unmap);
        }
        if (size && (mapping->access != GL_WRITE_ONLY ||
                     state->flush_on_unmap)) {
            glGetBufferSubData(mapping->target, 0, (GLsizeiptr)size,
                               (void *)(uintptr_t)mapping->storage);
        }
        *result = mapping->storage;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glMapBufferRange")) {
        GLint byte_count = 0;
        glGetBufferParameteriv(arguments[0], GL_BUFFER_SIZE, &byte_count);
        int32_t signed_offset = (int32_t)arguments[1];
        int32_t signed_size = (int32_t)arguments[2];
        CGLContextObj context = CGLGetCurrentContext();
        GLuint buffer = bound_buffer_for_target(arguments[0]);
        struct guest_buffer_state *state =
            buffer_state_for_name(context, buffer, true);
        struct guest_buffer_mapping *mapping = state ? begin_buffer_mapping(
            context, arguments[0], buffer, state) : NULL;
        bool valid = mapping && byte_count >= 0 && signed_offset >= 0 &&
            signed_size >= 0 && (size_t)signed_offset <= (size_t)byte_count &&
            (size_t)signed_size <= (size_t)byte_count - (size_t)signed_offset;
        if (!valid) {
            deactivate_buffer_mapping(mapping);
            *result = 0;
            return 1;
        }
        size_t size = (size_t)signed_size;
        if (size > mapping->capacity) {
            uint32_t storage = compat_runtime32_reallocate(
                mapping->storage, size ? size : 1);
            if (!storage) {
                deactivate_buffer_mapping(mapping);
                *result = 0;
                return 1;
            }
            mapping->storage = storage;
            mapping->capacity = size;
        }
        mapping->access = 0;
        mapping->access_flags = arguments[3];
        mapping->buffer_size = (size_t)byte_count;
        mapping->mapped_offset = (size_t)signed_offset;
        mapping->size = size;
        mapping->range_mapping = true;
        bool readable = (mapping->access_flags & GL_MAP_READ_BIT) != 0;
        bool preserve = (mapping->access_flags &
                         (GL_MAP_INVALIDATE_RANGE_BIT |
                          GL_MAP_INVALIDATE_BUFFER_BIT)) == 0;
        if (size && (readable || preserve)) {
            glGetBufferSubData(mapping->target,
                               (GLintptr)mapping->mapped_offset,
                               (GLsizeiptr)mapping->size,
                               (void *)(uintptr_t)mapping->storage);
        }
        if (trace_gl_buffer_call()) {
            fprintf(stderr,
                    "compat32: glMapBufferRange swap=%llu target=%04x "
                    "buffer=%u offset=%zu size=%zu access=%04x "
                    "staging=%08x preserve=%d\n",
                    (unsigned long long)objc_bridge_swap_count,
                    mapping->target, buffer, mapping->mapped_offset,
                    mapping->size, mapping->access_flags, mapping->storage,
                    preserve);
        }
        *result = mapping->storage;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glUnmapBuffer") ||
        LP32_NAME_IS(import_name, import_length, "glUnmapBufferARB")) {
        CGLContextObj context = CGLGetCurrentContext();
        GLuint buffer = bound_buffer_for_target(arguments[0]);
        struct guest_buffer_mapping *mapping =
            active_buffer_mapping(context, arguments[0], buffer);
        struct guest_buffer_state *state = mapping ? mapping->state : NULL;
        if (!mapping || !state || !mapping->storage) {
            *result = GL_FALSE;
            return 1;
        }
        bool writable = mapping->range_mapping ?
            (mapping->access_flags & GL_MAP_WRITE_BIT) != 0 :
            mapping->access != GL_READ_ONLY;
        bool explicit_flush = mapping->range_mapping &&
            (mapping->access_flags & GL_MAP_FLUSH_EXPLICIT_BIT) != 0;
        bool upload_all = writable && !explicit_flush &&
            (mapping->range_mapping || state->flush_on_unmap);
        if (mapping->size && upload_all) {
            glBufferSubData(mapping->target,
                            (GLintptr)mapping->mapped_offset,
                            (GLsizeiptr)mapping->size,
                            (const void *)(uintptr_t)mapping->storage);
        }
        if (trace_gl_buffer_call()) {
            fprintf(stderr,
                    "compat32: glUnmapBuffer swap=%llu target=%04x "
                    "buffer=%u offset=%zu size=%zu access=%04x "
                    "range=%d uploaded_all=%d\n",
                    (unsigned long long)objc_bridge_swap_count,
                    mapping->target, buffer, mapping->mapped_offset,
                    mapping->size,
                    mapping->range_mapping ? mapping->access_flags :
                                             mapping->access,
                    mapping->range_mapping, upload_all);
        }
        deactivate_buffer_mapping(mapping);
        *result = GL_TRUE;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glFlushMappedBufferRangeAPPLE")) {
        CGLContextObj context = CGLGetCurrentContext();
        GLuint buffer = bound_buffer_for_target(arguments[0]);
        struct guest_buffer_mapping *mapping =
            active_buffer_mapping(context, arguments[0], buffer);
        struct guest_buffer_state *state = mapping ? mapping->state : NULL;
        size_t offset = arguments[1];
        size_t size = arguments[2];
        bool valid = mapping && state && mapping->storage &&
            mapping->access != GL_READ_ONLY && offset <= mapping->size &&
            size <= mapping->size - offset;
        if (valid) {
            glBufferSubData(mapping->target, (GLintptr)offset,
                            (GLsizeiptr)size,
                            (const void *)(uintptr_t)(mapping->storage + offset));
        }
        if (trace_gl_buffer_call()) {
            fprintf(stderr,
                    "compat32: glFlushMappedBufferRangeAPPLE swap=%llu "
                    "target=%04x buffer=%u offset=%zu size=%zu valid=%d\n",
                    (unsigned long long)objc_bridge_swap_count, arguments[0],
                    buffer, offset, size, valid);
        }
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glFlushMappedBufferRange")) {
        CGLContextObj context = CGLGetCurrentContext();
        GLuint buffer = bound_buffer_for_target(arguments[0]);
        struct guest_buffer_mapping *mapping =
            active_buffer_mapping(context, arguments[0], buffer);
        int32_t signed_offset = (int32_t)arguments[1];
        int32_t signed_size = (int32_t)arguments[2];
        size_t offset = signed_offset >= 0 ? (size_t)signed_offset : SIZE_MAX;
        size_t size = signed_size >= 0 ? (size_t)signed_size : SIZE_MAX;
        bool valid = mapping && mapping->range_mapping && mapping->storage &&
            (mapping->access_flags & GL_MAP_WRITE_BIT) != 0 &&
            offset <= mapping->size && size <= mapping->size - offset;
        if (valid) {
            glBufferSubData(mapping->target,
                            (GLintptr)(mapping->mapped_offset + offset),
                            (GLsizeiptr)size,
                            (const void *)(uintptr_t)(mapping->storage + offset));
        }
        if (trace_gl_buffer_call()) {
            fprintf(stderr,
                    "compat32: glFlushMappedBufferRange swap=%llu "
                    "target=%04x buffer=%u relative_offset=%zu "
                    "absolute_offset=%zu size=%zu valid=%d\n",
                    (unsigned long long)objc_bridge_swap_count, arguments[0],
                    buffer, offset,
                    valid ? mapping->mapped_offset + offset : SIZE_MAX,
                    size, valid);
        }
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGetBufferParameteriv") ||
        LP32_NAME_IS(import_name, import_length, "glGetBufferParameterivARB")) {
        glGetBufferParameteriv(arguments[0], arguments[1],
                               (GLint *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glVertexAttribPointer") ||
        LP32_NAME_IS(import_name, import_length, "glVertexAttribPointerARB")) {
        *result = fast_glVertexAttribPointer(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glEnableVertexAttribArray") ||
        LP32_NAME_IS(import_name, import_length, "glEnableVertexAttribArrayARB")) {
        *result = fast_glEnableVertexAttribArray(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDisableVertexAttribArray") ||
        LP32_NAME_IS(import_name, import_length, "glDisableVertexAttribArrayARB")) {
        *result = fast_glDisableVertexAttribArray(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glTexImage3D") ||
        LP32_NAME_IS(import_name, import_length, "glTexImage3DEXT")) {
        ++gl_frame_diagnostics.texture_uploads;
        note_texture_level_change();
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glTexImage3D swap=%llu target=%04x level=%d "
                "internal=%04x size=%dx%dx%d border=%d format=%04x "
                "type=%04x pixels=%08x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                (GLint)arguments[1], arguments[2],
                (GLsizei)arguments[3], (GLsizei)arguments[4],
                (GLsizei)arguments[5], (GLint)arguments[6], arguments[7],
                arguments[8], arguments[9]);
        }
        glTexImage3D(arguments[0], (GLint)arguments[1],
                     (GLint)arguments[2], (GLsizei)arguments[3],
                     (GLsizei)arguments[4], (GLsizei)arguments[5],
                     (GLint)arguments[6], arguments[7], arguments[8],
                     (const void *)(uintptr_t)arguments[9]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glTexSubImage3D") ||
        LP32_NAME_IS(import_name, import_length, "glTexSubImage3DEXT")) {
        ++gl_frame_diagnostics.texture_uploads;
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glTexSubImage3D swap=%llu target=%04x level=%d "
                "offset={%d,%d,%d} size=%dx%dx%d format=%04x type=%04x "
                "pixels=%08x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                (GLint)arguments[1], (GLint)arguments[2],
                (GLint)arguments[3], (GLint)arguments[4],
                (GLsizei)arguments[5], (GLsizei)arguments[6],
                (GLsizei)arguments[7], arguments[8], arguments[9],
                arguments[10]);
        }
        glTexSubImage3D(
            arguments[0], (GLint)arguments[1], (GLint)arguments[2],
            (GLint)arguments[3], (GLint)arguments[4],
            (GLsizei)arguments[5], (GLsizei)arguments[6],
            (GLsizei)arguments[7], arguments[8], arguments[9],
            (const void *)(uintptr_t)arguments[10]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glCopyTexSubImage3D") ||
        LP32_NAME_IS(import_name, import_length, "glCopyTexSubImage3DEXT")) {
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glCopyTexSubImage3D swap=%llu target=%04x "
                "level=%d offset={%d,%d,%d} source={%d,%d} size=%dx%d\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                (GLint)arguments[1], (GLint)arguments[2],
                (GLint)arguments[3], (GLint)arguments[4],
                (GLint)arguments[5], (GLint)arguments[6],
                (GLsizei)arguments[7], (GLsizei)arguments[8]);
        }
        glCopyTexSubImage3D(
            arguments[0], (GLint)arguments[1], (GLint)arguments[2],
            (GLint)arguments[3], (GLint)arguments[4],
            (GLint)arguments[5], (GLint)arguments[6],
            (GLsizei)arguments[7], (GLsizei)arguments[8]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glCompressedTexImage2D") ||
        LP32_NAME_IS(import_name, import_length, "glCompressedTexImage2DARB")) {
        ++gl_frame_diagnostics.texture_uploads;
        note_texture_level_change();
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glCompressedTexImage2D swap=%llu target=%04x "
                "level=%d internal=%04x size=%dx%d border=%d bytes=%d "
                "pixels=%08x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                (GLint)arguments[1], arguments[2], (GLsizei)arguments[3],
                (GLsizei)arguments[4], (GLint)arguments[5],
                (GLsizei)arguments[6], arguments[7]);
        }
        glCompressedTexImage2D(arguments[0], (GLint)arguments[1], arguments[2],
                               (GLsizei)arguments[3], (GLsizei)arguments[4],
                               (GLint)arguments[5], (GLsizei)arguments[6],
                               (const void *)(uintptr_t)arguments[7]);
        repair_bound_texture_mipmap_range(arguments[0]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glCompressedTexSubImage2D") ||
        LP32_NAME_IS(import_name, import_length, "glCompressedTexSubImage2DARB")) {
        ++gl_frame_diagnostics.texture_uploads;
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glCompressedTexSubImage2D swap=%llu target=%04x "
                "level=%d offset={%d,%d} size=%dx%d format=%04x "
                "bytes=%d pixels=%08x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                (GLint)arguments[1], (GLint)arguments[2],
                (GLint)arguments[3], (GLsizei)arguments[4],
                (GLsizei)arguments[5], arguments[6],
                (GLsizei)arguments[7], arguments[8]);
        }
        glCompressedTexSubImage2D(
            arguments[0], (GLint)arguments[1], (GLint)arguments[2],
            (GLint)arguments[3], (GLsizei)arguments[4],
            (GLsizei)arguments[5], arguments[6], (GLsizei)arguments[7],
            (const void *)(uintptr_t)arguments[8]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDrawRangeElements") ||
        LP32_NAME_IS(import_name, import_length, "glDrawRangeElementsEXT")) {
        *result = fast_glDrawRangeElements(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGenQueries") ||
        LP32_NAME_IS(import_name, import_length, "glGenQueriesARB")) {
        glGenQueries((GLsizei)arguments[0],
                     (GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDeleteQueries") ||
        LP32_NAME_IS(import_name, import_length, "glDeleteQueriesARB")) {
        glDeleteQueries((GLsizei)arguments[0],
                        (const GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glIsQuery") ||
        LP32_NAME_IS(import_name, import_length, "glIsQueryARB")) {
        *result = glIsQuery((GLuint)arguments[0]); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBeginQuery") ||
        LP32_NAME_IS(import_name, import_length, "glBeginQueryARB")) {
        if (trace_gl_render_call()) {
            fprintf(stderr,
                    "compat32: glBeginQuery swap=%llu target=%04x id=%u\n",
                    (unsigned long long)objc_bridge_swap_count,
                    arguments[0], arguments[1]);
        }
        glBeginQuery(arguments[0], (GLuint)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glEndQuery") ||
        LP32_NAME_IS(import_name, import_length, "glEndQueryARB")) {
        if (trace_gl_render_call()) {
            fprintf(stderr, "compat32: glEndQuery swap=%llu target=%04x\n",
                    (unsigned long long)objc_bridge_swap_count, arguments[0]);
        }
        glEndQuery(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGetQueryiv") ||
        LP32_NAME_IS(import_name, import_length, "glGetQueryivARB")) {
        glGetQueryiv(arguments[0], arguments[1],
                     (GLint *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGetQueryObjectiv") ||
        LP32_NAME_IS(import_name, import_length, "glGetQueryObjectivARB")) {
        glGetQueryObjectiv((GLuint)arguments[0], arguments[1],
                           (GLint *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGetQueryObjectuiv") ||
        LP32_NAME_IS(import_name, import_length, "glGetQueryObjectuivARB")) {
        glGetQueryObjectuiv((GLuint)arguments[0], arguments[1],
                            (GLuint *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGenFramebuffers") ||
        LP32_NAME_IS(import_name, import_length, "glGenFramebuffersEXT")) {
        glGenFramebuffers(arguments[0], (GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDeleteFramebuffers") ||
        LP32_NAME_IS(import_name, import_length, "glDeleteFramebuffersEXT")) {
        glDeleteFramebuffers(arguments[0],
                             (const GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBindFramebuffer") ||
        LP32_NAME_IS(import_name, import_length, "glBindFramebufferEXT")) {
        ++gl_frame_diagnostics.framebuffer_binds;
        if (trace_gl_render_call()) {
            fprintf(stderr,
                    "compat32: glBindFramebuffer swap=%llu target=%04x fbo=%u\n",
                    (unsigned long long)objc_bridge_swap_count,
                    arguments[0], arguments[1]);
        }
        glBindFramebuffer(arguments[0], arguments[1]);
        *result = 0; return 1;
    }
    /* Bejeweled 3's renderer (unlike Peggle's fixed-function path) drives
       EXT framebuffer objects, GLSL shaders and client-state vertex arrays.
       Guest pointers are identity-mapped into the host address space, so they
       forward directly; the calls below complete the legacy-profile GL surface
       the game needs.  Deprecation is silenced file-wide (GL_SILENCE_DEPRECATION). */
    if (LP32_NAME_IS(import_name, import_length, "_glGenFramebuffersEXT")) {
        glGenFramebuffersEXT((GLsizei)arguments[0], (GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glBindFramebufferEXT")) {
        glBindFramebufferEXT(arguments[0], arguments[1]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glDeleteFramebuffersEXT")) {
        glDeleteFramebuffersEXT((GLsizei)arguments[0], (const GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glGenRenderbuffersEXT")) {
        glGenRenderbuffersEXT((GLsizei)arguments[0], (GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glBindRenderbufferEXT")) {
        glBindRenderbufferEXT(arguments[0], arguments[1]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glDeleteRenderbuffersEXT")) {
        glDeleteRenderbuffersEXT((GLsizei)arguments[0], (const GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glRenderbufferStorageEXT")) {
        glRenderbufferStorageEXT(arguments[0], arguments[1], (GLsizei)arguments[2],
                                 (GLsizei)arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glFramebufferRenderbufferEXT")) {
        glFramebufferRenderbufferEXT(arguments[0], arguments[1], arguments[2], arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glFramebufferTexture2DEXT")) {
        glFramebufferTexture2DEXT(arguments[0], arguments[1], arguments[2], arguments[3],
                                  (GLint)arguments[4]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glCheckFramebufferStatusEXT")) {
        *result = glCheckFramebufferStatusEXT(arguments[0]); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glCreateShader")) {
        *result = glCreateShader(arguments[0]); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glCreateProgram")) {
        *result = glCreateProgram(); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glCompileShader")) {
        glCompileShader(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glAttachShader")) {
        glAttachShader(arguments[0], arguments[1]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glLinkProgram")) {
        glLinkProgram(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glUseProgram")) {
        glUseProgram(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glDeleteShader")) {
        glDeleteShader(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glGetShaderiv")) {
        glGetShaderiv(arguments[0], arguments[1], (GLint *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glGetProgramiv")) {
        glGetProgramiv(arguments[0], arguments[1], (GLint *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glGetUniformLocation")) {
        *result = (uint32_t)glGetUniformLocation(arguments[0],
                                                 (const GLchar *)(uintptr_t)arguments[1]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glUniform1i")) {
        glUniform1i((GLint)arguments[0], (GLint)arguments[1]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glUniform4fv")) {
        glUniform4fv((GLint)arguments[0], (GLsizei)arguments[1],
                     (const GLfloat *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glUniformMatrix4fv")) {
        glUniformMatrix4fv((GLint)arguments[0], (GLsizei)arguments[1],
                           (GLboolean)arguments[2], (const GLfloat *)(uintptr_t)arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glShaderSource")) {
        GLsizei count = (GLsizei)arguments[1];
        const uint32_t *guest_strings = (const void *)(uintptr_t)arguments[2];
        const uint32_t *guest_lengths = (const void *)(uintptr_t)arguments[3];
        const GLchar **strings = NULL;
        GLint *lengths = NULL;
        if (count > 0 && guest_strings) {
            strings = malloc(sizeof(*strings) * (size_t)count);
            if (guest_lengths) lengths = malloc(sizeof(*lengths) * (size_t)count);
            for (GLsizei i = 0; i < count; ++i) {
                strings[i] = (const GLchar *)(uintptr_t)guest_strings[i];
                if (lengths) lengths[i] = (GLint)guest_lengths[i];
            }
        }
        glShaderSource(arguments[0], count, strings, lengths);
        free((void *)strings); free(lengths);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glActiveTexture")) {
        glActiveTexture(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glClientActiveTexture")) {
        glClientActiveTexture(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glEnableClientState")) {
        glEnableClientState(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glDisableClientState")) {
        glDisableClientState(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glVertexPointer")) {
        glVertexPointer((GLint)arguments[0], arguments[1], (GLsizei)arguments[2],
                        (const void *)(uintptr_t)arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glColorPointer")) {
        glColorPointer((GLint)arguments[0], arguments[1], (GLsizei)arguments[2],
                       (const void *)(uintptr_t)arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glTexCoordPointer")) {
        glTexCoordPointer((GLint)arguments[0], arguments[1], (GLsizei)arguments[2],
                          (const void *)(uintptr_t)arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glNormalPointer")) {
        glNormalPointer(arguments[0], (GLsizei)arguments[1],
                        (const void *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glLoadMatrixf")) {
        glLoadMatrixf((const GLfloat *)(uintptr_t)arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glDepthRange")) {
        glDepthRange(*(const double *)(const void *)&arguments[0],
                     *(const double *)(const void *)&arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glDrawElements")) {
        glDrawElements(arguments[0], (GLsizei)arguments[1], arguments[2],
                       (const void *)(uintptr_t)arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glDrawBuffers")) {
        glDrawBuffers((GLsizei)arguments[0], (const GLenum *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glGetTexImage")) {
        glGetTexImage(arguments[0], (GLint)arguments[1], arguments[2], arguments[3],
                      (void *)(uintptr_t)arguments[4]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glTexSubImage2D")) {
        glTexSubImage2D(arguments[0], (GLint)arguments[1], (GLint)arguments[2],
                        (GLint)arguments[3], (GLsizei)arguments[4], (GLsizei)arguments[5],
                        arguments[6], arguments[7], (const void *)(uintptr_t)arguments[8]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glCheckFramebufferStatus") ||
        LP32_NAME_IS(import_name, import_length, "glCheckFramebufferStatusEXT")) {
        *result = glCheckFramebufferStatus(arguments[0]);
        if (trace_gl_render_call()) {
            fprintf(stderr,
                    "compat32: glCheckFramebufferStatus swap=%llu "
                    "target=%04x status=%04llx\n",
                    (unsigned long long)objc_bridge_swap_count, arguments[0],
                    (unsigned long long)*result);
        }
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glFramebufferTexture2D") ||
        LP32_NAME_IS(import_name, import_length, "glFramebufferTexture2DEXT")) {
        ++gl_frame_diagnostics.framebuffer_attachments;
        if (trace_gl_render_call()) {
            fprintf(stderr,
                    "compat32: glFramebufferTexture2D swap=%llu target=%04x "
                    "attachment=%04x textarget=%04x texture=%u level=%d\n",
                    (unsigned long long)objc_bridge_swap_count, arguments[0],
                    arguments[1], arguments[2], arguments[3],
                    (GLint)arguments[4]);
        }
        glFramebufferTexture2D(arguments[0], arguments[1], arguments[2],
                               arguments[3], (GLint)arguments[4]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBlitFramebuffer") ||
        LP32_NAME_IS(import_name, import_length, "glBlitFramebufferEXT")) {
        ++gl_frame_diagnostics.framebuffer_blits;
        if (trace_gl_render_call()) {
            fprintf(stderr,
                    "compat32: glBlitFramebuffer swap=%llu "
                    "src={%d,%d,%d,%d} dst={%d,%d,%d,%d} "
                    "mask=%04x filter=%04x\n",
                    (unsigned long long)objc_bridge_swap_count,
                    (GLint)arguments[0], (GLint)arguments[1],
                    (GLint)arguments[2], (GLint)arguments[3],
                    (GLint)arguments[4], (GLint)arguments[5],
                    (GLint)arguments[6], (GLint)arguments[7],
                    arguments[8], arguments[9]);
        }
        glBlitFramebuffer((GLint)arguments[0], (GLint)arguments[1],
                          (GLint)arguments[2], (GLint)arguments[3],
                          (GLint)arguments[4], (GLint)arguments[5],
                          (GLint)arguments[6], (GLint)arguments[7],
                          arguments[8], arguments[9]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGenRenderbuffers") ||
        LP32_NAME_IS(import_name, import_length, "glGenRenderbuffersEXT")) {
        glGenRenderbuffers(arguments[0], (GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDeleteRenderbuffers") ||
        LP32_NAME_IS(import_name, import_length, "glDeleteRenderbuffersEXT")) {
        glDeleteRenderbuffers(arguments[0],
                              (const GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBindRenderbuffer") ||
        LP32_NAME_IS(import_name, import_length, "glBindRenderbufferEXT")) {
        glBindRenderbuffer(arguments[0], arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glRenderbufferStorage") ||
        LP32_NAME_IS(import_name, import_length, "glRenderbufferStorageEXT")) {
        glRenderbufferStorage(arguments[0], arguments[1], arguments[2],
                              arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glFramebufferRenderbuffer") ||
        LP32_NAME_IS(import_name, import_length, "glFramebufferRenderbufferEXT")) {
        ++gl_frame_diagnostics.framebuffer_attachments;
        glFramebufferRenderbuffer(arguments[0], arguments[1], arguments[2],
                                  arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGenerateMipmap") ||
        LP32_NAME_IS(import_name, import_length, "glGenerateMipmapEXT")) {
        note_texture_level_change();
        glGenerateMipmap(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDrawBuffers") ||
        LP32_NAME_IS(import_name, import_length, "glDrawBuffersARB")) {
        const GLenum *buffers = (const void *)(uintptr_t)arguments[1];
        if (trace_gl_render_call()) {
            GLsizei count = (GLsizei)arguments[0];
            fprintf(stderr,
                    "compat32: glDrawBuffers swap=%llu count=%d "
                    "buffers={%04x,%04x,%04x,%04x}\n",
                    (unsigned long long)objc_bridge_swap_count, count,
                    count > 0 ? buffers[0] : 0, count > 1 ? buffers[1] : 0,
                    count > 2 ? buffers[2] : 0, count > 3 ? buffers[3] : 0);
        }
        glDrawBuffers(arguments[0], buffers);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGenProgramsARB")) {
        glGenProgramsARB(arguments[0], (GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDeleteProgramsARB")) {
        forget_fragment_program_samplers(
            (GLsizei)arguments[0],
            (const GLuint *)(uintptr_t)arguments[1]);
        glDeleteProgramsARB(arguments[0],
                            (const GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBindProgramARB")) {
        *result = fast_glBindProgramARB(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glProgramStringARB")) {
        GLsizei source_size = (GLsizei)arguments[2];
        const void *source = (const void *)(uintptr_t)arguments[3];
        maybe_dump_gl_program(arguments[0], source_size, source);
        char *plain_targets = NULL;
        if (arguments[0] == GL_FRAGMENT_PROGRAM_ARB && source_size > 0 &&
            arguments[1] == GL_PROGRAM_FORMAT_ASCII_ARB &&
            !getenv("LP32_KEEP_ARB_SHADOW_TARGETS")) {
            size_t plain_size = 0;
            size_t rewrites = 0;
            plain_targets = arb_program_plain_shadow_targets(
                source, (size_t)source_size, &plain_size, &rewrites);
            if (plain_targets && plain_size <= INT_MAX) {
                static unsigned int reports;
                if (reports < 12) {
                    gl_trace_printf(
                        "compat32: rewrote %zu SHADOW texture target(s) to "
                        "plain targets in fragment program %u\n",
                        rewrites, trace_fragment_program);
                    ++reports;
                }
                source = plain_targets;
                source_size = (GLsizei)plain_size;
            } else {
                free(plain_targets);
                plain_targets = NULL;
            }
        }
        if (arguments[0] == GL_FRAGMENT_PROGRAM_ARB && source_size > 0) {
            GLint program = (GLint)trace_fragment_program;
            if (program > 0) {
                remember_fragment_program_samplers(
                    (GLuint)program, source, (size_t)source_size);
            }
        }
        size_t guarded_size = 0;
        size_t rsq_guard_count = 0;
        size_t rcp_guard_count = 0;
        char *guarded = NULL;
        if (guard_undefined_arb_math() && source_size > 0 &&
            arguments[1] == GL_PROGRAM_FORMAT_ASCII_ARB) {
            guarded = arb_program_guard_undefined_math(
                source, (size_t)source_size, &guarded_size,
                &rsq_guard_count, &rcp_guard_count);
        }
        if (guarded && guarded_size <= INT_MAX) {
            static unsigned int reports;
            if (reports < 12) {
                gl_trace_printf(
                    "compat32: guarded ARB math rsq=%zu rcp=%zu "
                    "target=%04x originalBytes=%d guardedBytes=%zu\n",
                    rsq_guard_count, rcp_guard_count,
                    arguments[0], source_size, guarded_size);
                ++reports;
            }
            glProgramStringARB(arguments[0], arguments[1],
                               (GLsizei)guarded_size, guarded);
        } else {
            glProgramStringARB(arguments[0], arguments[1], source_size,
                               source);
        }
        free(guarded);
        report_arb_program_load_failure(arguments[0], source, source_size);
        free(plain_targets);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glProgramLocalParameter4fvARB")) {
        *result = fast_glProgramLocalParameter4fvARB(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glProgramLocalParameters4fvEXT")) {
        *result = fast_glProgramLocalParameters4fvEXT(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glProgramEnvParameter4fvARB")) {
        *result = fast_glProgramEnvParameter4fvARB(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glProgramEnvParameters4fvEXT")) {
        *result = fast_glProgramEnvParameters4fvEXT(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGetProgramivARB")) {
        glGetProgramivARB(arguments[0], arguments[1],
                          (GLint *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGetProgramStringARB")) {
        glGetProgramStringARB(arguments[0], arguments[1],
                              (void *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glIsProgramARB")) {
        *result = glIsProgramARB(arguments[0]); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glGetString")) {
        *result = guest_gl_string(arguments[0]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glEnable")) {
        *result = fast_glEnable(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glDisable")) {
        *result = fast_glDisable(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glClear")) {
        ++gl_frame_diagnostics.clear_calls;
        if (trace_gl_render_call()) {
            GLint framebuffer = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
            fprintf(stderr,
                    "compat32: glClear swap=%llu mask=%04x fbo=%d\n",
                    (unsigned long long)objc_bridge_swap_count,
                    arguments[0], framebuffer);
        }
        glClear(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBlendEquation") ||
        LP32_NAME_IS(import_name, import_length, "glBlendEquationEXT")) {
        *result = fast_glBlendEquation(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBlendFuncSeparate") ||
        LP32_NAME_IS(import_name, import_length, "glBlendFuncSeparateEXT")) {
        *result = fast_glBlendFuncSeparate(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBlendEquationSeparate") ||
        LP32_NAME_IS(import_name, import_length, "glBlendEquationSeparateEXT")) {
        glBlendEquationSeparate(arguments[0], arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBlendColor") ||
        LP32_NAME_IS(import_name, import_length, "glBlendColorEXT")) {
        glBlendColor(guest_float_argument(arguments[0]),
                     guest_float_argument(arguments[1]),
                     guest_float_argument(arguments[2]),
                     guest_float_argument(arguments[3]));
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glStencilFuncSeparate")) {
        glStencilFuncSeparate(arguments[0], arguments[1], arguments[2],
                              arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glStencilMaskSeparate")) {
        glStencilMaskSeparate(arguments[0], arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glStencilOpSeparate") ||
        LP32_NAME_IS(import_name, import_length, "glStencilOpSeparateATI")) {
        glStencilOpSeparate(arguments[0], arguments[1], arguments[2],
                            arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glClearColor")) {
        const float *v = (const void *)arguments;
        glClearColor(v[0], v[1], v[2], v[3]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glClearDepth")) {
        double value; memcpy(&value, arguments, sizeof(value));
        glClearDepth(value); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glClearStencil")) { glClearStencil(arguments[0]); *result = 0; return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glAlphaFunc")) {
        *result = fast_glAlphaFunc(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glBindTexture")) {
        *result = fast_glBindTexture(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glBlendFunc")) { *result = fast_glBlendFunc(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glColorMask")) { *result = fast_glColorMask(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glCullFace")) { *result = fast_glCullFace(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glDeleteTextures")) {
        if (trace_gl_render_call()) {
            const GLuint *textures = (const void *)(uintptr_t)arguments[1];
            gl_trace_printf(
                "compat32: glDeleteTextures swap=%llu count=%u first=%u\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                arguments[0] && textures ? textures[0] : 0);
        }
        forget_mipmap_repairs(
            (GLsizei)arguments[0],
            (const GLuint *)(uintptr_t)arguments[1]);
        glDeleteTextures(arguments[0],
                         (const GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glDepthFunc")) { *result = fast_glDepthFunc(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glDepthMask")) { *result = fast_glDepthMask(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glDrawArrays")) {
        *result = fast_glDrawArrays(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glDrawBuffer")) {
        if (trace_gl_render_call()) {
            fprintf(stderr, "compat32: glDrawBuffer swap=%llu buffer=%04x\n",
                    (unsigned long long)objc_bridge_swap_count, arguments[0]);
        }
        glDrawBuffer(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glFlush")) { glFlush(); *result = 0; return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glFrontFace")) { *result = fast_glFrontFace(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glGenTextures")) {
        glGenTextures(arguments[0], (GLuint *)(uintptr_t)arguments[1]);
        if (trace_gl_render_call()) {
            const GLuint *textures = (const void *)(uintptr_t)arguments[1];
            gl_trace_printf(
                "compat32: glGenTextures swap=%llu count=%u first=%u\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                arguments[0] && textures ? textures[0] : 0);
        }
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glGetFloatv")) { *result = fast_glGetFloatv(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glGetIntegerv")) { *result = fast_glGetIntegerv(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glPolygonOffset")) {
        *result = fast_glPolygonOffset(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glReadBuffer")) { glReadBuffer(arguments[0]); *result = 0; return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glScissor")) { *result = fast_glScissor(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glStencilFunc")) { *result = fast_glStencilFunc(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glStencilMask")) { *result = fast_glStencilMask(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glStencilOp")) { *result = fast_glStencilOp(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glTexEnvf")) {
        *result = fast_glTexEnvf(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glTexImage2D")) {
        ++gl_frame_diagnostics.texture_uploads;
        note_texture_level_change();
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glTexImage2D swap=%llu target=%04x level=%d "
                "internal=%04x size=%dx%d border=%d format=%04x "
                "type=%04x pixels=%08x\n",
                (unsigned long long)objc_bridge_swap_count,
                arguments[0], (GLint)arguments[1], arguments[2],
                (GLsizei)arguments[3], (GLsizei)arguments[4],
                (GLint)arguments[5], arguments[6], arguments[7],
                arguments[8]);
        }
        glTexImage2D(arguments[0], arguments[1], arguments[2], arguments[3],
                     arguments[4], arguments[5], arguments[6], arguments[7],
                     (const void *)(uintptr_t)arguments[8]);
        repair_bound_texture_mipmap_range(arguments[0]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glTexParameterf")) {
        float value; memcpy(&value, arguments + 2, sizeof(value));
        glTexParameterf(arguments[0], arguments[1], value);
        if (arguments[1] == GL_TEXTURE_MIN_FILTER ||
            arguments[1] == GL_TEXTURE_BASE_LEVEL ||
            arguments[1] == GL_TEXTURE_MAX_LEVEL) {
            repair_bound_texture_mipmap_range(arguments[0]);
        }
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glTexParameterfv")) {
        glTexParameterfv(arguments[0], arguments[1],
                         (const GLfloat *)(uintptr_t)arguments[2]);
        if (arguments[1] == GL_TEXTURE_MIN_FILTER ||
            arguments[1] == GL_TEXTURE_BASE_LEVEL ||
            arguments[1] == GL_TEXTURE_MAX_LEVEL) {
            repair_bound_texture_mipmap_range(arguments[0]);
        }
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glTexParameteri")) {
        *result = fast_glTexParameteri(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glViewport")) {
        if (trace_gl_render_call()) {
            fprintf(stderr,
                    "compat32: glViewport swap=%llu value={%d,%d,%d,%d}\n",
                    (unsigned long long)objc_bridge_swap_count,
                    (GLint)arguments[0], (GLint)arguments[1],
                    (GLsizei)arguments[2], (GLsizei)arguments[3]);
        }
        glViewport(arguments[0], arguments[1], arguments[2], arguments[3]);
        *result = 0; return 1;
    }
    /* Fixed-function immediate mode used by the legacy display path for
       its loading/letterbox quads. */
    if (LP32_NAME_IS(import_name, import_length, "_glBegin")) {
        *result = fast_glBegin(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glEnd")) {
        *result = fast_glEnd(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glColor4f")) {
        glColor4f(guest_float_argument(arguments[0]),
                  guest_float_argument(arguments[1]),
                  guest_float_argument(arguments[2]),
                  guest_float_argument(arguments[3]));
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glVertex3f")) {
        glVertex3f(guest_float_argument(arguments[0]),
                   guest_float_argument(arguments[1]),
                   guest_float_argument(arguments[2]));
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glTexCoord2fv")) {
        glTexCoord2fv((const GLfloat *)(uintptr_t)arguments[0]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glLoadIdentity")) {
        glLoadIdentity();
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glMatrixMode")) {
        glMatrixMode(arguments[0]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glCopyTexSubImage2D")) {
        glCopyTexSubImage2D(arguments[0], (GLint)arguments[1],
                            (GLint)arguments[2], (GLint)arguments[3],
                            (GLint)arguments[4], (GLint)arguments[5],
                            (GLsizei)arguments[6], (GLsizei)arguments[7]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glFinish")) {
        glFinish();
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_IORegistryEntrySearchCFProperty")) {
        NSString *key = object_for_argument(arguments[2]);
        uint32_t identifier = 0;
        if ([key isEqualToString:@"vendor-id"]) identifier = UINT32_C(0x106b);
        else if ([key isEqualToString:@"device-id"]) identifier = 1;
        NSData *data = [NSData dataWithBytes:&identifier length:sizeof(identifier)];
        *result = proxy_for_object(data);
        return 1;
    }

    if (LP32_NAME_IS(import_name, import_length, "_CFBundleGetMainBundle")) {
        *result = proxy_for_object(legacy_game_bundle());
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCopyExecutableURL")) {
        NSBundle *bundle = object_for_argument(arguments[0]);
        *result = proxy_for_object([bundle executableURL]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCopyResourcesDirectoryURL")) {
        NSBundle *bundle = object_for_argument(arguments[0]);
        *result = proxy_for_object([bundle resourceURL]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCreate")) {
        NSURL *url = object_for_argument(arguments[1]);
        NSBundle *bundle = url ? [NSBundle bundleWithURL:url] : nil;
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: CFBundleCreate %s -> %s\n",
                    [[url path] UTF8String], bundle ? "ok" : "nil");
        }
        *result = proxy_for_object(bundle);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleGetFunctionPointerForName")) {
        /* Used to look up OpenGL entry points through the framework bundle.
           Hand back a guest thunk that dispatches through the import chains,
           exactly like dlsym does. */
        NSString *name = object_for_argument(arguments[1]);
        const char *symbol = [name UTF8String];
        uint32_t thunk = 0;
        if (symbol && strcmp(symbol, "MainEntry") == 0) {
        } else if (symbol && dlsym(RTLD_DEFAULT, symbol)) {
            thunk = compat_runtime32_guest_callback(symbol);
        }
        if (!thunk && getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: CFBundleGetFunctionPointerForName %s -> none\n",
                    symbol ? symbol : "(null)");
        }
        *result = thunk;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCopyResourceURL")) {
        NSBundle *bundle = object_for_argument(arguments[0]);
        NSString *name = object_for_argument(arguments[1]);
        NSString *extension = object_for_argument(arguments[2]);
        NSString *subdirectory = object_for_argument(arguments[3]);
        *result = proxy_for_object([bundle URLForResource:name
                                          withExtension:extension
                                           subdirectory:subdirectory]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLGetFileSystemRepresentation")) {
        NSURL *url = object_for_argument(arguments[0]);
        char *buffer = (void *)(uintptr_t)arguments[2];
        size_t capacity = arguments[3];
        const char *path = [[url path] fileSystemRepresentation];
        if (!buffer || !path || strlen(path) + 1 > capacity) {
            *result = 0;
        } else {
            memcpy(buffer, path, strlen(path) + 1);
            *result = 1;
        }
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFRelease")) {
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFRetain")) {
        *result = arguments[0];
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFGetTypeID")) {
        id object = object_for_argument(arguments[0]);
        *result = object ? CFGetTypeID((CFTypeRef)object) : 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetLength")) {
        NSString *string = object_for_argument(arguments[0]);
        *result = (uint32_t)[string length];
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetSystemEncoding")) {
        *result = CFStringGetSystemEncoding();
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetCString")) {
        NSString *string = object_for_argument(arguments[0]);
        char *buffer = (void *)(uintptr_t)arguments[1];
        CFIndex capacity = (int32_t)arguments[2];
        *result = CFStringGetCString((CFStringRef)string, buffer, capacity,
                                     arguments[3]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCompare")) {
        NSString *left = object_for_argument(arguments[0]);
        NSString *right = object_for_argument(arguments[1]);
        *result = (uint32_t)CFStringCompare((CFStringRef)left,
                                            (CFStringRef)right, arguments[2]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringFind")) {
        NSString *string = object_for_argument(arguments[0]);
        NSString *needle = object_for_argument(arguments[1]);
        CFRange range = string && needle ?
            CFStringFind((CFStringRef)string, (CFStringRef)needle,
                         arguments[2]) : CFRangeMake(kCFNotFound, 0);
        *result = (uint32_t)(int32_t)range.location |
                  ((uint64_t)(uint32_t)range.length << 32);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCreateWithBytes")) {
        const UInt8 *bytes = (const void *)(uintptr_t)arguments[1];
        CFIndex length = (int32_t)arguments[2];
        CFStringRef string = CFStringCreateWithBytes(NULL, bytes, length,
                                                     arguments[3], false);
        *result = proxy_for_object([(NSString *)string autorelease]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFArrayGetCount")) {
        NSArray *array = object_for_argument(arguments[0]);
        *result = (uint32_t)[array count];
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFArrayGetTypeID")) {
        *result = CFArrayGetTypeID();
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFArrayGetValueAtIndex")) {
        NSArray *array = object_for_argument(arguments[0]);
        NSUInteger index = arguments[1];
        *result = index < [array count] ? proxy_for_object([array objectAtIndex:index]) : 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDictionaryGetValue")) {
        NSDictionary *dictionary = object_for_argument(arguments[0]);
        id key = object_for_argument(arguments[1]);
        id value = [dictionary objectForKey:key];
        if (getenv("LP32_TRACE_DISPLAY") &&
            [dictionary objectForKey:@"Width"]) {
            fprintf(stderr, "compat32: display dictionary get %s -> %s\n",
                    [[key description] UTF8String],
                    [[value description] UTF8String]);
        }
        *result = proxy_for_object(value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDictionaryGetTypeID")) {
        *result = CFDictionaryGetTypeID();
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDictionaryGetValueIfPresent")) {
        NSDictionary *dictionary = object_for_argument(arguments[0]);
        id key = object_for_argument(arguments[1]);
        id value = [dictionary objectForKey:key];
        uint32_t *output = (void *)(uintptr_t)arguments[2];
        if (output) *output = proxy_for_object(value);
        *result = value != nil;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDictionarySetValue")) {
        NSMutableDictionary *dictionary = object_for_argument(arguments[0]);
        id key = object_for_argument(arguments[1]);
        id value = object_for_argument(arguments[2]);
        if (key && value) [dictionary setObject:value forKey:key];
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDataGetTypeID")) {
        *result = CFDataGetTypeID();
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDataGetBytePtr")) {
        NSData *data = object_for_argument(arguments[0]);
        if (!data) {
            *result = 0;
            return 1;
        }
        NSUInteger length = [data length];
        uint32_t copy = compat_runtime32_allocate(length ? length : 1, 0);
        if (copy && length) memcpy((void *)(uintptr_t)copy, [data bytes], length);
        *result = copy;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBooleanGetValue")) {
        NSNumber *number = object_for_argument(arguments[0]);
        *result = [number boolValue];
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFLocaleCopyCurrent")) {
        *result = proxy_for_object([NSLocale currentLocale]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFLocaleCopyPreferredLanguages")) {
        *result = proxy_for_object([NSLocale preferredLanguages]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFLocaleCreate")) {
        NSString *identifier = object_for_argument(arguments[1]);
        NSLocale *locale = [[[NSLocale alloc] initWithLocaleIdentifier:identifier]
                            autorelease];
        *result = proxy_for_object(locale);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFLocaleGetIdentifier")) {
        NSLocale *locale = object_for_argument(arguments[0]);
        *result = proxy_for_object([locale localeIdentifier]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFLocaleGetValue")) {
        NSLocale *locale = object_for_argument(arguments[0]);
        NSString *key = object_for_argument(arguments[1]);
        *result = proxy_for_object([locale objectForKey:key]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFNumberCreate")) {
        CFNumberType requested_type = (CFNumberType)arguments[1];
        const void *guest_value = (const void *)(uintptr_t)arguments[2];
        CFNumberRef number = NULL;

        /* CFNumber's platform-sized types describe the caller's ABI.  The
           guest is i386, so long/CFIndex/NSInteger are four bytes and
           CGFloat is a float.  Passing those type tags straight to the
           64-bit host CoreFoundation makes it read eight bytes from the
           guest stack. */
        if (requested_type == kCFNumberLongType ||
            requested_type == kCFNumberCFIndexType ||
            requested_type == kCFNumberNSIntegerType) {
            int32_t value = 0;
            if (guest_value) memcpy(&value, guest_value, sizeof(value));
            number = CFNumberCreate(NULL, kCFNumberSInt32Type, &value);
        } else if (requested_type == kCFNumberCGFloatType) {
            Float32 value = 0.0f;
            if (guest_value) memcpy(&value, guest_value, sizeof(value));
            number = CFNumberCreate(NULL, kCFNumberFloat32Type, &value);
        } else {
            number = CFNumberCreate(NULL, requested_type, guest_value);
        }
        *result = proxy_for_object([(NSNumber *)number autorelease]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFNumberGetValue")) {
        NSNumber *number = object_for_argument(arguments[0]);
        CFNumberType requested_type = (CFNumberType)arguments[1];
        void *guest_output = (void *)(uintptr_t)arguments[2];
        Boolean converted = false;

        if (requested_type == kCFNumberLongType ||
            requested_type == kCFNumberCFIndexType ||
            requested_type == kCFNumberNSIntegerType) {
            int32_t value = 0;
            converted = CFNumberGetValue((CFNumberRef)number,
                                         kCFNumberSInt32Type, &value);
            if (guest_output) memcpy(guest_output, &value, sizeof(value));
        } else if (requested_type == kCFNumberCGFloatType) {
            Float32 value = 0.0f;
            converted = CFNumberGetValue((CFNumberRef)number,
                                         kCFNumberFloat32Type, &value);
            if (guest_output) memcpy(guest_output, &value, sizeof(value));
        } else {
            converted = CFNumberGetValue((CFNumberRef)number, requested_type,
                                         guest_output);
        }
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr,
                    "compat32: CFNumberGetValue value=%s type=%u "
                    "output=0x%08x converted=%u\n",
                    [[number description] UTF8String], arguments[1],
                    arguments[2], converted);
        }
        *result = converted;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFAbsoluteTimeGetCurrent")) {
        double value = CFAbsoluteTimeGetCurrent();
        memcpy(result, &value, sizeof(value));
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFRunLoopGetCurrent")) {
        /* A host-thread token is sufficient because Add/Remove operate on
           the current run loop for this title's IOKit notification path. */
        *result = proxy_for_object([NSThread currentThread]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFShow")) {
        id object = object_for_argument(arguments[0]);
        fprintf(stderr, "compat32: CFShow %s\n", [[object description] UTF8String]);
        *result = 0;
        return 1;
    }

    if (LP32_NAME_IS(import_name, import_length, "_TISCopyCurrentKeyboardInputSource")) {
        TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
        *result = proxy_for_object((id)source);
        if (source) CFRelease(source);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_TISGetInputSourceProperty")) {
        TISInputSourceRef source =
            (TISInputSourceRef)object_for_argument(arguments[0]);
        CFStringRef key = (CFStringRef)object_for_argument(arguments[1]);
        CFTypeRef value = source && key ?
            TISGetInputSourceProperty(source, key) : NULL;
        *result = proxy_for_object((id)value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_LMGetKbdType")) {
        *result = LMGetKbdType();
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_UCKeyTranslate")) {
        const UCKeyboardLayout *layout =
            (const void *)(uintptr_t)arguments[0];
        *result = (uint32_t)UCKeyTranslate(
            layout, (UInt16)arguments[1], (UInt16)arguments[2], arguments[3],
            arguments[4], arguments[5],
            (UInt32 *)(uintptr_t)arguments[6], arguments[7],
            (UniCharCount *)(uintptr_t)arguments[8],
            (UniChar *)(uintptr_t)arguments[9]);
        return 1;
    }

    if (LP32_NAME_IS(import_name, import_length, "_NSRunAlertPanel")) {
        NSString *title = object_for_argument(arguments[0]);
        NSString *message = object_for_argument(arguments[1]);
        fprintf(stderr, "compat32: legacy alert: %s — %s\n",
                [[title description] UTF8String], [[message description] UTF8String]);
        /* The original API is a blocking convenience wrapper.  Logging and
           accepting its sole default button preserves control flow without
           stalling an automated launch behind a modal compatibility alert. */
        *result = NSAlertFirstButtonReturn;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSLog")) {
        NSString *format = object_for_argument(arguments[0]);
        fprintf(stderr, "compat32: NSLog: %s\n", [[format description] UTF8String]);
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSSearchPathForDirectoriesInDomains")) {
        NSArray *paths = NSSearchPathForDirectoriesInDomains(arguments[0],
                                                              arguments[1],
                                                              arguments[2] != 0);
        /* Test sessions redirect the save/config tree so an automated run can
           never touch the player's real saved games. */
        const char *override = getenv("LP32_APPLICATION_SUPPORT_DIR");
        if (override && override[0] &&
            arguments[0] == NSApplicationSupportDirectory &&
            (arguments[1] & NSUserDomainMask) != 0) {
            paths = @[[NSString stringWithUTF8String:override]];
        }
        *result = proxy_for_object(paths);
        return 1;
    }

    if (LP32_NAME_IS(import_name, import_length, "_NSApplicationMain")) {
        int argc = (int)arguments[0];
        const uint32_t *guest_argv = (const void *)(uintptr_t)arguments[1];
        if (argc < 0 || argc > 64 || !guest_argv) return 0;
        const char *host_argv[65] = {0};
        for (int index = 0; index < argc; ++index) {
            host_argv[index] = (const char *)(uintptr_t)guest_argv[index];
        }
        [[NSUserDefaults standardUserDefaults] setBool:YES
                                                forKey:@"ApplePersistenceIgnoreState"];
        *result = (uint32_t)NSApplicationMain(argc, host_argv);
        return 1;
    }

    if (LP32_NAME_IS(import_name, import_length, "_objc_msgSendSuper")) {
        /* struct objc_super { id receiver; Class super_class; } in guest mem.
           Route the message to the host superclass by re-dispatching to the
           receiver with guest overrides suppressed (g_force_super). */
        const uint32_t *super = (const void *)(uintptr_t)arguments[0];
        if (!super) { *result = 0; return 1; }
        /* A window/view guest instance running its designated initializer:
           create the real AppKit companion now (see guest_objc_super_init). */
        {
            const char *sel = (const char *)(uintptr_t)arguments[1];
            if (sel && guest_objc_super_init(super[0], sel, arguments + 2, result))
                return 1;
        }
        uint32_t newargs[16];
        newargs[0] = super[0];
        for (unsigned i = 1; i < 16; ++i) newargs[i] = arguments[i];
        int prev = g_force_super;
        g_force_super = 1;
        int handled = objc_bridge32_dispatch("_objc_msgSend", newargs, result);
        g_force_super = prev;
        return handled;
    }

    if (LP32_NAME_IS(import_name, import_length, "_objc_msgSend")) {
        /* Guest-defined classes (Bejeweled 3's SexyApplicationDelegate/Window/
           OpenGLView) are synthesized on demand; handle their class and
           instance messages before the general host path. */
        if (guest_objc_msgsend(arguments, 16, result)) return 1;
        const char *selector_name = (const char *)(uintptr_t)arguments[1];
        /* NSAutoreleasePool: this bridge already drains host temporaries at the
           end of every dispatch, so the guest's own pools are redundant.  A
           real pool would be created inside the bridge's transient
           @autoreleasepool, get drained when that pops, then be double-freed by
           the guest's own -drain ("invalid autorelease pool" abort).  Make the
           guest pool a no-op sentinel instead. */
        enum { kAutoreleasePoolSentinel = 0xFACE9001u };
        if (arguments[0] == kAutoreleasePoolSentinel) {
            *result = (selector_name && (strcmp(selector_name, "init") == 0 ||
                                         strcmp(selector_name, "retain") == 0))
                ? kAutoreleasePoolSentinel : 0;
            return 1;
        }
        if (selector_name &&
            (strcmp(selector_name, "alloc") == 0 ||
             strcmp(selector_name, "allocWithZone:") == 0 ||
             strcmp(selector_name, "new") == 0) &&
            object_for_receiver(arguments[0]) == (id)objc_getClass("NSAutoreleasePool")) {
            *result = kAutoreleasePoolSentinel;
            return 1;
        }
        id receiver = object_for_receiver(arguments[0]);
        static int trace_selectors = -1;
        if (trace_selectors < 0) {
            trace_selectors = getenv("LP32_TRACE_OBJC_SELECTORS") != NULL;
        }
        if (trace_selectors && selector_name) {
            fprintf(stderr, "compat32: objc swap=%llu %s receiver=%s\n",
                    (unsigned long long)objc_bridge_swap_count, selector_name,
                    receiver ? class_getName(object_getClass(receiver)) : "(none)");
        }
        /* Bejeweled 3 presents by messaging its NSOpenGLContext directly
           rather than through the bridge's OpenGLView.  Route those messages
           through the shared present path (frame statistics, the audio hold
           clock, optional pacing) and apply the swap-interval policy the CGL
           path already applies for Peggle. */
        if (selector_name && receiver &&
            [receiver isKindOfClass:[NSOpenGLContext class]]) {
            NSOpenGLContext *gl_context = (NSOpenGLContext *)receiver;
            if (strcmp(selector_name, "flushBuffer") == 0) {
                /* Same policy as the bridge view: the game's swap-interval
                   request was replaced by the bridge pacer (unless
                   LP32_HONOR_SWAP_INTERVAL), which frame_pacer_wait honours.
                   Bejeweled 3 then presents at 60 fps while its 100 Hz
                   simulation runs several updates per draw, at half the CPU
                   of an unpaced 100 fps loop. */
                ++objc_bridge_swap_count;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                NSWindow *gl_window = [[gl_context view] window];
#pragma clang diagnostic pop
                present_frame_with_stats(gl_context, gl_window, true);
                *result = 0;
                return 1;
            }
            if (strcmp(selector_name, "setValues:forParameter:") == 0) {
                const GLint *values = (const GLint *)(uintptr_t)arguments[2];
                NSOpenGLContextParameter parameter =
                    (NSOpenGLContextParameter)(int32_t)arguments[3];
                GLint override_value = 0;
                if (parameter == NSOpenGLContextParameterSwapInterval && values &&
                    frame_pacer_takes_over_swap_interval(*values)) {
                    values = &override_value;
                }
                if (getenv("LP32_TRACE_DISPLAY")) {
                    fprintf(stderr,
                            "compat32: NSOpenGLContext setValues parameter=%d "
                            "value=%d\n", (int)parameter, values ? *values : -1);
                }
                if (values) [gl_context setValues:values forParameter:parameter];
                *result = 0;
                return 1;
            }
        }
        /* Unattended test runs must never take focus away from the user: the
           game activates itself and makes its window key at startup, so answer
           those without doing them (the window is still ordered in). */
        if (background_test_mode() && selector_name) {
            if (strcmp(selector_name, "activateIgnoringOtherApps:") == 0 ||
                strcmp(selector_name, "makeKeyWindow") == 0 ||
                strcmp(selector_name, "makeMainWindow") == 0) {
                *result = 0;
                return 1;
            }
            if (strcmp(selector_name, "makeKeyAndOrderFront:") == 0 &&
                [receiver isKindOfClass:[NSWindow class]]) {
                [(NSWindow *)receiver orderFront:nil];
                *result = 0;
                return 1;
            }
        }
        if (getenv("LP32_TRACE_INPUT") && selector_name &&
            (strstr(selector_name, "Event") || strstr(selector_name, "event") ||
             strstr(selector_name, "key") || strstr(selector_name, "Key") ||
             strstr(selector_name, "character") ||
             strstr(selector_name, "modifier") ||
             strstr(selector_name, "button") || strstr(selector_name, "Button"))) {
            fprintf(stderr, "compat32: input ObjC %s receiver=%s\n",
                    selector_name,
                    receiver ? class_getName(object_getClass(receiver)) : "(none)");
        }
        if (!arguments[0]) { *result = 0; return 1; }
        if (!receiver || !selector_name) {
            fprintf(stderr,
                    "compat32: Objective-C lookup failed receiver=0x%08x selector=%s\n",
                    arguments[0], selector_name ? selector_name : "(null)");
            return 0;
        }
        if (strcmp(selector_name, "CGLContextObj") == 0) {
            CGLContextObj context = [(NSOpenGLContext *)receiver CGLContextObj];
            *result = context ? proxy_for_object([NSValue valueWithPointer:context]) : 0;
            return 1;
        }
        if (strcmp(selector_name, "terminate:") == 0 &&
            [receiver isKindOfClass:[NSApplication class]]) {
            /* See the exit import handling in compat_runtime.c.  AppKit's
               normal termination teardown can wait forever on a quarantined
               mixed-ABI AUGraph, so the confirmed game quit ends here. */
            fprintf(stderr, "compat32: guest requested AppKit termination\n");
            compat_runtime32_heap_report("AppKit-terminate");
            fflush(NULL);
            _Exit(EXIT_SUCCESS);
        }
        if (strcmp(selector_name,
                   "initWithContentRect:styleMask:backing:defer:") == 0) {
            const float *rect = (const void *)(arguments + 2);
            NSRect host_rect = NSMakeRect(rect[0], rect[1], rect[2], rect[3]);
            if ([receiver isKindOfClass:[NSFullScreenWindow class]]) {
                host_rect = [preferred_game_screen() frame];
                fprintf(stderr,
                        "compat32: placing fullscreen window at "
                        "%.0f,%.0f %.0fx%.0f\n",
                        host_rect.origin.x, host_rect.origin.y,
                        host_rect.size.width, host_rect.size.height);
            }
            id value = [(NSWindow *)receiver initWithContentRect:host_rect
                                                       styleMask:arguments[6]
                                                         backing:arguments[7]
                                                           defer:arguments[8] != 0];
            *result = proxy_for_object(value);
            return 1;
        }
        if (strcmp(selector_name, "setContentView:") == 0 &&
            [receiver isKindOfClass:[NSFullScreenWindow class]] &&
            [object_for_argument(arguments[2]) isKindOfClass:[OpenGLView class]]) {
            /* Host the GL view inside a plain container so the emulated
               display mode can letterbox it; the window's black background
               fills the bars.  The game keeps talking to the GL view. */
            OpenGLView *view = object_for_argument(arguments[2]);
            NSRect bounds = [[(NSWindow *)receiver contentView] bounds];
            NSView *container = [[[NSView alloc] initWithFrame:bounds] autorelease];
            [container setAutoresizesSubviews:NO];
            [(NSWindow *)receiver setContentView:container];
            [container addSubview:view];
            [view setFrame:[view letterboxedFrame]];
            *result = 0;
            return 1;
        }
        if ((strcmp(selector_name, "setFrame:display:") == 0 ||
             strcmp(selector_name, "setFrame:display:animate:") == 0) &&
            [receiver isKindOfClass:[NSFullScreenWindow class]]) {
            /* The legacy fullscreen path re-anchors its window at
               the main display's origin after creating it; keep it on the
               display the bridge chose. */
            NSRect host_rect = [preferred_game_screen() frame];
            [(NSWindow *)receiver setFrame:host_rect display:arguments[6] != 0];
            for (NSView *subview in [[(NSWindow *)receiver contentView] subviews]) {
                if ([subview isKindOfClass:[OpenGLView class]]) {
                    [(OpenGLView *)subview applyGuestDisplayMode];
                }
            }
            *result = 0;
            return 1;
        }
        if (strcmp(selector_name, "initWithFrame:") == 0 ||
            strcmp(selector_name, "initFullscreen:") == 0) {
            const float *rect = (const void *)(arguments + 2);
            NSRect host_rect = NSMakeRect(rect[0], rect[1], rect[2], rect[3]);
            id value = strcmp(selector_name, "initFullscreen:") == 0 ?
                [(OpenGLView *)receiver initFullscreen:host_rect] :
                [(NSView *)receiver initWithFrame:host_rect];
            *result = proxy_for_object(value);
            return 1;
        }
        if (strcmp(selector_name, "mainScreen") == 0 &&
            receiver == (id)[NSScreen class]) {
            *result = proxy_for_object(preferred_game_screen());
            return 1;
        }
        if (strcmp(selector_name, "initWithFrame:shareContext:") == 0) {
            const float *rect = (const void *)(arguments + 2);
            NSRect host_rect = NSMakeRect(rect[0], rect[1], rect[2], rect[3]);
            NSOpenGLContext *share = object_for_argument(arguments[6]);
            id value = [(OpenGLView *)receiver initWithFrame:host_rect
                                                shareContext:share];
            *result = proxy_for_object(value);
            return 1;
        }
        if (strcmp(selector_name,
                   "initWithFrame:shareContext:openGLDisplayMask:"
                   "sampleBuffers:samples:") == 0) {
            const float *rect = (const void *)(arguments + 2);
            NSRect host_rect = NSMakeRect(rect[0], rect[1], rect[2], rect[3]);
            NSOpenGLContext *share = object_for_argument(arguments[6]);
            id value = [(OpenGLView *)receiver initWithFrame:host_rect
                                                shareContext:share
                                           openGLDisplayMask:arguments[7]
                                               sampleBuffers:(int)arguments[8]
                                                     samples:(int)arguments[9]];
            *result = proxy_for_object(value);
            return 1;
        }
        if (strcmp(selector_name, "enterFullScreenMode:withOptions:") == 0) {
            /* Bejeweled 3 asks NSView to go fullscreen with options that
               capture the display and disable process switching, which locks
               the operator out (no Cmd-Tab) and hides the surface from screen
               capture.  Present fullscreen on the current Space instead:
               auto-hide the dock/menu bar but keep switching enabled and do
               not grab all screens. */
            NSScreen *screen = object_for_argument(arguments[2]);
            if (!screen) screen = preferred_game_screen();
            NSDictionary *options = @{
                NSFullScreenModeApplicationPresentationOptions :
                    @(NSApplicationPresentationAutoHideDock |
                      NSApplicationPresentationAutoHideMenuBar),
                NSFullScreenModeAllScreens : @NO,
            };
            BOOL entered = [(NSView *)receiver enterFullScreenMode:screen
                                                       withOptions:options];
            *result = entered ? 1 : 0;
            return 1;
        }
        if (strcmp(selector_name,
                   "initFullscreen:openGLDisplayMask:sampleBuffers:samples:") == 0) {
            const float *rect = (const void *)(arguments + 2);
            NSRect host_rect = NSMakeRect(rect[0], rect[1], rect[2], rect[3]);
            id value = [(OpenGLView *)receiver initFullscreen:host_rect
                                            openGLDisplayMask:arguments[6]
                                                sampleBuffers:(int)arguments[7]
                                                      samples:(int)arguments[8]];
            *result = proxy_for_object(value);
            return 1;
        }
        if (strcmp(selector_name,
                   "nextEventMatchingMask:untilDate:inMode:dequeue:") == 0 &&
            [receiver isKindOfClass:[NSApplication class]]) {
            /*
             * The guest pumps its own event loop (mask 0xffffffff, default
             * mode) after finishLaunching and never enters -[NSApplication
             * run].  Apple events (Dock "Quit", a quit from the Finder or a
             * script) are received and then parked in the Carbon high-level
             * event queue, which only -run drains, so a Dock quit did nothing
             * and the only way out was Force Quit.
             */
            static bool logged_mask;
            if (!logged_mask) {
                logged_mask = true;
                id until = object_for_argument(arguments[3]);
                id mode = object_for_argument(arguments[4]);
                fprintf(stderr, "compat32: guest event pump mask=0x%08x "
                        "until=%s mode=%s dequeue=%u\n", arguments[2],
                        until ? [[until description] UTF8String] : "(nil)",
                        mode ? [[mode description] UTF8String] : "(nil)",
                        arguments[5]);
                /* The guest pumps events instead of running -[NSApplication
                   run], so nothing activates the app.  Games (Bejeweled 3)
                   create their window from -applicationDidBecomeActive:, which
                   AppKit only sends once the app is active.  Activate now so
                   the pump delivers NSApplicationDidBecomeActive and the guest
                   delegate proceeds.  (For titles whose view drives activation
                   this is a harmless second activate.) */
                activate_game_application();
            }
            {
                /* Apple events wait in the Carbon high-level event queue
                   (the log says "Enqueuing event to local high-level event
                   queue") and nothing in this process drains it: AppKit does
                   that from -[NSApplication run], which the guest never
                   enters.  Drain it here, then the handlers run. */
                EventTypeSpec kinds = { kEventClassAppleEvent, kEventAppleEvent };
                EventRef carbon_event = NULL;
                while (ReceiveNextEvent(1, &kinds, kEventDurationNoWait, true,
                                        &carbon_event) == noErr) {
                    OSStatus status = AEProcessEvent(carbon_event);
                    fprintf(stderr, "compat32: processed a queued Apple event "
                            "(status %d)\n", (int)status);
                    ReleaseEvent(carbon_event);
                }
            }
        }
        bool invoked = invoke_simple_message(receiver, selector_name,
                                             arguments, result);
        if (!invoked) {
            fprintf(stderr,
                    "compat32: Objective-C bridge cannot invoke %s on %s\n",
                    selector_name, class_getName(object_getClass(receiver)));
        }
        return invoked;
    }

    if (LP32_NAME_IS(import_name, import_length, "_objc_msgSend_fpret")) {
        /* Float/double-returning message (e.g. -[NSScreen backingScaleFactor]).
           The guest reads the result from x87 st0, so return it through the
           fp-result path, not *result. */
        id receiver = object_for_receiver(arguments[0]);
        const char *selector_name = (const char *)(uintptr_t)arguments[1];
        if (!arguments[0] || !receiver || !selector_name) {
            *result = compat_runtime32_return_double(0.0);
            return 1;
        }
        SEL selector = sel_registerName(selector_name);
        if (![receiver respondsToSelector:selector]) {
            *result = compat_runtime32_return_double(0.0);
            return 1;
        }
        NSMethodSignature *signature = [receiver methodSignatureForSelector:selector];
        NSInvocation *invocation = [NSInvocation invocationWithMethodSignature:signature];
        [invocation setTarget:receiver];
        [invocation setSelector:selector];
        /* Marshal leading integer/pointer/object args (fpret getters are
           usually 0-arg; a few take an int). */
        for (NSUInteger i = 2; i < [signature numberOfArguments] && i < 6; ++i) {
            const char *t = skip_type_qualifiers([signature getArgumentTypeAtIndex:i]);
            if (*t == '@' || *t == '#') { id v = object_for_argument(arguments[i]); [invocation setArgument:&v atIndex:i]; }
            else if (*t == '^' || *t == '*' || *t == ':') { void *v = (void *)(uintptr_t)arguments[i]; [invocation setArgument:&v atIndex:i]; }
            else { int v = (int)arguments[i]; [invocation setArgument:&v atIndex:i]; }
        }
        @try { [invocation invoke]; }
        @catch (NSException *e) {
            fprintf(stderr, "compat32: fpret %s raised %s\n", selector_name, [[e reason] UTF8String]);
            *result = compat_runtime32_return_double(0.0);
            return 1;
        }
        const char *rt = skip_type_qualifiers([signature methodReturnType]);
        if (*rt == 'f') { float f = 0; [invocation getReturnValue:&f]; *result = compat_runtime32_return_float(f); }
        else { double d = 0; [invocation getReturnValue:&d]; *result = compat_runtime32_return_double(d); }
        return 1;
    }

    if (LP32_NAME_IS(import_name, import_length, "_objc_msgSend_stret")) {
        /* A struct-returning method defined by a synthesized guest class
           (e.g. -[SexyWindow contentRectForContent:preferred:safe:min:...])
           dispatches to the guest IMP.  Do this before setting the caller's
           callee-pop, because the guest call runs its own imports through the
           gateway and would otherwise consume the pending pop. */
        {
            uint64_t guest_result = 0;
            if (guest_objc_msgsend_stret(arguments, 16, &guest_result)) {
                *result = guest_result;
                compat_runtime32_struct_return();
                return 1;
            }
        }
        /* i386 struct-return functions pop the hidden struct pointer (ret $4);
           tell the gateway to advance the guest ESP by 4 so the caller's stack
           stays balanced.  Without this every stret call (e.g. -[NSScreen
           frame]) leaks 4 bytes and eventually corrupts a return address. */
        compat_runtime32_struct_return();
        struct guest_rect *destination = (void *)(uintptr_t)arguments[0];
        id receiver = object_for_receiver(arguments[1]);
        const char *selector_name = (const char *)(uintptr_t)arguments[2];
        if (!arguments[1] && destination) {
            memset(destination, 0, sizeof(*destination));
            *result = 0;
            return 1;
        }
        if (!destination || !receiver || !selector_name) return 0;

        NSRect rect;
        if (strcmp(selector_name, "frame") == 0 ||
            strcmp(selector_name, "visibleFrame") == 0) {
            if (guest_display_mode_active() &&
                [receiver isKindOfClass:[NSScreen class]] &&
                receiver == preferred_game_screen()) {
                /* After a mode switch the old API reported the new mode as
                   the screen size; keep the guest's view of it consistent. */
                rect = NSMakeRect(0, 0, guest_display_mode.width,
                                  guest_display_mode.height);
            } else if (strcmp(selector_name, "frame") == 0) {
                rect = [(NSScreen *)receiver frame];
            } else {
                rect = [(NSScreen *)receiver visibleFrame];
            }
        } else if (strcmp(selector_name,
                          "contentRectForFrameRect:styleMask:") == 0) {
            const float *guest_input = (const void *)(arguments + 3);
            NSRect input = NSMakeRect(guest_input[0], guest_input[1],
                                      guest_input[2], guest_input[3]);
            rect = [NSWindow contentRectForFrameRect:input
                                           styleMask:arguments[7]];
        } else {
            /* General host struct-return path for a geometry selector the
               fixed cases above do not name (e.g. -[NSView bounds]/frame, or
               -[NSWindow convert...] with a point/rect argument).  Invoke it
               through its real signature, marshalling the guest argument words
               (i386 CGFloat is 32-bit, passed as plain stack words), and repack
               the returned CGFloat struct into the guest's packed-float layout. */
            id host_receiver = object_for_receiver(arguments[1]);
            SEL sel = arguments[2]
                ? sel_registerName((const char *)(uintptr_t)arguments[2]) : NULL;
            NSMethodSignature *signature =
                (host_receiver && sel && [host_receiver respondsToSelector:sel])
                    ? [host_receiver methodSignatureForSelector:sel] : nil;
            const char *return_type = signature ? [signature methodReturnType] : NULL;
            if (signature && return_type && return_type[0] == '{') {
                NSInvocation *invocation =
                    [NSInvocation invocationWithMethodSignature:signature];
                [invocation setSelector:sel];
                [invocation setTarget:host_receiver];
                const uint32_t *guest_args = arguments + 3;
                size_t word = 0;
                bool ok = true;
                for (NSUInteger i = 2; ok && i < [signature numberOfArguments]; ++i) {
                    const char *t = skip_type_qualifiers([signature getArgumentTypeAtIndex:i]);
                    switch (*t) {
                        case '@': case '#': { id v = object_for_argument(guest_args[word]); [invocation setArgument:&v atIndex:i]; word += 1; break; }
                        case ':': { SEL v = guest_args[word] ? sel_registerName((const char *)(uintptr_t)guest_args[word]) : (SEL)0; [invocation setArgument:&v atIndex:i]; word += 1; break; }
                        case '*': case '^': { id ho = peggle_object_for_handle(guest_args[word]); void *v = ho ? (void *)ho : (void *)(uintptr_t)guest_args[word]; [invocation setArgument:&v atIndex:i]; word += 1; break; }
                        case 'c': { int8_t v = (int8_t)guest_args[word]; [invocation setArgument:&v atIndex:i]; word += 1; break; }
                        case 'C': case 'B': { uint8_t v = (uint8_t)guest_args[word]; [invocation setArgument:&v atIndex:i]; word += 1; break; }
                        case 's': { int16_t v = (int16_t)guest_args[word]; [invocation setArgument:&v atIndex:i]; word += 1; break; }
                        case 'S': { uint16_t v = (uint16_t)guest_args[word]; [invocation setArgument:&v atIndex:i]; word += 1; break; }
                        case 'i': case 'l': { int32_t v = (int32_t)guest_args[word]; [invocation setArgument:&v atIndex:i]; word += 1; break; }
                        case 'I': case 'L': { uint32_t v = guest_args[word]; [invocation setArgument:&v atIndex:i]; word += 1; break; }
                        case 'q': { int64_t v = (int32_t)guest_args[word]; [invocation setArgument:&v atIndex:i]; word += 1; break; }
                        case 'Q': { uint64_t v = guest_args[word]; [invocation setArgument:&v atIndex:i]; word += 1; break; }
                        case 'f': { float v; memcpy(&v, &guest_args[word], sizeof(v)); [invocation setArgument:&v atIndex:i]; word += 1; break; }
                        case 'd': { double v; memcpy(&v, &guest_args[word], sizeof(v)); [invocation setArgument:&v atIndex:i]; word += 2; break; }
                        case '{': {
                            const float *fields = (const float *)&guest_args[word];
                            if (strncmp(t, "{CGRect=", 8) == 0 || strncmp(t, "{_NSRect=", 9) == 0) {
                                NSRect v = NSMakeRect(fields[0], fields[1], fields[2], fields[3]);
                                [invocation setArgument:&v atIndex:i]; word += 4;
                            } else if (strncmp(t, "{CGPoint=", 9) == 0 || strncmp(t, "{_NSPoint=", 10) == 0) {
                                NSPoint v = NSMakePoint(fields[0], fields[1]);
                                [invocation setArgument:&v atIndex:i]; word += 2;
                            } else if (strncmp(t, "{CGSize=", 8) == 0 || strncmp(t, "{_NSSize=", 9) == 0) {
                                NSSize v = NSMakeSize(fields[0], fields[1]);
                                [invocation setArgument:&v atIndex:i]; word += 2;
                            } else { ok = false; }
                            break;
                        }
                        default: ok = false; break;
                    }
                }
                if (ok) {
                    [invocation invoke];
                    NSUInteger length = [signature methodReturnLength];
                    float *out = (float *)destination;
                    if (length == sizeof(NSRect)) {
                        NSRect value; [invocation getReturnValue:&value];
                        out[0] = (float)value.origin.x; out[1] = (float)value.origin.y;
                        out[2] = (float)value.size.width; out[3] = (float)value.size.height;
                        *result = 0; return 1;
                    }
                    if (length == sizeof(NSSize)) {  /* also NSPoint (both 2 CGFloats) */
                        NSSize value; [invocation getReturnValue:&value];
                        out[0] = (float)value.width; out[1] = (float)value.height;
                        *result = 0; return 1;
                    }
                }
            }
            return 0;
        }
        destination->x = (float)rect.origin.x;
        destination->y = (float)rect.origin.y;
        destination->width = (float)rect.size.width;
        destination->height = (float)rect.size.height;
        *result = 0;
        return 1;
    }

    return 0;
}

uint32_t objc_bridge32_pointer_import(const char *import_name)
{
    if (!strcmp(import_name,"_kCFBundleVersionKey")) return proxy_for_object((id)kCFBundleVersionKey);
    if (!strcmp(import_name,"_kCFPreferencesCurrentApplication")) return proxy_for_object(@"local.peggle.compat");
    if (!strcmp(import_name,"_kCFPreferencesAnyHost")) return proxy_for_object((id)kCFPreferencesAnyHost);
    if (!strcmp(import_name,"_kCFPreferencesCurrentUser")) return proxy_for_object((id)kCFPreferencesCurrentUser);
    if (!strcmp(import_name,"_kCFPreferencesAnyApplication")) return proxy_for_object((id)kCFPreferencesAnyApplication);
    const size_t import_length = strlen(import_name);
    if (LP32_NAME_IS(import_name, import_length, "_NSApp")) {
        return proxy_for_object([NSApplication sharedApplication]);
    }
    if (LP32_NAME_IS(import_name, import_length, "_kCFLocaleCountryCode")) {
        return proxy_for_object((NSString *)kCFLocaleCountryCode);
    }
    if (LP32_NAME_IS(import_name, import_length, "_kCFLocaleLanguageCode")) {
        return proxy_for_object((NSString *)kCFLocaleLanguageCode);
    }
    if (LP32_NAME_IS(import_name, import_length, "_kCFRunLoopDefaultMode")) {
        return proxy_for_object((NSString *)kCFRunLoopDefaultMode);
    }
    if (LP32_NAME_IS(import_name, import_length, "_kTISPropertyInputSourceLanguages")) {
        return proxy_for_object((NSString *)kTISPropertyInputSourceLanguages);
    }
    if (LP32_NAME_IS(import_name, import_length, "_kTISPropertyUnicodeKeyLayoutData")) {
        return proxy_for_object((NSString *)kTISPropertyUnicodeKeyLayoutData);
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSControlTextDidChangeNotification")) {
        return proxy_for_object(NSControlTextDidChangeNotification);
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSDefaultRunLoopMode")) {
        return proxy_for_object(NSDefaultRunLoopMode);
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSLinkAttributeName")) {
        return proxy_for_object(NSLinkAttributeName);
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSViewGlobalFrameDidChangeNotification")) {
        return proxy_for_object(@"NSViewGlobalFrameDidChangeNotification");
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSWindowWillCloseNotification")) {
        return proxy_for_object(NSWindowWillCloseNotification);
    }
    return 0;
}

int objc_bridge32_run_gl_parameter_self_test(void)
{
    if (split_program_parameter_batches()) {
        fputs("gl-parameter-selftest: default batch policy is split\n",
              stderr);
        return -1;
    }
    const GLfloat finite_values[] = {
        1.0f, -2.0f, 3.5f, 0.0f,
        4.0f, 5.0f, 6.0f, 7.0f,
    };
    size_t replaced = SIZE_MAX;
    const GLfloat *safe = safe_program_parameter_values(
        finite_values, 2, &replaced);
    if (safe != finite_values || replaced != 0) {
        fputs("gl-parameter-selftest: finite fast path failed\n", stderr);
        return -1;
    }

    const GLfloat nonfinite_values[] = {
        1.0f, NAN, 3.5f, INFINITY,
        -INFINITY, 5.0f, 6.0f, 7.0f,
    };
    safe = safe_program_parameter_values(nonfinite_values, 2, &replaced);
    if (safe == nonfinite_values || replaced != 3 ||
        safe[0] != 1.0f || safe[1] != 0.0f || safe[2] != 3.5f ||
        safe[3] != 0.0f || safe[4] != 0.0f || safe[5] != 5.0f ||
        safe[6] != 6.0f || safe[7] != 7.0f) {
        fputs("gl-parameter-selftest: non-finite containment failed\n",
              stderr);
        return -1;
    }
    if (!isnan(nonfinite_values[1]) || !isinf(nonfinite_values[3]) ||
        !isinf(nonfinite_values[4])) {
        fputs("gl-parameter-selftest: modified source values\n", stderr);
        return -1;
    }

    puts("gl-parameter-selftest: PASS (atomic batch policy and finite containment)");
    return 0;
}

int objc_bridge32_run_gl_buffer_self_test(void)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGLPixelFormatAttribute attributes[] = {
        kCGLPFAAccelerated,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj format = NULL;
    GLint format_count = 0;
    CGLContextObj context = NULL;
    CGLContextObj shared_context = NULL;
    CGLError error = CGLChoosePixelFormat(attributes, &format, &format_count);
    if (error != kCGLNoError || !format || format_count < 1) {
        fprintf(stderr, "gl-buffer-selftest: pixel format failed: %d\n", error);
        if (format) CGLDestroyPixelFormat(format);
        return -1;
    }
    error = CGLCreateContext(format, NULL, &context);
    if (error == kCGLNoError && context) {
        error = CGLCreateContext(format, context, &shared_context);
    }
    CGLDestroyPixelFormat(format);
    if (error != kCGLNoError || !context || !shared_context) {
        fprintf(stderr, "gl-buffer-selftest: context creation failed: %d\n",
                error);
        if (shared_context) CGLDestroyContext(shared_context);
        if (context) CGLDestroyContext(context);
        return -1;
    }
    error = CGLSetCurrentContext(context);
#pragma clang diagnostic pop
    if (error != kCGLNoError) {
        fprintf(stderr, "gl-buffer-selftest: make-current failed: %d\n", error);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CGLDestroyContext(shared_context);
        CGLDestroyContext(context);
#pragma clang diagnostic pop
        return -1;
    }

    enum { kTestSize = 32 };
    unsigned char initial_a[kTestSize];
    unsigned char initial_b[kTestSize];
    unsigned char actual[kTestSize];
    memset(initial_a, 0x11, sizeof(initial_a));
    memset(initial_b, 0x22, sizeof(initial_b));
    GLuint buffers[2] = {0, 0};
    glGenBuffers(2, buffers);
    uint64_t dispatch_result = 0;
    int status = 0;

    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, kTestSize, initial_a, GL_DYNAMIC_DRAW);
    const uint32_t disable_flush[] = {
        GL_ARRAY_BUFFER, GL_BUFFER_FLUSHING_UNMAP_APPLE, GL_FALSE,
    };
    objc_bridge32_dispatch("glBufferParameteriAPPLE", disable_flush,
                           &dispatch_result);
    const uint32_t map_arguments[] = {GL_ARRAY_BUFFER, GL_WRITE_ONLY};
    objc_bridge32_dispatch("glMapBuffer", map_arguments, &dispatch_result);
    uint32_t first_mapping = (uint32_t)dispatch_result;
    if (!first_mapping) {
        fputs("gl-buffer-selftest: first map failed\n", stderr);
        status = -1;
        goto cleanup;
    }
    memset((void *)(uintptr_t)first_mapping, 0xa1, kTestSize);
    const uint32_t flush_arguments[] = {GL_ARRAY_BUFFER, 8, 8};
    objc_bridge32_dispatch("glFlushMappedBufferRangeAPPLE", flush_arguments,
                           &dispatch_result);
    const uint32_t unmap_arguments[] = {GL_ARRAY_BUFFER};
    objc_bridge32_dispatch("glUnmapBuffer", unmap_arguments, &dispatch_result);
    memset(actual, 0, sizeof(actual));
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, kTestSize, actual);
    for (size_t index = 0; index < kTestSize; ++index) {
        unsigned char expected = index >= 8 && index < 16 ? 0xa1 : 0x11;
        if (actual[index] != expected) {
            fprintf(stderr,
                    "gl-buffer-selftest: explicit range mismatch at %zu: "
                    "%02x != %02x\n",
                    index, actual[index], expected);
            status = -1;
            goto cleanup;
        }
    }

    /* Per-object state must follow the VBO into a shared loading context. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    error = CGLSetCurrentContext(shared_context);
#pragma clang diagnostic pop
    if (error != kCGLNoError) {
        fprintf(stderr,
                "gl-buffer-selftest: shared make-current failed: %d\n", error);
        status = -1;
        goto cleanup;
    }
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    objc_bridge32_dispatch("glMapBuffer", map_arguments, &dispatch_result);
    if (!dispatch_result) {
        fputs("gl-buffer-selftest: shared-context map failed\n", stderr);
        status = -1;
        goto cleanup;
    }
    memset((void *)(uintptr_t)(uint32_t)dispatch_result, 0xc3, kTestSize);
    objc_bridge32_dispatch("glUnmapBuffer", unmap_arguments, &dispatch_result);
    memset(actual, 0, sizeof(actual));
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, kTestSize, actual);
    for (size_t index = 0; index < kTestSize; ++index) {
        unsigned char expected = index >= 8 && index < 16 ? 0xa1 : 0x11;
        if (actual[index] != expected) {
            fprintf(stderr,
                    "gl-buffer-selftest: shared policy mismatch at %zu: "
                    "%02x != %02x\n",
                    index, actual[index], expected);
            status = -1;
            goto cleanup;
        }
    }

    /* A second buffer on the same target must retain the default TRUE. */
    glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ARRAY_BUFFER, kTestSize, initial_b, GL_DYNAMIC_DRAW);
    objc_bridge32_dispatch("glMapBuffer", map_arguments, &dispatch_result);
    uint32_t second_mapping = (uint32_t)dispatch_result;
    if (!second_mapping) {
        fprintf(stderr,
                "gl-buffer-selftest: second mapping failed: %08x\n",
                second_mapping);
        status = -1;
        goto cleanup;
    }
    memset((void *)(uintptr_t)second_mapping, 0xb2, kTestSize);
    objc_bridge32_dispatch("glUnmapBuffer", unmap_arguments, &dispatch_result);
    memset(actual, 0, sizeof(actual));
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, kTestSize, actual);
    for (size_t index = 0; index < kTestSize; ++index) {
        if (actual[index] != 0xb2) {
            fprintf(stderr,
                    "gl-buffer-selftest: default unmap mismatch at %zu: %02x\n",
                    index, actual[index]);
            status = -1;
            goto cleanup;
        }
    }

    /* Core map-range flush offsets are relative to the mapped window. */
    glBufferData(GL_ARRAY_BUFFER, kTestSize, initial_b, GL_DYNAMIC_DRAW);
    const uint32_t map_range_arguments[] = {
        GL_ARRAY_BUFFER, 4, 16,
        GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT,
    };
    objc_bridge32_dispatch("glMapBufferRange", map_range_arguments,
                           &dispatch_result);
    uint32_t range_mapping = (uint32_t)dispatch_result;
    if (!range_mapping) {
        fputs("gl-buffer-selftest: explicit range map failed\n", stderr);
        status = -1;
        goto cleanup;
    }
    memset((void *)(uintptr_t)range_mapping, 0xd4, 16);
    const uint32_t range_flush_arguments[] = {GL_ARRAY_BUFFER, 3, 5};
    objc_bridge32_dispatch("glFlushMappedBufferRange", range_flush_arguments,
                           &dispatch_result);
    objc_bridge32_dispatch("glUnmapBuffer", unmap_arguments, &dispatch_result);
    memset(actual, 0, sizeof(actual));
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, kTestSize, actual);
    for (size_t index = 0; index < kTestSize; ++index) {
        unsigned char expected = index >= 7 && index < 12 ? 0xd4 : 0x22;
        if (actual[index] != expected) {
            fprintf(stderr,
                    "gl-buffer-selftest: relative range mismatch at %zu: "
                    "%02x != %02x\n",
                    index, actual[index], expected);
            status = -1;
            goto cleanup;
        }
    }

    /* A non-explicit writable range is committed in full on unmap. */
    const uint32_t implicit_range_arguments[] = {
        GL_ARRAY_BUFFER, 12, 8,
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT,
    };
    objc_bridge32_dispatch("glMapBufferRange", implicit_range_arguments,
                           &dispatch_result);
    range_mapping = (uint32_t)dispatch_result;
    if (!range_mapping) {
        fputs("gl-buffer-selftest: implicit range map failed\n", stderr);
        status = -1;
        goto cleanup;
    }
    memset((void *)(uintptr_t)range_mapping, 0xe5, 8);
    objc_bridge32_dispatch("glUnmapBuffer", unmap_arguments, &dispatch_result);
    memset(actual, 0, sizeof(actual));
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, kTestSize, actual);
    for (size_t index = 0; index < kTestSize; ++index) {
        unsigned char expected = index >= 12 && index < 20 ? 0xe5 :
            (index >= 7 && index < 12 ? 0xd4 : 0x22);
        if (actual[index] != expected) {
            fprintf(stderr,
                    "gl-buffer-selftest: implicit range mismatch at %zu: "
                    "%02x != %02x\n",
                    index, actual[index], expected);
            status = -1;
            goto cleanup;
        }
    }

cleanup:
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CGLSetCurrentContext(context);
#pragma clang diagnostic pop
        uint32_t guest_buffers = compat_runtime32_allocate(sizeof(buffers), 0);
        if (guest_buffers) {
            memcpy((void *)(uintptr_t)guest_buffers, buffers, sizeof(buffers));
            const uint32_t delete_arguments[] = {2, guest_buffers};
            objc_bridge32_dispatch("glDeleteBuffers", delete_arguments,
                                   &dispatch_result);
            compat_runtime32_deallocate(guest_buffers);
        } else {
            glDeleteBuffers(2, buffers);
            status = -1;
        }
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(shared_context);
    CGLDestroyContext(context);
#pragma clang diagnostic pop
    if (status == 0) {
        puts("gl-buffer-selftest: PASS (per-object Apple and core mapped-range staging)");
    }
    return status;
}

static size_t compressed_texture_bytes(GLsizei width, GLsizei height,
                                       size_t block_bytes)
{
    size_t blocks_wide = (size_t)(width > 4 ? width : 4) / 4;
    size_t blocks_high = (size_t)(height > 4 ? height : 4) / 4;
    return blocks_wide * blocks_high * block_bytes;
}

static void fill_compressed_texture_pattern(unsigned char *bytes, size_t size,
                                            unsigned int seed)
{
    uint32_t value = UINT32_C(0x9e3779b9) ^ seed;
    for (size_t index = 0; index < size; ++index) {
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        bytes[index] = (unsigned char)(value >> 24);
    }
}

static int run_compressed_texture_case(CGLContextObj upload_context,
                                       CGLContextObj render_context,
                                       GLenum format, size_t block_bytes,
                                       GLsizei width, GLsizei height,
                                       bool sliced, unsigned int seed)
{
    size_t byte_count = compressed_texture_bytes(width, height, block_bytes);
    unsigned char *expected = malloc(byte_count);
    unsigned char *actual = malloc(byte_count);
    unsigned char *decoded = malloc((size_t)width * (size_t)height * 4);
    uint32_t guest_bytes = compat_runtime32_allocate(byte_count, 0);
    if (!expected || !actual || !decoded || !guest_bytes) {
        fputs("gl-texture-selftest: allocation failed\n", stderr);
        free(decoded);
        free(actual);
        free(expected);
        if (guest_bytes) compat_runtime32_deallocate(guest_bytes);
        return -1;
    }
    fill_compressed_texture_pattern(expected, byte_count, seed);
    memcpy((void *)(uintptr_t)guest_bytes, expected, byte_count);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGLError error = CGLSetCurrentContext(upload_context);
#pragma clang diagnostic pop
    if (error != kCGLNoError) {
        fprintf(stderr, "gl-texture-selftest: upload make-current failed: %d\n",
                error);
        compat_runtime32_deallocate(guest_bytes);
        free(decoded);
        free(actual);
        free(expected);
        return -1;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    uint64_t dispatch_result = 0;
    int status = 0;
    if (!sliced) {
        const uint32_t arguments[] = {
            GL_TEXTURE_2D, 0, format, (uint32_t)width, (uint32_t)height, 0,
            (uint32_t)byte_count, guest_bytes,
        };
        objc_bridge32_dispatch("glCompressedTexImage2D", arguments,
                               &dispatch_result);
    } else {
        const uint32_t allocate_arguments[] = {
            GL_TEXTURE_2D, 0, format, (uint32_t)width, (uint32_t)height, 0,
            (uint32_t)byte_count, 0,
        };
        objc_bridge32_dispatch("glCompressedTexImage2D", allocate_arguments,
                               &dispatch_result);
        GLsizei y = 0;
        size_t source_offset = 0;
        while (y < height) {
            GLsizei rows = height - y > 256 ? 256 : height - y;
            size_t slice_bytes = compressed_texture_bytes(
                width, rows, block_bytes);
            const uint32_t update_arguments[] = {
                GL_TEXTURE_2D, 0, 0, (uint32_t)y,
                (uint32_t)width, (uint32_t)rows, format,
                (uint32_t)slice_bytes, guest_bytes + (uint32_t)source_offset,
            };
            objc_bridge32_dispatch("glCompressedTexSubImage2D",
                                   update_arguments, &dispatch_result);
            source_offset += slice_bytes;
            y += rows;
        }
        if (source_offset != byte_count) {
            fputs("gl-texture-selftest: sliced byte accounting failed\n",
                  stderr);
            status = -1;
        }
    }
    glFinish();

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    error = CGLSetCurrentContext(render_context);
#pragma clang diagnostic pop
    if (error != kCGLNoError) {
        fprintf(stderr, "gl-texture-selftest: render make-current failed: %d\n",
                error);
        status = -1;
        goto cleanup;
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    GLint stored_width = 0;
    GLint stored_height = 0;
    GLint stored_format = 0;
    GLint stored_size = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
                             &stored_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,
                             &stored_height);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT,
                             &stored_format);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0,
                             GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &stored_size);
    if (stored_width != width || stored_height != height ||
        stored_format != (GLint)format || stored_size != (GLint)byte_count) {
        fprintf(stderr,
                "gl-texture-selftest: metadata mismatch sliced=%d "
                "size=%dx%d/%dx%d format=%04x/%04x bytes=%d/%zu\n",
                sliced, stored_width, stored_height, width, height,
                stored_format, format, stored_size, byte_count);
        status = -1;
        goto cleanup;
    }
    memset(actual, 0, byte_count);
    glGetCompressedTexImage(GL_TEXTURE_2D, 0, actual);
    if (memcmp(actual, expected, byte_count) != 0) {
        size_t mismatch = 0;
        while (mismatch < byte_count && actual[mismatch] == expected[mismatch]) {
            ++mismatch;
        }
        fprintf(stderr,
                "gl-texture-selftest: compressed payload mismatch "
                "sliced=%d format=%04x at=%zu actual=%02x expected=%02x\n",
                sliced, format, mismatch,
                mismatch < byte_count ? actual[mismatch] : 0,
                mismatch < byte_count ? expected[mismatch] : 0);
        status = -1;
        goto cleanup;
    }
    /* Force the driver to decode the texture as well as preserve its blocks. */
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, decoded);
    {
        GLenum gl_error = glGetError();
        if (gl_error != GL_NO_ERROR) {
            fprintf(stderr,
                    "gl-texture-selftest: decode readback failed "
                    "sliced=%d format=%04x error=%04x\n",
                    sliced, format, gl_error);
            status = -1;
        }
    }

cleanup:
    glDeleteTextures(1, &texture);
    compat_runtime32_deallocate(guest_bytes);
    free(decoded);
    free(actual);
    free(expected);
    return status;
}

static int run_3d_texture_dispatch_case(CGLContextObj upload_context,
                                        CGLContextObj render_context)
{
    enum {
        kWidth = 4,
        kHeight = 3,
        kDepth = 2,
        kBytesPerPixel = 4,
        kByteCount = kWidth * kHeight * kDepth * kBytesPerPixel,
    };
    unsigned char expected[kByteCount];
    unsigned char actual[kByteCount];
    for (size_t index = 0; index < sizeof(expected); ++index) {
        expected[index] = (unsigned char)(index * 37u + 11u);
    }

    uint32_t guest_bytes = compat_runtime32_allocate(sizeof(expected), 0);
    if (!guest_bytes) {
        fputs("gl-texture-selftest: 3D guest allocation failed\n", stderr);
        return -1;
    }
    memcpy((void *)(uintptr_t)guest_bytes, expected, sizeof(expected));

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGLError context_error = CGLSetCurrentContext(upload_context);
#pragma clang diagnostic pop
    if (context_error != kCGLNoError) {
        fprintf(stderr,
                "gl-texture-selftest: 3D upload make-current failed: %d\n",
                context_error);
        compat_runtime32_deallocate(guest_bytes);
        return -1;
    }

    GLuint texture = 0;
    int status = 0;
    uint64_t dispatch_result = 0;
    while (glGetError() != GL_NO_ERROR) {}
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_3D, texture);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    const uint32_t image_arguments[] = {
        GL_TEXTURE_3D, 0, GL_RGBA8, kWidth, kHeight, kDepth, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, guest_bytes,
    };
    if (!objc_bridge32_dispatch("glTexImage3D", image_arguments,
                                &dispatch_result)) {
        fputs("gl-texture-selftest: glTexImage3D was not dispatched\n",
              stderr);
        status = -1;
        goto cleanup_3d;
    }
    GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        fprintf(stderr,
                "gl-texture-selftest: glTexImage3D failed: %04x\n",
                gl_error);
        status = -1;
        goto cleanup_3d;
    }
    glFinish();

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    context_error = CGLSetCurrentContext(render_context);
#pragma clang diagnostic pop
    if (context_error != kCGLNoError) {
        fprintf(stderr,
                "gl-texture-selftest: 3D render make-current failed: %d\n",
                context_error);
        status = -1;
        goto cleanup_3d;
    }
    glBindTexture(GL_TEXTURE_3D, texture);
    GLint stored_width = 0;
    GLint stored_height = 0;
    GLint stored_depth = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_WIDTH,
                             &stored_width);
    glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_HEIGHT,
                             &stored_height);
    glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_DEPTH,
                             &stored_depth);
    memset(actual, 0, sizeof(actual));
    glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_UNSIGNED_BYTE, actual);
    gl_error = glGetError();
    if (gl_error != GL_NO_ERROR || stored_width != kWidth ||
        stored_height != kHeight || stored_depth != kDepth ||
        memcmp(actual, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "gl-texture-selftest: 3D upload mismatch error=%04x "
                "size=%dx%dx%d expected=%dx%dx%d bytes_equal=%d\n",
                gl_error, stored_width, stored_height, stored_depth,
                kWidth, kHeight, kDepth,
                memcmp(actual, expected, sizeof(expected)) == 0);
        status = -1;
        goto cleanup_3d;
    }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    context_error = CGLSetCurrentContext(upload_context);
#pragma clang diagnostic pop
    if (context_error != kCGLNoError) {
        status = -1;
        goto cleanup_3d;
    }
    static const unsigned char replacement[] = {0xde, 0xad, 0xbe, 0xef};
    memcpy((void *)(uintptr_t)guest_bytes, replacement, sizeof(replacement));
    const uint32_t update_arguments[] = {
        GL_TEXTURE_3D, 0, 1, 1, 1, 1, 1, 1,
        GL_RGBA, GL_UNSIGNED_BYTE, guest_bytes,
    };
    if (!objc_bridge32_dispatch("glTexSubImage3DEXT", update_arguments,
                                &dispatch_result)) {
        fputs("gl-texture-selftest: glTexSubImage3DEXT was not dispatched\n",
              stderr);
        status = -1;
        goto cleanup_3d;
    }
    gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        fprintf(stderr,
                "gl-texture-selftest: glTexSubImage3DEXT failed: %04x\n",
                gl_error);
        status = -1;
        goto cleanup_3d;
    }
    memcpy(expected + (((1 * kHeight + 1) * kWidth + 1) * kBytesPerPixel),
           replacement, sizeof(replacement));
    glFinish();

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    context_error = CGLSetCurrentContext(render_context);
#pragma clang diagnostic pop
    if (context_error != kCGLNoError) {
        status = -1;
        goto cleanup_3d;
    }
    glBindTexture(GL_TEXTURE_3D, texture);
    memset(actual, 0, sizeof(actual));
    glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_UNSIGNED_BYTE, actual);
    gl_error = glGetError();
    if (gl_error != GL_NO_ERROR ||
        memcmp(actual, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "gl-texture-selftest: 3D subimage mismatch error=%04x "
                "bytes_equal=%d\n",
                gl_error, memcmp(actual, expected, sizeof(expected)) == 0);
        status = -1;
    }

cleanup_3d:
    if (texture) glDeleteTextures(1, &texture);
    compat_runtime32_deallocate(guest_bytes);
    return status;
}

int objc_bridge32_run_gl_texture_self_test(void)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGLPixelFormatAttribute attributes[] = {
        kCGLPFAAccelerated,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj pixel_format = NULL;
    GLint format_count = 0;
    CGLContextObj render_context = NULL;
    CGLContextObj upload_context = NULL;
    CGLError error = CGLChoosePixelFormat(attributes, &pixel_format,
                                          &format_count);
    if (error == kCGLNoError && pixel_format && format_count > 0) {
        error = CGLCreateContext(pixel_format, NULL, &render_context);
    }
    if (error == kCGLNoError && render_context) {
        error = CGLCreateContext(pixel_format, render_context,
                                 &upload_context);
    }
    if (pixel_format) CGLDestroyPixelFormat(pixel_format);
#pragma clang diagnostic pop
    if (error != kCGLNoError || !render_context || !upload_context) {
        fprintf(stderr,
                "gl-texture-selftest: context creation failed: %d\n", error);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (upload_context) CGLDestroyContext(upload_context);
        if (render_context) CGLDestroyContext(render_context);
#pragma clang diagnostic pop
        return -1;
    }

    static const struct {
        GLenum format;
        size_t block_bytes;
    } cases[] = {
        {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 8},
        {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 16},
    };
    int status = 0;
    for (unsigned int cycle = 0; cycle < 16 && status == 0; ++cycle) {
        for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]);
             ++index) {
            status = run_compressed_texture_case(
                upload_context, render_context, cases[index].format,
                cases[index].block_bytes, 512, 768, false,
                cycle * 17u + (unsigned int)index);
            if (status != 0) break;
            status = run_compressed_texture_case(
                upload_context, render_context, cases[index].format,
                cases[index].block_bytes, 512, 768, true,
                cycle * 17u + (unsigned int)index);
            if (status != 0) break;
            status = run_compressed_texture_case(
                upload_context, render_context, cases[index].format,
                cases[index].block_bytes, 64, 64, true,
                cycle * 31u + (unsigned int)index);
            if (status != 0) break;
        }
    }

    if (status == 0) {
        status = run_3d_texture_dispatch_case(upload_context,
                                              render_context);
    }

    if (status == 0) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        error = CGLSetCurrentContext(upload_context);
#pragma clang diagnostic pop
        if (error != kCGLNoError) {
            status = -1;
        } else {
            GLuint texture = 0;
            unsigned char blocks[2048];
            memset(blocks, 0x5a, sizeof(blocks));
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            GL_LINEAR_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 6);
            for (GLint level = 0; level < 6; ++level) {
                GLsizei dimension = 64 >> level;
                size_t size = compressed_texture_bytes(
                    dimension, dimension, 8);
                glCompressedTexImage2D(
                    GL_TEXTURE_2D, level,
                    GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                    dimension, dimension, 0, (GLsizei)size, blocks);
                repair_bound_texture_mipmap_range(GL_TEXTURE_2D);
            }
            GLint maximum = -1;
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                                &maximum);
            if (maximum != 5) {
                fprintf(stderr,
                        "gl-texture-selftest: incomplete chain was not "
                        "clamped: max=%d expected=5\n", maximum);
                status = -1;
            }
            glCompressedTexImage2D(
                GL_TEXTURE_2D, 6,
                GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                1, 1, 0, 8, blocks);
            repair_bound_texture_mipmap_range(GL_TEXTURE_2D);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                                &maximum);
            if (maximum != 6) {
                fprintf(stderr,
                        "gl-texture-selftest: completed chain did not "
                        "restore requested max: max=%d expected=6\n",
                        maximum);
                status = -1;
            }
            forget_mipmap_repairs(1, &texture);
            glDeleteTextures(1, &texture);

            if (status == 0) {
                texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_CUBE_MAP, texture);
                glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                                GL_LINEAR_MIPMAP_NEAREST);
                glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL,
                                6);
                for (GLint level = 0; level < 6; ++level) {
                    GLsizei dimension = 64 >> level;
                    size_t size = compressed_texture_bytes(
                        dimension, dimension, 8);
                    for (unsigned int face = 0; face < 6; ++face) {
                        GLenum face_target =
                            GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
                        glCompressedTexImage2D(
                            face_target, level,
                            GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                            dimension, dimension, 0, (GLsizei)size, blocks);
                        repair_bound_texture_mipmap_range(face_target);
                    }
                }
                glGetTexParameteriv(GL_TEXTURE_CUBE_MAP,
                                    GL_TEXTURE_MAX_LEVEL, &maximum);
                if (maximum != 5) {
                    fprintf(stderr,
                            "gl-texture-selftest: incomplete cube chain was "
                            "not clamped: max=%d expected=5\n", maximum);
                    status = -1;
                }
                for (unsigned int face = 0; face < 5; ++face) {
                    GLenum face_target =
                        GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
                    glCompressedTexImage2D(
                        face_target, 6,
                        GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                        1, 1, 0, 8, blocks);
                    repair_bound_texture_mipmap_range(face_target);
                }
                glGetTexParameteriv(GL_TEXTURE_CUBE_MAP,
                                    GL_TEXTURE_MAX_LEVEL, &maximum);
                if (maximum != 5) {
                    fprintf(stderr,
                            "gl-texture-selftest: five-face cube level "
                            "escaped clamp: max=%d expected=5\n", maximum);
                    status = -1;
                }
                glCompressedTexImage2D(
                    GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, 6,
                    GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                    1, 1, 0, 8, blocks);
                repair_bound_texture_mipmap_range(
                    GL_TEXTURE_CUBE_MAP_NEGATIVE_Z);
                glGetTexParameteriv(GL_TEXTURE_CUBE_MAP,
                                    GL_TEXTURE_MAX_LEVEL, &maximum);
                if (maximum != 6) {
                    fprintf(stderr,
                            "gl-texture-selftest: completed cube chain did "
                            "not restore requested max: max=%d expected=6\n",
                            maximum);
                    status = -1;
                }
                forget_mipmap_repairs(1, &texture);
                glDeleteTextures(1, &texture);
            }
        }
    }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(upload_context);
    CGLDestroyContext(render_context);
#pragma clang diagnostic pop
    if (status == 0) {
        puts("gl-texture-selftest: PASS (DXT1/DXT5 direct and sliced "
             "uploads survive shared contexts and texture-name reuse; "
             "3D image/subimage dispatch works; incomplete 2D/cubemap "
             "ranges are clamped and restored)");
    }
    return status;
}
