#include "guest_profiler.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <unistd.h>

extern uint16_t lp32_cs32;

enum { kProfileCapacity = 1u << 22, kProfileDepth = 6 };
/* Guest thread stacks (compat_runtime.c kGuestStackBase/Size). */
enum { kProfileStackBase = 0x7c000000u, kProfileStackEnd = 0x7f000000u };
#define kHostTag (UINT64_C(1) << 63)

static uint64_t *profile_samples;
static uint32_t *profile_callers;
/* LP32_PROFILE_WATCH=<lo>-<hi>: for samples inside [lo, hi) also record the
   two stack arguments [ebp+8] and [ebp+12] of the interrupted frame. */
static uint32_t profile_watch_lo, profile_watch_hi;
static uint32_t *profile_watch_values;
static _Atomic uint32_t profile_count;
static pthread_t profile_target;
static unsigned profile_interval_us;
static FILE *profile_out;

static void profile_signal(int signal_number, siginfo_t *info, void *opaque)
{
    (void)signal_number;
    (void)info;
    ucontext_t *context = opaque;
    uint64_t rip = context->uc_mcontext->__ss.__rip;
    uint64_t cs = context->uc_mcontext->__ss.__cs;
    uint32_t index = atomic_fetch_add_explicit(&profile_count, 1,
                                               memory_order_relaxed);
    if (index < kProfileCapacity) {
        profile_samples[index] = cs == lp32_cs32 ? (rip & UINT32_MAX)
                                                 : (rip | kHostTag);
        /* Guest callers via the i386 frame-pointer chain: [ebp] = saved ebp,
           [ebp+4] = return address.  Only follow frames inside the guest
           stack region (always mapped), so the handler cannot fault. */
        uint32_t *callers = &profile_callers[(size_t)index * kProfileDepth];
        unsigned depth = 0;
        if (cs == lp32_cs32) {
            uint32_t ebp = (uint32_t)context->uc_mcontext->__ss.__rbp;
            if (profile_watch_values && rip >= profile_watch_lo &&
                rip < profile_watch_hi && ebp >= kProfileStackBase &&
                ebp + 16 <= kProfileStackEnd) {
                const uint32_t *frame = (const uint32_t *)(uintptr_t)ebp;
                profile_watch_values[(size_t)index * 3] = frame[2];
                profile_watch_values[(size_t)index * 3 + 1] = frame[3];
                /* Two frames up (the caller's caller): its second argument. */
                uint32_t up = frame[0];
                if (up > ebp && up + 16 <= kProfileStackEnd) {
                    const uint32_t *f1 = (const uint32_t *)(uintptr_t)up;
                    uint32_t up2 = f1[0];
                    if (up2 > up && up2 + 16 <= kProfileStackEnd) {
                        const uint32_t *f2 = (const uint32_t *)(uintptr_t)up2;
                        profile_watch_values[(size_t)index * 3 + 2] = f2[3];
                    }
                }
            }
            while (depth < kProfileDepth && ebp >= kProfileStackBase &&
                   ebp + 8 <= kProfileStackEnd) {
                const uint32_t *frame = (const uint32_t *)(uintptr_t)ebp;
                uint32_t next = frame[0], ret = frame[1];
                if (ret < 0x1000 || ret >= 0x7f000000) break;
                callers[depth++] = ret;
                if (next <= ebp) break;
                ebp = next;
            }
        }
        if (depth < kProfileDepth) callers[depth] = 0;
    }
}

/* Safe guest read: fails instead of faulting on an unmapped page. */
static bool guest_read(uint32_t address, void *buffer, size_t size)
{
    mach_vm_size_t out = 0;
    return address >= 0x1000 && address + size <= 0x7f000000 &&
           mach_vm_read_overwrite(mach_task_self(), address, size,
                                  (mach_vm_address_t)(uintptr_t)buffer, &out) ==
               KERN_SUCCESS && out == size;
}

/* Dumps a guest std::wstring (GCC COW layout) the first few times each
   distinct object is seen: rep fields plus the leading characters. */
static void dump_guest_wstring(uint32_t object)
{
    static uint32_t seen[32];
    static unsigned seen_count;
    if (!object) return;
    for (unsigned i = 0; i < seen_count; ++i) if (seen[i] == object) return;
    if (seen_count >= 32) return;
    seen[seen_count++] = object;
    uint32_t data = 0, rep[3] = {0, 0, 0};
    if (!guest_read(object, &data, 4) || !guest_read(data - 12, rep, 12)) {
        fprintf(profile_out, "# wstr obj=0x%" PRIx32 " unreadable\n", object);
        return;
    }
    fprintf(profile_out, "# wstr obj=0x%" PRIx32 " data=0x%" PRIx32
            " len=%" PRIu32 " cap=%" PRIu32 " ref=%" PRId32 " chars:",
            object, data, rep[0], rep[1], (int32_t)rep[2]);
    uint32_t count = rep[0] < 40 ? rep[0] + 2 : 40;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t ch = 0;
        if (!guest_read(data + i * 4, &ch, 4)) { fprintf(profile_out, " ?"); break; }
        if (ch >= 0x20 && ch < 0x7f) fprintf(profile_out, " '%c'", (char)ch);
        else fprintf(profile_out, " %" PRIx32, ch);
    }
    fputc('\n', profile_out);
}

static void profile_flush(uint32_t *flushed)
{
    uint32_t count = atomic_load_explicit(&profile_count, memory_order_acquire);
    if (count > kProfileCapacity) count = kProfileCapacity;
    for (uint32_t index = *flushed; index < count; ++index) {
        uint64_t sample = profile_samples[index];
        if (sample & kHostTag) {
            uint64_t rip = sample & ~kHostTag;
            Dl_info dl;
            if (dladdr((void *)(uintptr_t)rip, &dl) && dl.dli_sname) {
                const char *image = strrchr(dl.dli_fname, '/');
                fprintf(profile_out, "h 0x%" PRIx64 " %s!%s+0x%" PRIx64 "\n",
                        rip, image ? image + 1 : dl.dli_fname, dl.dli_sname,
                        rip - (uint64_t)(uintptr_t)dl.dli_saddr);
            } else if (dladdr((void *)(uintptr_t)rip, &dl) && dl.dli_fname) {
                const char *image = strrchr(dl.dli_fname, '/');
                fprintf(profile_out, "h 0x%" PRIx64 " %s!?+0x%" PRIx64 "\n",
                        rip, image ? image + 1 : dl.dli_fname,
                        rip - (uint64_t)(uintptr_t)dl.dli_fbase);
            } else {
                fprintf(profile_out, "h 0x%" PRIx64 " ?\n", rip);
            }
        } else {
            fprintf(profile_out, "g 0x%" PRIx64, sample);
            const uint32_t *callers = &profile_callers[(size_t)index * kProfileDepth];
            for (unsigned depth = 0; depth < kProfileDepth && callers[depth]; ++depth) {
                fprintf(profile_out, " 0x%" PRIx32, callers[depth]);
            }
            if (profile_watch_values && sample >= profile_watch_lo &&
                sample < profile_watch_hi) {
                fprintf(profile_out, " arg0=0x%" PRIx32 " arg1=0x%" PRIx32
                        " up2arg1=0x%" PRIx32,
                        profile_watch_values[(size_t)index * 3],
                        profile_watch_values[(size_t)index * 3 + 1],
                        profile_watch_values[(size_t)index * 3 + 2]);
                dump_guest_wstring(profile_watch_values[(size_t)index * 3 + 2]);
            }
            fputc('\n', profile_out);
        }
    }
    *flushed = count;
    fflush(profile_out);
}

static void *profile_thread(void *argument)
{
    (void)argument;
    uint32_t flushed = 0;
    unsigned since_flush_us = 0;
    for (;;) {
        usleep(profile_interval_us);
        pthread_kill(profile_target, SIGPROF);
        since_flush_us += profile_interval_us;
        if (since_flush_us >= 2000000) {
            profile_flush(&flushed);
            since_flush_us = 0;
        }
    }
    return NULL;
}

void lp32_guest_profiler_start(void)
{
    const char *text = getenv("LP32_PROFILE_GUEST");
    if (!text || !text[0]) return;
    profile_interval_us = (unsigned)strtoul(text, NULL, 0);
    if (profile_interval_us < 100) profile_interval_us = 1000;
    profile_samples = calloc(kProfileCapacity, sizeof(*profile_samples));
    profile_callers = calloc((size_t)kProfileCapacity * kProfileDepth,
                             sizeof(*profile_callers));
    if (!profile_samples || !profile_callers) return;
    const char *path = getenv("LP32_PROFILE_OUT");
    char buffer[1024];
    if (!path || !path[0]) {
        const char *tmp = getenv("TMPDIR");
        snprintf(buffer, sizeof(buffer), "%s/lp32-profile-%d.txt",
                 tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
        path = buffer;
    }
    profile_out = fopen(path, "w");
    if (!profile_out) {
        perror("compat32: profile output");
        return;
    }
    const char *watch = getenv("LP32_PROFILE_WATCH");
    if (watch && strchr(watch, '-')) {
        profile_watch_lo = (uint32_t)strtoul(watch, NULL, 0);
        profile_watch_hi = (uint32_t)strtoul(strchr(watch, '-') + 1, NULL, 0);
        profile_watch_values = calloc((size_t)kProfileCapacity * 3,
                                      sizeof(*profile_watch_values));
    }
    profile_target = pthread_self();
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = profile_signal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF, &action, NULL);
    pthread_t thread;
    if (pthread_create(&thread, NULL, profile_thread, NULL) == 0) {
        pthread_detach(thread);
        fprintf(stderr, "compat32: guest profiler sampling every %u us -> %s\n",
                profile_interval_us, path);
    }
}
