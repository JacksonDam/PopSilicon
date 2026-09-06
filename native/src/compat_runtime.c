#include "compat_runtime.h"
#include "audio_bridge.h"
#include "game_profile.h"
#include "objc_bridge.h"
#include "name_match.h"

#include <architecture/i386/table.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <runetype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <i386/user_ldt.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

extern uint32_t run_compat32(uint32_t eip, uint32_t esp, uint16_t cs32);
extern void lp32_import_gateway(void);

uint16_t lp32_cs32;
_Thread_local int lp32_leave_guest;
static _Thread_local uint32_t lp32_stack_pop;
void compat_runtime32_struct_return(void) { lp32_stack_pop = 4; }
uint32_t lp32_take_stack_pop(void) { uint32_t n = lp32_stack_pop; lp32_stack_pop = 0; return n; }
_Thread_local int lp32_return_fp_kind;
_Thread_local union {
    float f;
    double d;
    uint64_t bits;
} lp32_fp_result;

enum {
    kBridgeCodeBase = 0x7f000000,
    kBridgeCodeSize = 0x00010000,
    kBridgeDataBase = 0x7f010000,
    kBridgeDataSize = 0x00010000,
    kGuestStackBase = 0x7c000000,
    kGuestStackSize = 0x03000000,
    kGuestStackPerThread = 0x00100000,
    /* Pages 0x0000-0xcfff hold only i386 code and far-pointer data; the
       64-bit landing pads live on their own page and the i386 landing
       trampolines on another.  Rosetta normally translates each region in
       the mode it is executed in, but see the mode guards below: a thread
       can arrive at either kind of pad in the wrong mode. */
    kExitThunkOffset = 0x000,
    kExitFarPointerOffset = 0x010,
    kGatewayFarPointerOffset = 0x040,
    kImportThunksOffset = 0x1000,
    kImportThunkSize = 16,
    kDynamicThunksOffset = 0x5000,
    kDynamicThunkCapacity = 1792,
    kHost64PadsOffset = 0xd000,
    kReturn64Offset = kHost64PadsOffset + 0x000,
    kGateway64Offset = kHost64PadsOffset + 0x040,
    kReturn64AltOffset = kHost64PadsOffset + 0x080,
    kGateway64AltOffset = kHost64PadsOffset + 0x0c0,
    kLanding32Offset = 0xe000,
    kLanding32AltOffset = 0xe040,
    /* Mode-guard event counters: the bridge data region holds import data
       cells from 0x0000, Cocoa proxy slots from 0x8000 (objc_bridge) and the
       startup input latch at 0xfff0 and the SaveCore start latch at 0xffe0
       (game_loader); the cells stop short of this. */
    kModeGuardCountersOffset = 0x7ff0,
    kWrongModeTo64Counter = kBridgeDataBase + kModeGuardCountersOffset + 0,
    kWrongModeTo32Counter = kBridgeDataBase + kModeGuardCountersOffset + 4,
    kModeGuardFailedCounter = kBridgeDataBase + kModeGuardCountersOffset + 8,
};

/* Where the 64-bit gateway sends a thread back into i386 code. */
uint32_t lp32_landing32 = kBridgeCodeBase + kLanding32Offset;

struct __attribute__((packed)) far_ptr32 {
    uint32_t offset;
    uint16_t selector;
};

static struct macho_image32 *current_image;
static _Thread_local int last_call_trapped;
static uint32_t keymgr_slots[64];

enum {
    kGuestHeapBase = 0x02000000,
    kGuestHeapEnd = 0x70000000,
    kGuestHeapHeaderSize = 16,
    kGuestHeapBinCount = 64,
    kGuestHeapSiteCapacity = 512,
    kGuestHeapLiveMagic = 0x4c503332, /* LP32 */
    kGuestHeapFreeMagic = 0x46524545, /* FREE */
    kGuestHeapHostSite = 0xff000001,
    kGuestHeapStringSite = 0xff000002,
    kGuestHeapRenderPoolSite = 0xff000003,
};

struct guest_heap_site_stats {
    uint32_t site;
    uint64_t allocations;
    uint64_t frees;
    uint64_t allocated_bytes;
    uint64_t freed_bytes;
    uint64_t live_bytes;
    uint64_t peak_live_bytes;
};

struct guest_heap_stats {
    uint64_t allocation_calls;
    uint64_t bump_allocations;
    uint64_t reused_allocations;
    uint64_t split_blocks;
    uint64_t failed_allocations;
    uint64_t free_calls;
    uint64_t valid_frees;
    uint64_t null_frees;
    uint64_t invalid_frees;
    uint64_t double_frees;
    uint64_t realloc_calls;
    uint64_t realloc_in_place;
    uint64_t realloc_moved;
    uint64_t live_blocks;
    uint64_t live_requested;
    uint64_t live_capacity;
    uint64_t peak_live_requested;
    uint64_t peak_live_capacity;
    uint64_t reusable_capacity;
    uint64_t abandoned_capacity;
    uintptr_t high_water;
};

static uintptr_t guest_heap_cursor = kGuestHeapBase;
static uint32_t guest_heap_free_bins[kGuestHeapBinCount];
static struct guest_heap_site_stats guest_heap_sites[kGuestHeapSiteCapacity];
static unsigned guest_heap_site_count;
static struct guest_heap_stats guest_heap_statistics;
static pthread_mutex_t guest_heap_lock = PTHREAD_MUTEX_INITIALIZER;
static bool guest_heap_reuse = true;
static bool guest_heap_poison;
static bool guest_heap_trace;
static bool timing_trace;
static uint64_t guest_heap_report_swap_interval = 600;
static uint32_t guest_errno_address;
static _Thread_local char *guest_strtok_state;
static uint32_t guest_pthread_next_key;
static _Thread_local uint32_t guest_pthread_values[128];
/*
 * Multiprocessing Services semaphores.  Handles are (generation << 16 | slot)
 * so a deleted semaphore's slot can be reused while any stale handle, or a
 * waiter that has not yet observed the deletion, is still rejected.  The
 * former implementation handed out 255 handles once and never reused them.
 */
enum {
    kGuestSemaphoreCapacity = 4096,
    kGuestSemaphoreSlotMask = 0xffff,
    kGuestSemaphoreGenerationShift = 16,
};

struct guest_semaphore {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int32_t count;
    int32_t maximum;
    uint32_t generation;
    bool initialized;
    bool constructed;
};

static struct guest_semaphore guest_semaphores[kGuestSemaphoreCapacity];
static uint32_t guest_semaphore_high_water = 1;
static uint32_t guest_semaphore_live_count;
static pthread_mutex_t guest_semaphore_table_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t guest_semaphore_handle(uint32_t slot, uint32_t generation)
{
    return ((generation & kGuestSemaphoreSlotMask) <<
            kGuestSemaphoreGenerationShift) | slot;
}

/* Returns the semaphore with its mutex held, or NULL for a stale handle. */
static struct guest_semaphore *guest_semaphore_acquire(uint32_t handle)
{
    uint32_t slot = handle & kGuestSemaphoreSlotMask;
    uint32_t generation = handle >> kGuestSemaphoreGenerationShift;
    if (!slot || slot >= kGuestSemaphoreCapacity) return NULL;
    struct guest_semaphore *semaphore = &guest_semaphores[slot];
    if (!semaphore->constructed) return NULL;
    pthread_mutex_lock(&semaphore->mutex);
    if (!semaphore->initialized ||
        (semaphore->generation & kGuestSemaphoreSlotMask) != generation) {
        pthread_mutex_unlock(&semaphore->mutex);
        return NULL;
    }
    return semaphore;
}

static uint32_t guest_semaphore_create(int32_t maximum, int32_t initial)
{
    pthread_mutex_lock(&guest_semaphore_table_lock);
    uint32_t slot = 0;
    for (uint32_t candidate = 1; candidate < guest_semaphore_high_water;
         ++candidate) {
        if (!guest_semaphores[candidate].initialized) {
            slot = candidate;
            break;
        }
    }
    if (!slot) {
        if (guest_semaphore_high_water >= kGuestSemaphoreCapacity) {
            pthread_mutex_unlock(&guest_semaphore_table_lock);
            fprintf(stderr, "compat32: MP semaphore table exhausted\n");
            return 0;
        }
        slot = guest_semaphore_high_water++;
    }
    struct guest_semaphore *semaphore = &guest_semaphores[slot];
    if (!semaphore->constructed) {
        pthread_mutex_init(&semaphore->mutex, NULL);
        pthread_cond_init(&semaphore->condition, NULL);
        semaphore->constructed = true;
    }
    pthread_mutex_lock(&semaphore->mutex);
    ++semaphore->generation;
    if ((semaphore->generation & kGuestSemaphoreSlotMask) == 0) {
        ++semaphore->generation;
    }
    semaphore->maximum = maximum;
    semaphore->count = initial;
    semaphore->initialized = true;
    uint32_t handle = guest_semaphore_handle(slot, semaphore->generation);
    pthread_mutex_unlock(&semaphore->mutex);
    __atomic_fetch_add(&guest_semaphore_live_count, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&guest_semaphore_table_lock);
    return handle;
}
static _Thread_local uint32_t guest_call_depth;
static _Thread_local uint32_t guest_thread_slot = UINT32_MAX;
static pthread_mutex_t guest_stack_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t guest_stack_slots_in_use;
static pthread_key_t guest_stack_slot_key;
static pthread_once_t guest_stack_slot_key_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t guest_execution_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool guest_execution_lock_held;

enum {
    kGuestStdinHandle = 0x7f030000,
    kGuestStdoutHandle = 0x7f030010,
    kGuestStderrHandle = 0x7f030020,
    kGuestFileHandleBase = 0x7f030100,
    kGuestFileHandleStride = 16,
    kGuestFileCapacity = 1024,
    kGuestFileGenerationStride = kGuestFileCapacity * kGuestFileHandleStride,
    kGuestFileGenerationCount = 3,
};

struct guest_file_entry {
    uint32_t handle;
    FILE *file;
};

static struct guest_file_entry guest_files[kGuestFileCapacity];
static uint32_t guest_file_next_index;
static uint32_t guest_file_active_count;
static uint32_t guest_file_peak_count;
static uint64_t guest_file_exhaustion_count;
static pthread_mutex_t guest_file_lock = PTHREAD_MUTEX_INITIALIZER;

enum {
    kGuestDirectoryHandleBase = 0x7f070000,
    kGuestDirectoryHandleStride = 16,
    kGuestDirectoryCapacity = 256,
};

/* This title was built against Darwin's pre-ino64 i386 dirent ABI.  Its
   directory iterator reads d_name at byte offset 8 (confirmed at 0xd91cb),
   whereas the current x86_64 SDK puts it at byte offset 21. */
struct __attribute__((packed, aligned(4))) guest_dirent32 {
    uint32_t d_ino;
    uint16_t d_reclen;
    uint8_t d_type;
    uint8_t d_namlen;
    char d_name[256];
};

_Static_assert(offsetof(struct guest_dirent32, d_name) == 8,
               "legacy Darwin i386 dirent layout");

struct guest_directory_entry {
    uint32_t handle;
    DIR *directory;
    uint32_t guest_dirent;
};

static struct guest_directory_entry guest_directories[kGuestDirectoryCapacity];
static pthread_mutex_t guest_directory_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Guest pthread mutexes and condition variables are keyed by the address of
 * the guest's own (opaque, never dereferenced) object.  Host objects are
 * allocated individually so the lookup tables can grow without moving a
 * mutex that another thread is currently blocked on, and destroy calls
 * release the entry so a long campaign cannot exhaust the registry.  A host
 * object that is still busy at destroy time (the guest destroyed a locked
 * mutex or a waited-on condition, which is undefined for it as well) is kept
 * rather than freed underneath its waiters.
 */
struct guest_mutex_entry {
    uint32_t address;
    pthread_mutex_t *mutex;
};

struct guest_cond_entry {
    uint32_t address;
    pthread_cond_t *condition;
};

static struct guest_mutex_entry *guest_mutexes;
static struct guest_cond_entry *guest_conditions;
static uint32_t guest_mutex_count;
static uint32_t guest_mutex_capacity;
static uint32_t guest_cond_count;
static uint32_t guest_cond_capacity;
static uint64_t guest_sync_released_count;
static pthread_mutex_t guest_sync_table_lock = PTHREAD_MUTEX_INITIALIZER;
static char dynamic_symbol_names[kDynamicThunkCapacity][96];
static uint32_t dynamic_symbol_count;

/*
 * Cg is still present in the game bundle as an x86_64 slice.  Cg 3.0 does not
 * hand out pointers for CGcontext/CGprogram/CGparameter: every handle is a
 * small integer allocated from a monotonically increasing per-process counter
 * (the bundled runtime returns 0x1 for the first context, 0x9 for the first
 * program, and roughly eighteen values per compiled program).  Those values
 * are passed to the guest unchanged, exactly as the original i386 Cg did.
 *
 * The previous design interned every handle in a fixed 4,096-entry table and
 * silently returned NULL once it filled.  A single session compiles several
 * hundred programs, so the table saturated part way through the campaign and
 * from then on every newly compiled material either lost its program
 * (cgCreateProgram == NULL) or its parameters (cgGetFirstParameter == NULL).
 * The renderer then drew those materials with whichever ARB program and
 * constants happened to be bound last, producing the flat saturated skies,
 * oceans, cards, and stretched props that appeared in whichever level was
 * loaded latest.
 *
 * A growable table is retained purely as a safety net for the theoretical
 * case of a Cg build that returns real pointers above the i386 range.
 */
enum {
    kGuestCgHandleBase = 0x7f040000,
    kGuestCgHandleStride = 16,
};

static void *cg_library;
static void **cg_objects;
static uint32_t cg_object_count;
static uint32_t cg_object_capacity;
static bool cg_object_table_reported;
static pthread_mutex_t cg_object_lock = PTHREAD_MUTEX_INITIALIZER;

/* Guest copies of strings owned by the Cg runtime.  Cg interns parameter
   names, so identical host pointers are returned for the life of the process;
   the guest copy is keyed by that pointer and refreshed only when the content
   changes (for example a recycled compiled-program buffer).  This keeps the
   guest heap bounded instead of allocating a new copy on every call. */
struct cg_string_entry {
    const char *host;
    uint32_t owner;
    uint32_t guest;
    uint32_t capacity;
};

static struct cg_string_entry *cg_strings;
static uint32_t cg_string_count;
static uint32_t cg_string_capacity;
static pthread_mutex_t cg_string_lock = PTHREAD_MUTEX_INITIALIZER;

/* Session counters reported at exit.  A non-zero null count for programs or
   first parameters is exactly the symptom that used to produce flat
   saturated materials, so it is the number to check after a long session. */
static uint32_t cg_programs_created;
static uint32_t cg_programs_destroyed;
static uint32_t cg_null_programs;
static uint32_t cg_null_first_parameters;

static void cg_report(const char *reason)
{
    if (!cg_library) return;
    fprintf(stderr,
            "compat32: Cg %s: programs created=%" PRIu32 " destroyed=%" PRIu32
            " null-programs=%" PRIu32 " null-first-parameters=%" PRIu32
            " pointer-handles=%" PRIu32 " guest-strings=%" PRIu32 "\n",
            reason, __atomic_load_n(&cg_programs_created, __ATOMIC_RELAXED),
            __atomic_load_n(&cg_programs_destroyed, __ATOMIC_RELAXED),
            __atomic_load_n(&cg_null_programs, __ATOMIC_RELAXED),
            __atomic_load_n(&cg_null_first_parameters, __ATOMIC_RELAXED),
            __atomic_load_n(&cg_object_count, __ATOMIC_RELAXED),
            __atomic_load_n(&cg_string_count, __ATOMIC_RELAXED));
}

struct guest_thread_context {
    uint32_t function;
    uint32_t argument;
};

static uint32_t guest_allocate(size_t size, bool clear);
static uint32_t guest_allocate_at(size_t size, bool clear, uint32_t site);
static bool guest_deallocate(uint32_t pointer);

/* guest_file_lock must be held while the returned FILE is in use.  This is
   important even for reads: another guest thread may close the same opaque
   handle concurrently, and libc FILE objects are invalid immediately after
   fclose. */
static uint32_t guest_sF_address;
static FILE *host_file_for_guest_locked(uint32_t handle)
{
    if (guest_sF_address && handle == guest_sF_address) return stdin;
    if (guest_sF_address && handle == guest_sF_address + 88) return stdout;
    if (guest_sF_address && handle == guest_sF_address + 176) return stderr;
    if (handle == kGuestStdinHandle) return stdin;
    if (handle == kGuestStdoutHandle) return stdout;
    if (handle == kGuestStderrHandle) return stderr;
    if (handle < kGuestFileHandleBase) return NULL;
    uint32_t offset = handle - kGuestFileHandleBase;
    uint32_t slot_offset = offset % kGuestFileGenerationStride;
    if (slot_offset % kGuestFileHandleStride) return NULL;
    uint32_t index = slot_offset / kGuestFileHandleStride;
    if (index >= kGuestFileCapacity) return NULL;
    struct guest_file_entry *entry = &guest_files[index];
    return entry->file && entry->handle == handle ? entry->file : NULL;
}

static FILE *lock_host_file_for_guest(uint32_t handle)
{
    pthread_mutex_lock(&guest_file_lock);
    FILE *file = host_file_for_guest_locked(handle);
    if (!file) {
        pthread_mutex_unlock(&guest_file_lock);
        errno = EBADF;
    }
    return file;
}

static void unlock_host_file(void)
{
    pthread_mutex_unlock(&guest_file_lock);
}

static uint32_t guest_handle_for_file(FILE *file)
{
    if (!file) return 0;
    pthread_mutex_lock(&guest_file_lock);
    for (uint32_t offset = 0; offset < kGuestFileCapacity; ++offset) {
        uint32_t index = (guest_file_next_index + offset) % kGuestFileCapacity;
        struct guest_file_entry *entry = &guest_files[index];
        if (entry->file) continue;

        /* Keep every opaque FILE value 16-byte aligned, as the original table
           did, while rotating through three non-overlapping generations below
           the Cg handle range.  Round-robin slot selection therefore rejects
           a recently closed stale handle for at least 3,072 subsequent opens. */
        uint32_t generation = 0;
        if (entry->handle) {
            generation = ((entry->handle - kGuestFileHandleBase) /
                          kGuestFileGenerationStride + 1) %
                         kGuestFileGenerationCount;
        }
        uint32_t handle = kGuestFileHandleBase +
                          generation * kGuestFileGenerationStride +
                          index * kGuestFileHandleStride;
        entry->handle = handle;
        entry->file = file;
        guest_file_next_index = (index + 1) % kGuestFileCapacity;
        ++guest_file_active_count;
        if (guest_file_active_count > guest_file_peak_count) {
            guest_file_peak_count = guest_file_active_count;
        }
        pthread_mutex_unlock(&guest_file_lock);
        return handle;
    }

    ++guest_file_exhaustion_count;
    uint64_t exhaustion = guest_file_exhaustion_count;
    uint32_t active = guest_file_active_count;
    pthread_mutex_unlock(&guest_file_lock);

    /* Ownership transfers to this function even on exhaustion.  The former
       implementation leaked this FILE and its descriptor whenever its
       monotonic 1,024-entry table filled. */
    fclose(file);
    errno = EMFILE;
    if (exhaustion == 1 || getenv("LP32_TRACE_FILES")) {
        fprintf(stderr,
                "compat32: guest FILE table exhausted active=%" PRIu32
                " capacity=%u occurrence=%" PRIu64 "\n",
                active, kGuestFileCapacity, exhaustion);
    }
    return 0;
}

static int guest_close_file(uint32_t handle)
{
    pthread_mutex_lock(&guest_file_lock);
    FILE *file = host_file_for_guest_locked(handle);
    if (!file) {
        pthread_mutex_unlock(&guest_file_lock);
        errno = EBADF;
        return EOF;
    }

    if (handle != kGuestStdinHandle && handle != kGuestStdoutHandle &&
        handle != kGuestStderrHandle) {
        uint32_t index = ((handle - kGuestFileHandleBase) %
                          kGuestFileGenerationStride) /
                         kGuestFileHandleStride;
        guest_files[index].file = NULL;
        if (guest_file_active_count) --guest_file_active_count;
    }
    int result = fclose(file);
    pthread_mutex_unlock(&guest_file_lock);
    return result;
}

static uint32_t guest_handle_for_directory(DIR *directory)
{
    if (!directory) return 0;
    pthread_mutex_lock(&guest_directory_lock);
    for (uint32_t index = 0; index < kGuestDirectoryCapacity; ++index) {
        struct guest_directory_entry *entry = &guest_directories[index];
        if (entry->directory) continue;
        if (!entry->guest_dirent) {
            entry->guest_dirent = guest_allocate(sizeof(struct guest_dirent32), true);
            if (!entry->guest_dirent) break;
        }
        entry->handle = kGuestDirectoryHandleBase +
                        index * kGuestDirectoryHandleStride;
        entry->directory = directory;
        uint32_t handle = entry->handle;
        pthread_mutex_unlock(&guest_directory_lock);
        return handle;
    }
    pthread_mutex_unlock(&guest_directory_lock);
    closedir(directory);
    errno = EMFILE;
    return 0;
}

static struct guest_directory_entry *guest_directory_for_handle(uint32_t handle)
{
    if (handle < kGuestDirectoryHandleBase) return NULL;
    uint32_t offset = handle - kGuestDirectoryHandleBase;
    if (offset % kGuestDirectoryHandleStride) return NULL;
    uint32_t index = offset / kGuestDirectoryHandleStride;
    if (index >= kGuestDirectoryCapacity) return NULL;
    struct guest_directory_entry *entry = &guest_directories[index];
    return entry->directory && entry->handle == handle ? entry : NULL;
}

static uint32_t guest_standard_file_import(const char *name)
{
    if (strcmp(name, "___stdinp") == 0) return kGuestStdinHandle;
    if (strcmp(name, "___stdoutp") == 0) return kGuestStdoutHandle;
    if (strcmp(name, "___stderrp") == 0) return kGuestStderrHandle;
    return 0;
}

/* Released host objects are recycled rather than freed so that a guest
   thread racing a destroy with a lock (undefined for the guest as well) can
   never dereference unmapped host memory. */
struct host_sync_free_node {
    struct host_sync_free_node *next;
};

static struct host_sync_free_node *free_host_mutexes;
static struct host_sync_free_node *free_host_conditions;

static int initialize_host_mutex(pthread_mutex_t *mutex)
{
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    int status = pthread_mutex_init(mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    return status;
}

static pthread_mutex_t *create_host_mutex(void)
{
    pthread_mutex_t *mutex;
    if (free_host_mutexes) {
        struct host_sync_free_node *node = free_host_mutexes;
        free_host_mutexes = node->next;
        mutex = (pthread_mutex_t *)node;
    } else {
        mutex = malloc(sizeof(*mutex) > sizeof(struct host_sync_free_node) ?
                       sizeof(*mutex) : sizeof(struct host_sync_free_node));
        if (!mutex) return NULL;
    }
    if (initialize_host_mutex(mutex) != 0) {
        free(mutex);
        return NULL;
    }
    return mutex;
}

static void recycle_host_mutex(pthread_mutex_t *mutex)
{
    struct host_sync_free_node *node = (struct host_sync_free_node *)mutex;
    node->next = free_host_mutexes;
    free_host_mutexes = node;
}

static pthread_cond_t *create_host_condition(void)
{
    pthread_cond_t *condition;
    if (free_host_conditions) {
        struct host_sync_free_node *node = free_host_conditions;
        free_host_conditions = node->next;
        condition = (pthread_cond_t *)node;
    } else {
        condition = malloc(sizeof(*condition) >
                           sizeof(struct host_sync_free_node) ?
                           sizeof(*condition) :
                           sizeof(struct host_sync_free_node));
        if (!condition) return NULL;
    }
    if (pthread_cond_init(condition, NULL) != 0) {
        free(condition);
        return NULL;
    }
    return condition;
}

static void recycle_host_condition(pthread_cond_t *condition)
{
    struct host_sync_free_node *node = (struct host_sync_free_node *)condition;
    node->next = free_host_conditions;
    free_host_conditions = node;
}

/* Direct-mapped guess at a guest mutex's table index.  A miss (stale after a
   destroy compacted the table) just falls back to the scan, so entries are
   never invalidated. */
enum { kGuestMutexIndexCacheSize = 1024 };
static uint32_t guest_mutex_index_cache[kGuestMutexIndexCacheSize];

static pthread_mutex_t *host_mutex_for_guest(uint32_t address, bool create)
{
    uint32_t *cached = &guest_mutex_index_cache[
        (address >> 3) % kGuestMutexIndexCacheSize];
    pthread_mutex_lock(&guest_sync_table_lock);
    if (*cached < guest_mutex_count && guest_mutexes[*cached].address == address) {
        pthread_mutex_t *mutex = guest_mutexes[*cached].mutex;
        pthread_mutex_unlock(&guest_sync_table_lock);
        return mutex;
    }
    for (uint32_t index = 0; index < guest_mutex_count; ++index) {
        if (guest_mutexes[index].address == address) {
            pthread_mutex_t *mutex = guest_mutexes[index].mutex;
            *cached = index;
            pthread_mutex_unlock(&guest_sync_table_lock);
            return mutex;
        }
    }
    if (!create) {
        pthread_mutex_unlock(&guest_sync_table_lock);
        return NULL;
    }
    if (guest_mutex_count == guest_mutex_capacity) {
        uint32_t grown = guest_mutex_capacity ? guest_mutex_capacity * 2 : 1024;
        struct guest_mutex_entry *table =
            realloc(guest_mutexes, (size_t)grown * sizeof(*table));
        if (!table) {
            pthread_mutex_unlock(&guest_sync_table_lock);
            return NULL;
        }
        guest_mutexes = table;
        guest_mutex_capacity = grown;
    }
    pthread_mutex_t *mutex = create_host_mutex();
    if (!mutex) {
        pthread_mutex_unlock(&guest_sync_table_lock);
        return NULL;
    }
    *cached = guest_mutex_count;
    struct guest_mutex_entry *entry = &guest_mutexes[guest_mutex_count++];
    entry->address = address;
    entry->mutex = mutex;
    pthread_mutex_unlock(&guest_sync_table_lock);
    return mutex;
}

/* pthread_mutex_init on an address that already has a host mutex means the
   guest recycled that memory (or re-initialized a static).  Replace the host
   object so no stale lock state leaks into the new mutex; if the old one is
   still held it is unrecoverable for the guest too, so keep it instead. */
static uint32_t guest_mutex_initialize(uint32_t address)
{
    pthread_mutex_lock(&guest_sync_table_lock);
    for (uint32_t index = 0; index < guest_mutex_count; ++index) {
        struct guest_mutex_entry *entry = &guest_mutexes[index];
        if (entry->address != address) continue;
        if (pthread_mutex_destroy(entry->mutex) == 0) {
            initialize_host_mutex(entry->mutex);
        }
        pthread_mutex_unlock(&guest_sync_table_lock);
        return 0;
    }
    pthread_mutex_unlock(&guest_sync_table_lock);
    return host_mutex_for_guest(address, true) ? 0 : (uint32_t)ENOMEM;
}

static uint32_t guest_mutex_destroy(uint32_t address)
{
    pthread_mutex_lock(&guest_sync_table_lock);
    for (uint32_t index = 0; index < guest_mutex_count; ++index) {
        struct guest_mutex_entry *entry = &guest_mutexes[index];
        if (entry->address != address) continue;
        int status = pthread_mutex_destroy(entry->mutex);
        if (status == 0) {
            recycle_host_mutex(entry->mutex);
            *entry = guest_mutexes[--guest_mutex_count];
            ++guest_sync_released_count;
        }
        pthread_mutex_unlock(&guest_sync_table_lock);
        return (uint32_t)status;
    }
    pthread_mutex_unlock(&guest_sync_table_lock);
    /* Never created on the host: statically initialized but never locked. */
    return 0;
}

static pthread_cond_t *host_cond_for_guest(uint32_t address, bool create)
{
    pthread_mutex_lock(&guest_sync_table_lock);
    for (uint32_t index = 0; index < guest_cond_count; ++index) {
        if (guest_conditions[index].address == address) {
            pthread_cond_t *condition = guest_conditions[index].condition;
            pthread_mutex_unlock(&guest_sync_table_lock);
            return condition;
        }
    }
    if (!create) {
        pthread_mutex_unlock(&guest_sync_table_lock);
        return NULL;
    }
    if (guest_cond_count == guest_cond_capacity) {
        uint32_t grown = guest_cond_capacity ? guest_cond_capacity * 2 : 1024;
        struct guest_cond_entry *table =
            realloc(guest_conditions, (size_t)grown * sizeof(*table));
        if (!table) {
            pthread_mutex_unlock(&guest_sync_table_lock);
            return NULL;
        }
        guest_conditions = table;
        guest_cond_capacity = grown;
    }
    pthread_cond_t *condition = create_host_condition();
    if (!condition) {
        pthread_mutex_unlock(&guest_sync_table_lock);
        return NULL;
    }
    struct guest_cond_entry *entry = &guest_conditions[guest_cond_count++];
    entry->address = address;
    entry->condition = condition;
    pthread_mutex_unlock(&guest_sync_table_lock);
    return condition;
}

static uint32_t guest_cond_initialize(uint32_t address)
{
    pthread_mutex_lock(&guest_sync_table_lock);
    for (uint32_t index = 0; index < guest_cond_count; ++index) {
        struct guest_cond_entry *entry = &guest_conditions[index];
        if (entry->address != address) continue;
        pthread_cond_broadcast(entry->condition);
        if (pthread_cond_destroy(entry->condition) == 0) {
            pthread_cond_init(entry->condition, NULL);
        }
        pthread_mutex_unlock(&guest_sync_table_lock);
        return 0;
    }
    pthread_mutex_unlock(&guest_sync_table_lock);
    return host_cond_for_guest(address, true) ? 0 : (uint32_t)ENOMEM;
}

/* macOS reports success from pthread_cond_destroy even while a thread is
   blocked in pthread_cond_wait on the object, so a busy check cannot protect
   waiters.  Wake them first: a spurious wakeup is permitted by POSIX and the
   guest re-tests its predicate, whereas a waiter left on a recycled object
   would sleep forever. */
static uint32_t guest_cond_destroy(uint32_t address)
{
    pthread_mutex_lock(&guest_sync_table_lock);
    for (uint32_t index = 0; index < guest_cond_count; ++index) {
        struct guest_cond_entry *entry = &guest_conditions[index];
        if (entry->address != address) continue;
        pthread_cond_broadcast(entry->condition);
        int status = pthread_cond_destroy(entry->condition);
        if (status == 0) {
            recycle_host_condition(entry->condition);
            *entry = guest_conditions[--guest_cond_count];
            ++guest_sync_released_count;
        }
        pthread_mutex_unlock(&guest_sync_table_lock);
        return (uint32_t)status;
    }
    pthread_mutex_unlock(&guest_sync_table_lock);
    return 0;
}

static int runtime_error(const char *what)
{
    fprintf(stderr, "compat32: %s: %s\n", what, strerror(errno));
    return -1;
}

static bool process_is_translated(void)
{
    int translated = 0;
    size_t size = sizeof(translated);
    return sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) == 0 &&
           translated == 1;
}

static int allocate_cs32(void)
{
    ldt_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.code.limit00 = 0xffff;
    entry.code.type = DESC_CODE_READ;
    entry.code.dpl = 3;
    entry.code.present = 1;
    entry.code.limit16 = 0x0f;
    entry.code.opsz = DESC_CODE_32B;
    entry.code.granular = DESC_GRAN_PAGE;

    const int index = 32;
    if (i386_set_ldt(index, &entry, 1) < 0) return runtime_error("i386_set_ldt(cs32)");
    lp32_cs32 = (uint16_t)((index << 3) | 7);
    return 0;
}

static void emit_u32(uint8_t *destination, uint32_t value)
{
    memcpy(destination, &value, sizeof(value));
}

static void emit_u64(uint8_t *destination, uint64_t value)
{
    memcpy(destination, &value, sizeof(value));
}

static uint8_t *emit_bytes(uint8_t *destination, const uint8_t *bytes, size_t size)
{
    memcpy(destination, bytes, size);
    return destination + size;
}

/*
 * Mode guards.
 *
 * Every transition between i386 and x86_64 code is a far jump through a
 * selector.  Under Rosetta a thread that is in the middle of such a jump
 * while the translator flushes its code cache can resume on the far side in
 * its *old* mode: the 64-bit gateway pad is then decoded as i386 (the
 * observed crash: `movabs $gateway,%rax` becomes `dec eax; mov eax,imm32;
 * add [eax],eax`, a write into the game's text), and an i386 thunk can
 * likewise be decoded as x86_64.  A standalone stress test reproduces both
 * within seconds when another thread keeps Rosetta translating fresh code.
 *
 * Each pad therefore starts with a probe whose bytes are valid in both
 * modes but compute a different value: 0x40 is `inc eax` in i386 and a REX
 * prefix in x86_64, and 0x41 is `inc ecx` versus REX.B.  When the probe
 * shows the wrong mode the pad counts the event and redoes the far jump to
 * an alternate pad; the alternate pad traps if that fails too, so a stuck
 * thread never loops.  Registers used by the probes are scratch at every
 * site: eax at an import call, ecx at a return.
 */
static bool mode_guards_disabled;

/* i386 tail: lock incl counter; ljmp $cs64:retry (or int3 when retry==0). */
static uint8_t *emit_recovery32(uint8_t *destination, uint32_t counter,
                                uint32_t retry, uint16_t cs64)
{
    static const uint8_t lock_inc[] = {0xf0, 0xff, 0x05};
    uint8_t *p = emit_bytes(destination, lock_inc, sizeof(lock_inc));
    emit_u32(p, counter);
    p += 4;
    if (!retry) {
        *p++ = 0xcc;
        return p;
    }
    *p++ = 0xea;
    emit_u32(p, retry);
    p += 4;
    memcpy(p, &cs64, sizeof(cs64));
    return p + 2;
}

/* 64-bit pad reached from import thunks: jump to lp32_import_gateway. */
static void emit_gateway64_pad(uint8_t *destination, uint32_t retry,
                               uint16_t cs64)
{
    uint8_t *p = destination;
    if (!mode_guards_disabled) {
        /* xorl %eax,%eax; inc-eax-or-REX nop; testl %eax,%eax; jnz recover */
        static const uint8_t probe[] = {0x31, 0xc0, 0x40, 0x90,
                                        0x85, 0xc0, 0x75, 0x0c};
        p = emit_bytes(p, probe, sizeof(probe));
    }
    /* movabs $lp32_import_gateway,%rax; jmp *%rax */
    p[0] = 0x48;
    p[1] = 0xb8;
    emit_u64(p + 2, (uint64_t)(uintptr_t)lp32_import_gateway);
    p[10] = 0xff;
    p[11] = 0xe0;
    p += 12;
    if (!mode_guards_disabled) {
        emit_recovery32(p, retry ? kWrongModeTo64Counter : kModeGuardFailedCounter,
                        retry, cs64);
    }
}

/* 64-bit pad reached from the exit thunk: restore the host ABI frame
   established by run_compat32 and return to it with eax untouched. */
static void emit_return64_pad(uint8_t *destination, uint32_t retry,
                              uint16_t cs64)
{
    static const uint8_t return64[] = {
        0x4c, 0x87, 0xf4,
        0x48, 0x83, 0xc4, 0x18,
        0x41, 0x5f,
        0x41, 0x5e,
        0x41, 0x5d,
        0x41, 0x5c,
        0x5b,
        0x5d,
        0xc3,
    };
    uint8_t *p = destination;
    if (!mode_guards_disabled) {
        /* xorl %ecx,%ecx; inc-ecx-twice-or-inc-r9d; testl %ecx,%ecx; jnz */
        static const uint8_t probe[] = {0x31, 0xc9, 0x41, 0xff, 0xc1,
                                        0x85, 0xc9, 0x75, 0x12};
        p = emit_bytes(p, probe, sizeof(probe));
    }
    p = emit_bytes(p, return64, sizeof(return64));
    if (!mode_guards_disabled) {
        emit_recovery32(p, retry ? kWrongModeTo64Counter : kModeGuardFailedCounter,
                        retry, cs64);
    }
}

/* i386 landing trampoline: the 64-bit side leaves the i386 destination on
   top of the guest stack and far-jumps here; `ret` pops it.  Decoded as
   x86_64 (wrong mode) the probe falls into the recovery tail instead, which
   counts the event, redirects the far pointer still addressed by r14 at the
   alternate trampoline and redoes the jump. */
static void emit_landing32(uint8_t *destination, uint32_t retry)
{
    static const uint8_t probe[] = {0x31, 0xc9, 0x41, 0xff, 0xc1,
                                    0x85, 0xc9, 0x74, 0x01, 0xc3};
    uint8_t *p = emit_bytes(destination, probe, sizeof(probe));
    static const uint8_t lock_inc_abs[] = {0xf0, 0xff, 0x04, 0x25};
    p = emit_bytes(p, lock_inc_abs, sizeof(lock_inc_abs));
    emit_u32(p, retry ? kWrongModeTo32Counter : kModeGuardFailedCounter);
    p += 4;
    if (!retry) {
        *p = 0xcc;
        return;
    }
    static const uint8_t store_r14[] = {0x41, 0xc7, 0x06};
    p = emit_bytes(p, store_r14, sizeof(store_r14));
    emit_u32(p, retry);
    p += 4;
    static const uint8_t ljmp_r14[] = {0x41, 0xff, 0x2e};
    emit_bytes(p, ljmp_r14, sizeof(ljmp_r14));
}

void compat_runtime32_mode_guard_counts(uint32_t *recovered_to64,
                                        uint32_t *recovered_to32)
{
    *recovered_to64 = *(volatile uint32_t *)(uintptr_t)kWrongModeTo64Counter;
    *recovered_to32 = *(volatile uint32_t *)(uintptr_t)kWrongModeTo32Counter;
}

static void (*diagnostic_sink)(const char *line);

void compat_runtime32_set_diagnostic_sink(void (*sink)(const char *line))
{
    diagnostic_sink = sink;
}

static int game_config_missing;

int compat_runtime32_game_config_missing(void)
{
    return game_config_missing;
}

static bool path_is_game_config(const char *path)
{
    static const char suffix[] = "/pcconfig.txt";
    size_t length = strlen(path);
    return length >= sizeof(suffix) - 1 &&
           strcasecmp(path + length - (sizeof(suffix) - 1), suffix) == 0;
}

/* Called once per presented frame: report newly recovered wrong-mode
   landings so the persistent run log shows how often Rosetta did this. */
void compat_runtime32_check_mode_guards(uint64_t swap_count)
{
    static uint32_t reported_to64, reported_to32;
    uint32_t to64, to32;
    compat_runtime32_mode_guard_counts(&to64, &to32);
    if (to64 == reported_to64 && to32 == reported_to32) return;
    char line[160];
    snprintf(line, sizeof(line),
             "compat32: recovered wrong-mode landing(s) at swap %llu: "
             "to64=%u (+%u) to32=%u (+%u)\n",
             (unsigned long long)swap_count, to64, to64 - reported_to64,
             to32, to32 - reported_to32);
    reported_to64 = to64;
    reported_to32 = to32;
    if (diagnostic_sink) {
        diagnostic_sink(line);
    } else {
        fputs(line, stderr);
    }
}

/*
 * Test aid (LP32_TEST_CODE_CHURN=<mib>): keep generating and executing fresh
 * x86_64 code so Rosetta has to translate continuously and flush its cache
 * every few seconds.  This is what makes wrong-mode landings frequent enough
 * to observe; without guards the game dies within seconds under it.
 */
static void *code_churn_thread(void *opaque)
{
    size_t size = (size_t)(uintptr_t)opaque << 20;
    for (;;) {
        uint8_t *block = mmap(NULL, size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
        if (block == MAP_FAILED) return NULL;
        for (size_t index = 0; index < size / 16; ++index) {
            uint8_t *function = block + index * 16;
            function[0] = 0xb8;                 /* movl $index,%eax */
            emit_u32(function + 1, (uint32_t)index);
            function[5] = 0xc3;                 /* ret */
            memset(function + 6, 0xcc, 10);
        }
        if (mprotect(block, size, PROT_READ | PROT_EXEC) != 0) return NULL;
        volatile uint32_t sink = 0;
        for (size_t index = 0; index < size / 16; ++index) {
            sink += ((uint32_t (*)(void))(block + index * 16))();
        }
        munmap(block, size);
    }
}

static void start_code_churn_if_requested(void)
{
    const char *text = getenv("LP32_TEST_CODE_CHURN");
    if (!text || !text[0]) return;
    unsigned mib = (unsigned)strtoul(text, NULL, 0);
    if (!mib) mib = 8;
    pthread_t thread;
    if (pthread_create(&thread, NULL, code_churn_thread,
                       (void *)(uintptr_t)mib) == 0) {
        pthread_detach(thread);
        fprintf(stderr, "compat32: test code churn active (%u MiB cycles)\n", mib);
    }
}

static int build_transition_bridge(void)
{
    uint8_t *code = (void *)(uintptr_t)kBridgeCodeBase;
    memset(code, 0xcc, kBridgeCodeSize);
    mode_guards_disabled = getenv("LP32_NO_MODE_GUARDS") != NULL;
    if (mode_guards_disabled) {
        fprintf(stderr, "compat32: mode guards disabled by LP32_NO_MODE_GUARDS\n");
    }
    start_code_churn_if_requested();

    uint16_t cs64 = 0;
    __asm__ volatile("movw %%cs, %0" : "=r"(cs64));

    /* i386 exit thunk: ljmp *exit_far_pointer */
    code[kExitThunkOffset + 0] = 0xff;
    code[kExitThunkOffset + 1] = 0x2d;
    emit_u32(code + kExitThunkOffset + 2,
             kBridgeCodeBase + kExitFarPointerOffset);

    const struct far_ptr32 exit_far = {
        .offset = kBridgeCodeBase + kReturn64Offset,
        .selector = cs64,
    };
    memcpy(code + kExitFarPointerOffset, &exit_far, sizeof(exit_far));

    emit_return64_pad(code + kReturn64Offset,
                      kBridgeCodeBase + kReturn64AltOffset, cs64);
    emit_return64_pad(code + kReturn64AltOffset, 0, cs64);

    const struct far_ptr32 gateway_far = {
        .offset = kBridgeCodeBase + kGateway64Offset,
        .selector = cs64,
    };
    memcpy(code + kGatewayFarPointerOffset, &gateway_far, sizeof(gateway_far));

    emit_gateway64_pad(code + kGateway64Offset,
                       kBridgeCodeBase + kGateway64AltOffset, cs64);
    emit_gateway64_pad(code + kGateway64AltOffset, 0, cs64);

    emit_landing32(code + kLanding32Offset, kBridgeCodeBase + kLanding32AltOffset);
    emit_landing32(code + kLanding32AltOffset, 0);

    /* Bejeweled 3 places its symbol stubs in __TEXT,__symbol_stub, which is
       read-only after mapping; Peggle's live in the rwx __IMPORT segment.
       Make each stub's page writable+executable before patching (a no-op when
       it is already writable).  A one-page cache keeps this to a few mprotect
       calls since the stubs are contiguous. */
    size_t page_size = (size_t)getpagesize();
    uintptr_t protected_page = 0;
    for (uint32_t index = 0; index < current_image->import_count; ++index) {
        if (current_image->imports[index].kind != MACHO_IMPORT32_STUB) continue;
        uint8_t *thunk = code + kImportThunksOffset + index * kImportThunkSize;
        /* push $import_index; ljmp *gateway_far_pointer */
        thunk[0] = 0x68;
        emit_u32(thunk + 1, index);
        thunk[5] = 0xff;
        thunk[6] = 0x2d;
        emit_u32(thunk + 7, kBridgeCodeBase + kGatewayFarPointerOffset);

        uint8_t *stub = (void *)(uintptr_t)current_image->imports[index].address;
        int64_t displacement = (int64_t)(uintptr_t)thunk -
                               ((int64_t)(uintptr_t)stub + 5);
        if (displacement < INT32_MIN || displacement > INT32_MAX) {
            errno = ERANGE;
            return runtime_error("i386 import thunk displacement");
        }
        /* The stub is 5 bytes; protect the page it starts on plus the next so
           a stub straddling a page boundary is fully writable. */
        uintptr_t page = (uintptr_t)stub & ~(uintptr_t)(page_size - 1);
        if (page != protected_page) {
            if (mprotect((void *)page, page_size * 2,
                         PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
                return runtime_error("mprotect import stub page");
            }
            protected_page = page;
        }
        stub[0] = 0xe9;
        emit_u32(stub + 1, (uint32_t)(int32_t)displacement);
    }

    for (uint32_t index = 0; index < kDynamicThunkCapacity; ++index) {
        uint8_t *thunk = code + kDynamicThunksOffset + index * kImportThunkSize;
        thunk[0] = 0x68;
        emit_u32(thunk + 1, UINT32_C(0x80000000) | index);
        thunk[5] = 0xff;
        thunk[6] = 0x2d;
        emit_u32(thunk + 7, kBridgeCodeBase + kGatewayFarPointerOffset);
    }

    uint32_t *data_cell = (void *)(uintptr_t)kBridgeDataBase;
    size_t data_cells = 0;
    memset(data_cell, 0, kBridgeDataSize);
    for (uint32_t index = 0; index < current_image->import_count; ++index) {
        if (current_image->imports[index].kind != MACHO_IMPORT32_POINTER) continue;
        if ((data_cells + 4) * sizeof(*data_cell) > kModeGuardCountersOffset) {
            errno = ENOSPC;
            return runtime_error("i386 import data cells");
        }
        uint32_t cell_address = kBridgeDataBase +
                                (uint32_t)(data_cells * sizeof(*data_cell));
        uint32_t *slot = (void *)(uintptr_t)current_image->imports[index].address;
        *slot = cell_address;
        for (uint32_t function = 0; function < current_image->import_count; ++function) {
            if (current_image->imports[function].kind == MACHO_IMPORT32_STUB &&
                !strcmp(current_image->imports[index].name, current_image->imports[function].name)) {
                *slot = kBridgeCodeBase + kImportThunksOffset + function * kImportThunkSize;
                break;
            }
        }
        if (strcmp(current_image->imports[index].name, "_errno") == 0) {
            guest_errno_address = cell_address;
        }
        if (strcmp(current_image->imports[index].name, "_mach_task_self_") == 0) {
            data_cell[data_cells] = mach_task_self();
        }
        uint32_t objc_pointer =
            objc_bridge32_pointer_import(current_image->imports[index].name);
        if (objc_pointer) data_cell[data_cells] = objc_pointer;
        uint32_t standard_file =
            guest_standard_file_import(current_image->imports[index].name);
        if (standard_file) data_cell[data_cells] = standard_file;
        if (!strcmp(current_image->imports[index].name, "__DefaultRuneLocale")) {
            /* The Sexy XMLParser classifies characters by reading
               _DefaultRuneLocale.__runetype[c] inline (i386 layout: the
               __runetype[256] array sits at struct offset 0x34, one 4-byte
               entry per rune).  The host x86_64 _RuneLocale uses 8-byte
               function pointers, so its __runetype array sits at a different
               offset; binding the guest symbol to it would make the inline
               read return garbage and the parser would reject ordinary
               whitespace as an "Illegal Character".  Synthesise an i386
               _RuneLocale here whose __runetype/__maplower/__mapupper tables
               are rebuilt from the host ctype predicates (the _CTYPE_* bit
               values are ABI-stable across architectures). */
            uint8_t *rune = (uint8_t *)(data_cell + data_cells);
            memcpy(rune, "RuneMagA", 8);
            uint32_t *runetype = (uint32_t *)(rune + 0x34);
            uint32_t *maplower = (uint32_t *)(rune + 0x34 + 256 * 4);
            uint32_t *mapupper = (uint32_t *)(rune + 0x34 + 256 * 8);
            for (int c = 0; c < 256; ++c) {
                unsigned long t = 0;
                if (isupper(c))  t |= _CTYPE_U;
                if (islower(c))  t |= _CTYPE_L;
                if (isdigit(c))  t |= _CTYPE_D;
                if (isspace(c))  t |= _CTYPE_S;
                if (ispunct(c))  t |= _CTYPE_P;
                if (iscntrl(c))  t |= _CTYPE_C;
                if (isxdigit(c)) t |= _CTYPE_X;
                if (isblank(c))  t |= _CTYPE_B;
                if (isalpha(c))  t |= _CTYPE_A;
                if (isgraph(c))  t |= _CTYPE_G;
                if (isprint(c))  t |= _CTYPE_R;
                runetype[c] = (uint32_t)t;
                maplower[c] = (uint32_t)(isupper(c) ? tolower(c) : c);
                mapupper[c] = (uint32_t)(islower(c) ? toupper(c) : c);
            }
            /* header + __runetype/__maplower/__mapupper + the three empty
               _RuneRange extents and trailing __variable fields (all left
               zero by the region-wide memset, i.e. no extended ranges). */
            data_cells += (0x34 + 256 * 4 * 3 + 64) / 4;
        } else if (!strcmp(current_image->imports[index].name,"___sF")) {
            guest_sF_address = cell_address;
            data_cells += 68;
        } else data_cells += 4;
    }

    if (mprotect(code, kBridgeCodeSize, PROT_READ | PROT_EXEC) != 0) {
        return runtime_error("mprotect transition bridge");
    }
    return 0;
}

static uint32_t guest_thunk_for_dynamic_symbol(const char *name)
{
    for (uint32_t index = 0; index < dynamic_symbol_count; ++index) {
        if (strcmp(dynamic_symbol_names[index], name) == 0) {
            return kBridgeCodeBase + kDynamicThunksOffset + index * kImportThunkSize;
        }
    }
    if (dynamic_symbol_count >= kDynamicThunkCapacity || strlen(name) >= 96) return 0;
    uint32_t index = dynamic_symbol_count++;
    strcpy(dynamic_symbol_names[index], name);
    return kBridgeCodeBase + kDynamicThunksOffset + index * kImportThunkSize;
}

uint32_t compat_runtime32_guest_callback(const char *name)
{
    return guest_thunk_for_dynamic_symbol(name);
}

static bool ensure_cg_library(void)
{
    if (cg_library) return true;
    char executable_path[PATH_MAX];
    uint32_t executable_path_size = sizeof(executable_path);
    if (_NSGetExecutablePath(executable_path, &executable_path_size) != 0) {
        fprintf(stderr, "compat32: executable path is too long for Cg lookup\n");
        return false;
    }
    char *last_slash = strrchr(executable_path, '/');
    if (!last_slash) return false;
    *last_slash = '\0';
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path),
                          "%s/../Library/Frameworks/Cg.framework/Cg",
                          executable_path);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        fprintf(stderr, "compat32: bundled Cg path is too long\n");
        return false;
    }
    cg_library = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!cg_library) {
        fprintf(stderr, "compat32: unable to load Cg framework: %s\n", dlerror());
        return false;
    }
    fprintf(stderr, "compat32: using bundled x86_64 Cg framework\n");
    return true;
}

static void *cg_symbol(const char *name)
{
    if (!ensure_cg_library()) return NULL;
    void *result = dlsym(cg_library, name);
    if (!result) fprintf(stderr, "compat32: Cg symbol %s is unavailable\n", name);
    return result;
}

static uint32_t guest_handle_for_cg_object(void *object)
{
    if (!object) return 0;
    uintptr_t value = (uintptr_t)object;
    if (value < kGuestCgHandleBase) return (uint32_t)value;

    pthread_mutex_lock(&cg_object_lock);
    if (!cg_object_table_reported) {
        fprintf(stderr,
                "compat32: Cg returned a pointer-sized handle %p; using the "
                "translation table for it\n", object);
        cg_object_table_reported = true;
    }
    for (uint32_t index = 0; index < cg_object_count; ++index) {
        if (cg_objects[index] == object) {
            pthread_mutex_unlock(&cg_object_lock);
            return kGuestCgHandleBase + index * kGuestCgHandleStride;
        }
    }
    if (cg_object_count == cg_object_capacity) {
        uint32_t grown = cg_object_capacity ? cg_object_capacity * 2 : 1024;
        void **table = realloc(cg_objects, (size_t)grown * sizeof(*table));
        if (!table) {
            pthread_mutex_unlock(&cg_object_lock);
            fprintf(stderr, "compat32: Cg handle table growth failed\n");
            return 0;
        }
        cg_objects = table;
        cg_object_capacity = grown;
    }
    uint32_t index = cg_object_count++;
    cg_objects[index] = object;
    pthread_mutex_unlock(&cg_object_lock);
    return kGuestCgHandleBase + index * kGuestCgHandleStride;
}

static void *cg_object_for_guest(uint32_t handle)
{
    if (!handle) return NULL;
    if (handle < kGuestCgHandleBase) return (void *)(uintptr_t)handle;
    uint32_t offset = handle - kGuestCgHandleBase;
    if (offset % kGuestCgHandleStride) return NULL;
    uint32_t index = offset / kGuestCgHandleStride;
    pthread_mutex_lock(&cg_object_lock);
    void *result = index < cg_object_count ? cg_objects[index] : NULL;
    pthread_mutex_unlock(&cg_object_lock);
    return result;
}

/* `owner` is the guest handle of the Cg program whose lifetime bounds the
   string (compiled listings), or 0 for strings Cg interns for the whole
   process (parameter names, semantics, profile strings). */
static uint32_t guest_cg_string_owned(uint32_t owner, const char *string)
{
    if (!string) return 0;
    size_t size = strlen(string) + 1;
    if (size > UINT32_MAX) return 0;

    pthread_mutex_lock(&cg_string_lock);
    struct cg_string_entry *entry = NULL;
    for (uint32_t index = 0; index < cg_string_count; ++index) {
        if (cg_strings[index].host == string &&
            cg_strings[index].owner == owner) {
            entry = &cg_strings[index];
            break;
        }
    }
    if (entry && entry->guest && entry->capacity >= size &&
        memcmp((const void *)(uintptr_t)entry->guest, string, size) == 0) {
        uint32_t guest = entry->guest;
        pthread_mutex_unlock(&cg_string_lock);
        return guest;
    }
    if (!entry) {
        if (cg_string_count == cg_string_capacity) {
            uint32_t grown = cg_string_capacity ? cg_string_capacity * 2 : 1024;
            struct cg_string_entry *table =
                realloc(cg_strings, (size_t)grown * sizeof(*table));
            if (!table) {
                pthread_mutex_unlock(&cg_string_lock);
                return compat_runtime32_copy_cstring(string);
            }
            cg_strings = table;
            cg_string_capacity = grown;
        }
        entry = &cg_strings[cg_string_count++];
        memset(entry, 0, sizeof(*entry));
        entry->host = string;
        entry->owner = owner;
    }
    if (entry->capacity < size) {
        uint32_t guest = compat_runtime32_reallocate(entry->guest, size);
        if (!guest) {
            pthread_mutex_unlock(&cg_string_lock);
            return 0;
        }
        entry->guest = guest;
        entry->capacity = (uint32_t)size;
    }
    memcpy((void *)(uintptr_t)entry->guest, string, size);
    uint32_t guest = entry->guest;
    pthread_mutex_unlock(&cg_string_lock);
    return guest;
}

static uint32_t guest_cg_string(const char *string)
{
    return guest_cg_string_owned(0, string);
}

/* Drop every guest string that belonged to a destroyed program.  Its host
   buffer is about to be freed and may be handed to the next program. */
static void release_cg_program_strings(uint32_t owner)
{
    if (!owner) return;
    pthread_mutex_lock(&cg_string_lock);
    for (uint32_t index = 0; index < cg_string_count;) {
        struct cg_string_entry *entry = &cg_strings[index];
        if (entry->owner != owner) {
            ++index;
            continue;
        }
        if (entry->guest) compat_runtime32_deallocate(entry->guest);
        *entry = cg_strings[--cg_string_count];
    }
    pthread_mutex_unlock(&cg_string_lock);
}

static uint32_t guest_copy_nullable_cstring(const char *string)
{
    return guest_cg_string(string);
}

uint32_t compat_runtime32_cg_object_count(void)
{
    pthread_mutex_lock(&cg_object_lock);
    uint32_t count = cg_object_count;
    pthread_mutex_unlock(&cg_object_lock);
    return count;
}

uint32_t compat_runtime32_cg_string_count(void)
{
    pthread_mutex_lock(&cg_string_lock);
    uint32_t count = cg_string_count;
    pthread_mutex_unlock(&cg_string_lock);
    return count;
}

static size_t guest_heap_capacity_for_size(size_t size)
{
    if (size == 0) size = 1;
    if (size > UINT32_MAX - 31) return 0;
    return (size + 31) & ~(size_t)15;
}

static unsigned guest_heap_bin_for_capacity(size_t capacity)
{
    if (capacity <= 512) {
        unsigned index = (unsigned)(capacity / 16);
        return index ? index - 1 : 0;
    }
    unsigned index = 32;
    size_t upper_bound = 1024;
    while (capacity > upper_bound && index + 1 < kGuestHeapBinCount) {
        upper_bound <<= 1;
        ++index;
    }
    return index;
}

static struct guest_heap_site_stats *guest_heap_site_locked(uint32_t site,
                                                             bool create)
{
    if (!site) site = kGuestHeapHostSite;
    for (unsigned index = 0; index < guest_heap_site_count; ++index) {
        if (guest_heap_sites[index].site == site) return &guest_heap_sites[index];
    }
    if (!create || guest_heap_site_count >= kGuestHeapSiteCapacity) return NULL;
    struct guest_heap_site_stats *entry =
        &guest_heap_sites[guest_heap_site_count++];
    memset(entry, 0, sizeof(*entry));
    entry->site = site;
    return entry;
}

static void guest_heap_record_allocation_locked(uint32_t site,
                                                 size_t requested,
                                                 size_t capacity)
{
    struct guest_heap_stats *stats = &guest_heap_statistics;
    ++stats->allocation_calls;
    ++stats->live_blocks;
    stats->live_requested += requested;
    stats->live_capacity += capacity;
    if (stats->live_requested > stats->peak_live_requested) {
        stats->peak_live_requested = stats->live_requested;
    }
    if (stats->live_capacity > stats->peak_live_capacity) {
        stats->peak_live_capacity = stats->live_capacity;
    }
    struct guest_heap_site_stats *entry = guest_heap_site_locked(site, true);
    if (entry) {
        ++entry->allocations;
        entry->allocated_bytes += requested;
        entry->live_bytes += requested;
        if (entry->live_bytes > entry->peak_live_bytes) {
            entry->peak_live_bytes = entry->live_bytes;
        }
    }
}

static void guest_heap_record_free_locked(uint32_t site, size_t requested,
                                           size_t capacity)
{
    struct guest_heap_stats *stats = &guest_heap_statistics;
    ++stats->valid_frees;
    if (stats->live_blocks) --stats->live_blocks;
    stats->live_requested = requested <= stats->live_requested ?
        stats->live_requested - requested : 0;
    stats->live_capacity = capacity <= stats->live_capacity ?
        stats->live_capacity - capacity : 0;
    struct guest_heap_site_stats *entry = guest_heap_site_locked(site, false);
    if (entry) {
        ++entry->frees;
        entry->freed_bytes += requested;
        entry->live_bytes = requested <= entry->live_bytes ?
            entry->live_bytes - requested : 0;
    }
}

static bool guest_heap_validate_header_locked(uint32_t pointer,
                                              uint32_t **metadata_out,
                                              bool *already_free)
{
    if (already_free) *already_free = false;
    if (pointer < kGuestHeapBase + kGuestHeapHeaderSize ||
        (pointer & 15) != 0 || pointer > guest_heap_cursor) {
        return false;
    }
    uintptr_t header_address = pointer - kGuestHeapHeaderSize;
    if (header_address < kGuestHeapBase ||
        header_address + kGuestHeapHeaderSize > guest_heap_cursor) {
        return false;
    }
    uint32_t *metadata = (void *)header_address;
    if (metadata[1] == kGuestHeapFreeMagic) {
        if (already_free) *already_free = true;
        return false;
    }
    if (metadata[1] != kGuestHeapLiveMagic) return false;
    uint32_t capacity = metadata[2];
    if (capacity < 32 || (capacity & 15) != 0 ||
        (uintptr_t)pointer + capacity > guest_heap_cursor ||
        metadata[0] > capacity) {
        return false;
    }
    if (metadata_out) *metadata_out = metadata;
    return true;
}

static void guest_heap_insert_free_locked(uint32_t header_address)
{
    uint32_t *metadata = (void *)(uintptr_t)header_address;
    unsigned bin = guest_heap_bin_for_capacity(metadata[2]);
    uint32_t *footer = (void *)(uintptr_t)(header_address +
        kGuestHeapHeaderSize + metadata[2] - 8);
    footer[0] = kGuestHeapFreeMagic;
    footer[1] = metadata[2];
    metadata[3] = guest_heap_free_bins[bin];
    guest_heap_free_bins[bin] = header_address;
    guest_heap_statistics.reusable_capacity += metadata[2];
}

static bool guest_heap_remove_free_locked(uint32_t header_address)
{
    const uint32_t *target = (const void *)(uintptr_t)header_address;
    if (target[1] != kGuestHeapFreeMagic || target[2] < 32 ||
        (target[2] & 15) != 0) {
        return false;
    }
    unsigned bin = guest_heap_bin_for_capacity(target[2]);
    uint32_t *link = &guest_heap_free_bins[bin];
    uint64_t maximum_nodes = guest_heap_statistics.valid_frees +
                             guest_heap_statistics.split_blocks + 1;
    for (uint64_t count = 0; *link && count < maximum_nodes; ++count) {
        uint32_t current = *link;
        uint32_t *metadata = (void *)(uintptr_t)current;
        if (current == header_address) {
            *link = metadata[3];
            guest_heap_statistics.reusable_capacity -= metadata[2];
            metadata[3] = 0;
            return true;
        }
        link = &metadata[3];
    }
    return false;
}

static uint32_t guest_heap_coalesce_free_locked(uint32_t header_address)
{
    uint32_t *metadata = (void *)(uintptr_t)header_address;
    uint32_t capacity = metadata[2];
    uintptr_t next_address = (uintptr_t)header_address +
                             kGuestHeapHeaderSize + capacity;
    if (next_address + kGuestHeapHeaderSize <= guest_heap_cursor) {
        uint32_t *next = (void *)next_address;
        uint32_t next_capacity = next[2];
        if (next[1] == kGuestHeapFreeMagic && next_capacity >= 32 &&
            (next_capacity & 15) == 0 &&
            next_address + kGuestHeapHeaderSize + next_capacity <=
                guest_heap_cursor &&
            guest_heap_remove_free_locked((uint32_t)next_address)) {
            capacity += kGuestHeapHeaderSize + next_capacity;
            metadata[2] = capacity;
        }
    }

    if (header_address >= kGuestHeapBase + kGuestHeapHeaderSize + 32) {
        const uint32_t *previous_footer =
            (const void *)(uintptr_t)(header_address - 8);
        uint32_t previous_capacity = previous_footer[1];
        if (previous_footer[0] == kGuestHeapFreeMagic &&
            previous_capacity >= 32 && (previous_capacity & 15) == 0 &&
            previous_capacity <= header_address - kGuestHeapBase -
                                 kGuestHeapHeaderSize) {
            uint32_t previous_address = header_address -
                kGuestHeapHeaderSize - previous_capacity;
            uint32_t *previous = (void *)(uintptr_t)previous_address;
            if (previous[1] == kGuestHeapFreeMagic &&
                previous[2] == previous_capacity &&
                guest_heap_remove_free_locked(previous_address)) {
                previous[2] = previous_capacity + kGuestHeapHeaderSize +
                              capacity;
                metadata = previous;
                header_address = previous_address;
                capacity = metadata[2];
            }
        }
    }

    if ((uintptr_t)header_address + kGuestHeapHeaderSize + capacity ==
        guest_heap_cursor) {
        guest_heap_cursor = header_address;
        return 0;
    }
    metadata[0] = 0;
    metadata[1] = kGuestHeapFreeMagic;
    metadata[3] = 0;
    return header_address;
}

static uint32_t guest_heap_take_free_locked(size_t needed_capacity)
{
    unsigned first_bin = guest_heap_bin_for_capacity(needed_capacity);
    for (unsigned bin = first_bin; bin < kGuestHeapBinCount; ++bin) {
        uint32_t *link = &guest_heap_free_bins[bin];
        uint64_t traversed = 0;
        while (*link && traversed++ < guest_heap_statistics.valid_frees +
                                      guest_heap_statistics.split_blocks + 1) {
            uint32_t header_address = *link;
            if (header_address < kGuestHeapBase ||
                (header_address & 15) != 0 ||
                (uintptr_t)header_address + kGuestHeapHeaderSize >
                    guest_heap_cursor) {
                fprintf(stderr,
                        "compat32: corrupt guest heap free-list address "
                        "0x%08" PRIx32 "\n", header_address);
                ++guest_heap_statistics.invalid_frees;
                *link = 0;
                break;
            }
            uint32_t *metadata = (void *)(uintptr_t)header_address;
            uint32_t next = metadata[3];
            size_t capacity = metadata[2];
            if (metadata[1] != kGuestHeapFreeMagic || capacity < 32 ||
                (capacity & 15) != 0 ||
                (uintptr_t)header_address + kGuestHeapHeaderSize + capacity >
                    guest_heap_cursor) {
                fprintf(stderr,
                        "compat32: corrupt guest heap free block "
                        "0x%08" PRIx32 "\n", header_address);
                ++guest_heap_statistics.invalid_frees;
                *link = next;
                continue;
            }
            if (capacity < needed_capacity) {
                link = &metadata[3];
                continue;
            }
            *link = next;
            guest_heap_statistics.reusable_capacity -= capacity;
            if (capacity >= needed_capacity + kGuestHeapHeaderSize + 32) {
                uint32_t remainder_address = header_address +
                    kGuestHeapHeaderSize + (uint32_t)needed_capacity;
                uint32_t *remainder = (void *)(uintptr_t)remainder_address;
                remainder[0] = 0;
                remainder[1] = kGuestHeapFreeMagic;
                remainder[2] = (uint32_t)(capacity - needed_capacity -
                                           kGuestHeapHeaderSize);
                remainder[3] = 0;
                metadata[2] = (uint32_t)needed_capacity;
                guest_heap_insert_free_locked(remainder_address);
                ++guest_heap_statistics.split_blocks;
            }
            return header_address;
        }
    }
    return 0;
}

static uint32_t guest_allocate_at(size_t size, bool clear, uint32_t site)
{
    if (size == 0) size = 1;
    size_t needed_capacity = guest_heap_capacity_for_size(size);
    if (!needed_capacity) return 0;

    pthread_mutex_lock(&guest_heap_lock);
    uint32_t header_address = guest_heap_reuse ?
        guest_heap_take_free_locked(needed_capacity) : 0;
    bool reused = header_address != 0;
    if (reused) {
        ++guest_heap_statistics.reused_allocations;
    } else {
        uintptr_t next = guest_heap_cursor + kGuestHeapHeaderSize +
                         needed_capacity;
        if (next < guest_heap_cursor || next > kGuestHeapEnd) {
            ++guest_heap_statistics.failed_allocations;
            fprintf(stderr,
                    "compat32: guest heap exhausted request=%zu cursor=0x%08" PRIxPTR
                    " end=0x%08x\n", size, guest_heap_cursor, kGuestHeapEnd);
            pthread_mutex_unlock(&guest_heap_lock);
            return 0;
        }
        header_address = (uint32_t)guest_heap_cursor;
        guest_heap_cursor = next;
        if (guest_heap_cursor > guest_heap_statistics.high_water) {
            guest_heap_statistics.high_water = guest_heap_cursor;
        }
        ++guest_heap_statistics.bump_allocations;
    }
    uint32_t *metadata = (void *)(uintptr_t)header_address;
    metadata[0] = (uint32_t)size;
    metadata[1] = kGuestHeapLiveMagic;
    if (!reused) metadata[2] = (uint32_t)needed_capacity;
    metadata[3] = site ? site : kGuestHeapHostSite;
    size_t capacity = metadata[2];
    guest_heap_record_allocation_locked(metadata[3], size, capacity);
    pthread_mutex_unlock(&guest_heap_lock);

    uint32_t result = header_address + kGuestHeapHeaderSize;
    if (clear) memset((void *)(uintptr_t)result, 0, capacity);
    return result;
}

static uint32_t guest_allocate(size_t size, bool clear)
{
    return guest_allocate_at(size, clear, kGuestHeapHostSite);
}

static bool guest_deallocate(uint32_t pointer)
{
    pthread_mutex_lock(&guest_heap_lock);
    ++guest_heap_statistics.free_calls;
    if (!pointer) {
        ++guest_heap_statistics.null_frees;
        pthread_mutex_unlock(&guest_heap_lock);
        return true;
    }
    uint32_t *metadata = NULL;
    bool already_free = false;
    if (!guest_heap_validate_header_locked(pointer, &metadata, &already_free)) {
        if (already_free) {
            ++guest_heap_statistics.double_frees;
        } else {
            ++guest_heap_statistics.invalid_frees;
        }
        uint64_t failures = guest_heap_statistics.invalid_frees +
                            guest_heap_statistics.double_frees;
        if (failures <= 32) {
            fprintf(stderr, "compat32: ignored %s guest free 0x%08" PRIx32
                    "\n", already_free ? "double" : "invalid", pointer);
        }
        pthread_mutex_unlock(&guest_heap_lock);
        return false;
    }
    uint32_t requested = metadata[0];
    uint32_t capacity = metadata[2];
    uint32_t site = metadata[3];
    guest_heap_record_free_locked(site, requested, capacity);
    metadata[0] = 0;
    metadata[1] = kGuestHeapFreeMagic;
    metadata[3] = 0;
    if (guest_heap_poison) {
        memset((void *)(uintptr_t)pointer, 0xdd, capacity);
    }
    if (guest_heap_reuse) {
        uint32_t free_header = guest_heap_coalesce_free_locked(
            (uint32_t)(uintptr_t)metadata);
        if (free_header) guest_heap_insert_free_locked(free_header);
    } else {
        guest_heap_statistics.abandoned_capacity += capacity;
    }
    pthread_mutex_unlock(&guest_heap_lock);
    return true;
}

static const char *guest_heap_site_label(uint32_t site)
{
    switch (site) {
        case kGuestHeapHostSite: return "host-bridge";
        case kGuestHeapStringSite: return "compat-string";
        case kGuestHeapRenderPoolSite: return "render-pool";
        default: return "guest-call";
    }
}

void compat_runtime32_heap_report(const char *reason)
{
    pthread_mutex_lock(&guest_heap_lock);
    const struct guest_heap_stats *stats = &guest_heap_statistics;
    fprintf(stderr,
            "compat32: guest heap [%s] reuse=%s alloc=%llu bump=%llu "
            "reused=%llu split=%llu free=%llu/%llu live=%llu "
            "requested=%.2fMiB capacity=%.2fMiB peak=%.2f/%.2fMiB "
            "reusable=%.2fMiB abandoned=%.2fMiB cursor=0x%08" PRIxPTR
            " high=0x%08" PRIxPTR " invalid=%llu double=%llu failed=%llu "
            "realloc=%llu(in-place=%llu moved=%llu)\n",
            reason ? reason : "report", guest_heap_reuse ? "on" : "off",
            (unsigned long long)stats->allocation_calls,
            (unsigned long long)stats->bump_allocations,
            (unsigned long long)stats->reused_allocations,
            (unsigned long long)stats->split_blocks,
            (unsigned long long)stats->valid_frees,
            (unsigned long long)stats->free_calls,
            (unsigned long long)stats->live_blocks,
            (double)stats->live_requested / (1024.0 * 1024.0),
            (double)stats->live_capacity / (1024.0 * 1024.0),
            (double)stats->peak_live_requested / (1024.0 * 1024.0),
            (double)stats->peak_live_capacity / (1024.0 * 1024.0),
            (double)stats->reusable_capacity / (1024.0 * 1024.0),
            (double)stats->abandoned_capacity / (1024.0 * 1024.0),
            guest_heap_cursor, stats->high_water,
            (unsigned long long)stats->invalid_frees,
            (unsigned long long)stats->double_frees,
            (unsigned long long)stats->failed_allocations,
            (unsigned long long)stats->realloc_calls,
            (unsigned long long)stats->realloc_in_place,
            (unsigned long long)stats->realloc_moved);

    if (getenv("LP32_HEAP_REPORT_SITES")) {
        bool selected[kGuestHeapSiteCapacity] = {false};
        for (unsigned rank = 0; rank < 12; ++rank) {
            unsigned best = UINT_MAX;
            for (unsigned index = 0; index < guest_heap_site_count; ++index) {
                if (selected[index] || !guest_heap_sites[index].live_bytes) {
                    continue;
                }
                if (best == UINT_MAX || guest_heap_sites[index].live_bytes >
                                        guest_heap_sites[best].live_bytes) {
                    best = index;
                }
            }
            if (best == UINT_MAX) break;
            selected[best] = true;
            const struct guest_heap_site_stats *entry = &guest_heap_sites[best];
            fprintf(stderr,
                    "compat32: guest heap site[%u] 0x%08" PRIx32
                    " %s live=%.2fMiB peak=%.2fMiB alloc/free=%llu/%llu\n",
                    rank + 1, entry->site, guest_heap_site_label(entry->site),
                    (double)entry->live_bytes / (1024.0 * 1024.0),
                    (double)entry->peak_live_bytes / (1024.0 * 1024.0),
                    (unsigned long long)entry->allocations,
                    (unsigned long long)entry->frees);
        }
    }
    pthread_mutex_unlock(&guest_heap_lock);
}

void compat_runtime32_heap_frame(uint64_t swap_count)
{
    if (!guest_heap_trace || !guest_heap_report_swap_interval) return;
    if (swap_count == 1 || swap_count % guest_heap_report_swap_interval == 0) {
        char reason[48];
        snprintf(reason, sizeof(reason), "swap-%llu",
                 (unsigned long long)swap_count);
        compat_runtime32_heap_report(reason);
        cg_report(reason);
    }
}

int lp32_apply_thread_return_state(void)
{
    /* Consumed here rather than cleared at every dispatch entry: only the
       few float-returning imports ever set it, and each thread-local access
       is a tlv_get_addr call on this ABI. */
    int kind = lp32_return_fp_kind;
    if (kind) {
        lp32_return_fp_kind = 0;
        if (kind == 1) {
            __asm__ volatile("flds %0" : : "m"(lp32_fp_result.f));
        } else {
            __asm__ volatile("fldl %0" : : "m"(lp32_fp_result.d));
        }
    }
    return lp32_leave_guest;
}

static uint32_t guest_reallocate(uint32_t old_pointer, size_t new_size,
                                 uint32_t site)
{
    pthread_mutex_lock(&guest_heap_lock);
    ++guest_heap_statistics.realloc_calls;
    pthread_mutex_unlock(&guest_heap_lock);
    if (!old_pointer) return guest_allocate_at(new_size, false, site);
    if (!new_size) {
        guest_deallocate(old_pointer);
        return 0;
    }

    size_t needed_capacity = guest_heap_capacity_for_size(new_size);
    if (!needed_capacity) return 0;
    pthread_mutex_lock(&guest_heap_lock);
    uint32_t *metadata = NULL;
    bool already_free = false;
    if (!guest_heap_validate_header_locked(old_pointer, &metadata,
                                           &already_free)) {
        ++guest_heap_statistics.invalid_frees;
        pthread_mutex_unlock(&guest_heap_lock);
        return 0;
    }
    uint32_t old_size = metadata[0];
    uint32_t old_capacity = metadata[2];
    uint32_t old_site = metadata[3];
    if (needed_capacity <= old_capacity) {
        metadata[0] = (uint32_t)new_size;
        if (new_size >= old_size) {
            guest_heap_statistics.live_requested += new_size - old_size;
        } else {
            guest_heap_statistics.live_requested -= old_size - new_size;
        }
        if (guest_heap_statistics.live_requested >
            guest_heap_statistics.peak_live_requested) {
            guest_heap_statistics.peak_live_requested =
                guest_heap_statistics.live_requested;
        }
        struct guest_heap_site_stats *entry =
            guest_heap_site_locked(old_site, false);
        if (entry) {
            if (new_size >= old_size) entry->live_bytes += new_size - old_size;
            else entry->live_bytes -= old_size - new_size;
            if (entry->live_bytes > entry->peak_live_bytes) {
                entry->peak_live_bytes = entry->live_bytes;
            }
        }
        ++guest_heap_statistics.realloc_in_place;
        pthread_mutex_unlock(&guest_heap_lock);
        return old_pointer;
    }
    uintptr_t old_end = (uintptr_t)old_pointer + old_capacity;
    if (old_end == guest_heap_cursor) {
        uintptr_t new_end = (uintptr_t)old_pointer + needed_capacity;
        if (new_end >= old_end && new_end <= kGuestHeapEnd) {
            guest_heap_cursor = new_end;
            if (guest_heap_cursor > guest_heap_statistics.high_water) {
                guest_heap_statistics.high_water = guest_heap_cursor;
            }
            metadata[0] = (uint32_t)new_size;
            metadata[2] = (uint32_t)needed_capacity;
            guest_heap_statistics.live_requested += new_size - old_size;
            guest_heap_statistics.live_capacity +=
                needed_capacity - old_capacity;
            if (guest_heap_statistics.live_requested >
                guest_heap_statistics.peak_live_requested) {
                guest_heap_statistics.peak_live_requested =
                    guest_heap_statistics.live_requested;
            }
            if (guest_heap_statistics.live_capacity >
                guest_heap_statistics.peak_live_capacity) {
                guest_heap_statistics.peak_live_capacity =
                    guest_heap_statistics.live_capacity;
            }
            struct guest_heap_site_stats *entry =
                guest_heap_site_locked(old_site, false);
            if (entry) {
                entry->live_bytes += new_size - old_size;
                if (entry->live_bytes > entry->peak_live_bytes) {
                    entry->peak_live_bytes = entry->live_bytes;
                }
            }
            ++guest_heap_statistics.realloc_in_place;
            pthread_mutex_unlock(&guest_heap_lock);
            return old_pointer;
        }
    }
    pthread_mutex_unlock(&guest_heap_lock);

    uint32_t result = guest_allocate_at(new_size, false, site);
    if (!result) return 0;
    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy((void *)(uintptr_t)result, (const void *)(uintptr_t)old_pointer, copy_size);
    guest_deallocate(old_pointer);
    pthread_mutex_lock(&guest_heap_lock);
    ++guest_heap_statistics.realloc_moved;
    pthread_mutex_unlock(&guest_heap_lock);
    return result;
}

static void replenish_legacy_render_pool_if_empty(void)
{
    const struct lp32_render_pool *pool = lp32_profile()->render_pool;
    if (!pool) return;
    uint32_t *free_count = (void *)(uintptr_t)pool->free_count;
    uint32_t *free_head = (void *)(uintptr_t)pool->free_head;
    if (*free_head) return;

    const uint32_t object_size = pool->object_size;
    const uint32_t object_count = pool->object_count;
    uint32_t block = guest_allocate_at(object_size * object_count, true,
                                       kGuestHeapRenderPoolSite);
    if (!block) return;
    for (uint32_t index = 0; index < object_count; ++index) {
        uint32_t object = block + index * object_size;
        uint32_t next = index + 1 < object_count ? object + object_size : 0;
        *(uint32_t *)(uintptr_t)(object + 0x28) = next;
    }
    *free_head = block;
    *free_count += object_count;
    fprintf(stderr, "compat32: extended legacy renderer pool by %u objects\n",
            object_count);
}

/* GCC 4's 32-bit COW std::string stores one pointer to character data.  The
   three 32-bit _Rep fields immediately preceding it are length, capacity and
   reference count.  Compatibility-created strings normally have independent
   reps; honoring a positive reference count also makes disposal safe if guest
   inline code happens to share one. */
static uint32_t guest_string_make(const char *bytes, size_t length,
                                  uint32_t site)
{
    if (length > UINT32_MAX - 13) return 0;
    uint32_t allocation = guest_allocate_at(12 + length + 1, false,
                                            site ? site : kGuestHeapStringSite);
    if (!allocation) return 0;
    uint32_t *rep = (void *)(uintptr_t)allocation;
    rep[0] = (uint32_t)length;
    rep[1] = (uint32_t)length;
    rep[2] = 0;
    char *data = (void *)(uintptr_t)(allocation + 12);
    if (length) memcpy(data, bytes, length);
    data[length] = '\0';
    return allocation + 12;
}

static const char *guest_string_data(uint32_t object)
{
    if (!object) return "";
    uint32_t data = *(const uint32_t *)(uintptr_t)object;
    return data ? (const char *)(uintptr_t)data : "";
}

static size_t guest_string_length(uint32_t object)
{
    if (!object) return 0;
    uint32_t data = *(const uint32_t *)(uintptr_t)object;
    if (!data) return 0;
    return *(const uint32_t *)(uintptr_t)(data - 12);
}

static void guest_string_dispose_rep(uint32_t rep)
{
    if (!rep || rep < kGuestHeapBase + kGuestHeapHeaderSize) {
        return; /* Includes libstdc++'s mapped static empty representation. */
    }
    pthread_mutex_lock(&guest_heap_lock);
    uint32_t *metadata = NULL;
    bool already_free = false;
    if (!guest_heap_validate_header_locked(rep, &metadata, &already_free) ||
        metadata[0] < 12) {
        pthread_mutex_unlock(&guest_heap_lock);
        return;
    }
    int32_t *reference_count = (void *)(uintptr_t)(rep + 8);
    if (*reference_count > 0) {
        --*reference_count;
        pthread_mutex_unlock(&guest_heap_lock);
        return;
    }
    pthread_mutex_unlock(&guest_heap_lock);
    guest_deallocate(rep);
}

static void guest_string_dispose_object(uint32_t object)
{
    if (!object) return;
    uint32_t *data_pointer = (void *)(uintptr_t)object;
    uint32_t data = *data_pointer;
    *data_pointer = 0;
    if (data >= 12) guest_string_dispose_rep(data - 12);
}

static uint32_t guest_string_construct(uint32_t object, const char *bytes,
                                       size_t length, uint32_t site)
{
    if (!object) return 0;
    uint32_t data = guest_string_make(bytes ? bytes : "", bytes ? length : 0,
                                      site);
    if (!data) return 0;
    *(uint32_t *)(uintptr_t)object = data;
    return object;
}

static uint32_t guest_string_assign(uint32_t object, const char *bytes,
                                    size_t length, uint32_t site)
{
    if (!object) return 0;
    uint32_t old_data = *(const uint32_t *)(uintptr_t)object;
    uint32_t data = guest_string_make(bytes ? bytes : "", bytes ? length : 0,
                                      site);
    if (!data) return 0;
    *(uint32_t *)(uintptr_t)object = data;
    if (old_data >= 12 && old_data != data) {
        guest_string_dispose_rep(old_data - 12);
    }
    return object;
}

struct __attribute__((packed, aligned(4))) guest_stat32 {
    int32_t st_dev;
    uint32_t st_ino;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    int32_t st_rdev;
    int32_t st_atime_sec;
    int32_t st_atime_nsec;
    int32_t st_mtime_sec;
    int32_t st_mtime_nsec;
    int32_t st_ctime_sec;
    int32_t st_ctime_nsec;
    int64_t st_size;
    int64_t st_blocks;
    int32_t st_blksize;
    uint32_t st_flags;
    uint32_t st_gen;
    int32_t st_lspare;
    int64_t st_qspare[2];
};

_Static_assert(sizeof(struct guest_stat32) == 96, "Darwin i386 stat layout");

/* Darwin's legacy i386 ABI uses four-byte longs and pointers in struct tm.
   The current host structure has eight-byte tm_gmtoff and tm_zone fields. */
struct __attribute__((packed, aligned(4))) guest_tm32 {
    int32_t tm_sec;
    int32_t tm_min;
    int32_t tm_hour;
    int32_t tm_mday;
    int32_t tm_mon;
    int32_t tm_year;
    int32_t tm_wday;
    int32_t tm_yday;
    int32_t tm_isdst;
    int32_t tm_gmtoff;
    uint32_t tm_zone;
};

struct __attribute__((packed, aligned(4))) guest_timeval32 {
    int32_t tv_sec;
    int32_t tv_usec;
};

_Static_assert(sizeof(struct guest_tm32) == 44,
               "Darwin i386 struct tm layout");
_Static_assert(sizeof(struct guest_timeval32) == 8,
               "Darwin i386 timeval layout");

static uint32_t copy_host_tm_to_guest(const struct tm *host,
                                      struct guest_tm32 *guest)
{
    if (!host || !guest) return 0;
    memset(guest, 0, sizeof(*guest));
    guest->tm_sec = host->tm_sec;
    guest->tm_min = host->tm_min;
    guest->tm_hour = host->tm_hour;
    guest->tm_mday = host->tm_mday;
    guest->tm_mon = host->tm_mon;
    guest->tm_year = host->tm_year;
    guest->tm_wday = host->tm_wday;
    guest->tm_yday = host->tm_yday;
    guest->tm_isdst = host->tm_isdst;
    guest->tm_gmtoff = (int32_t)host->tm_gmtoff;
    if (host->tm_zone) {
        size_t size = strlen(host->tm_zone) + 1;
        guest->tm_zone = guest_allocate(size, false);
        if (guest->tm_zone) {
            memcpy((void *)(uintptr_t)guest->tm_zone, host->tm_zone, size);
        }
    }
    return (uint32_t)(uintptr_t)guest;
}

static void append_formatted(char *output, size_t capacity, size_t *length,
                             const char *piece, size_t piece_length)
{
    if (capacity) {
        size_t available = *length < capacity - 1 ? capacity - 1 - *length : 0;
        size_t copy = piece_length < available ? piece_length : available;
        if (copy) memcpy(output + *length, piece, copy);
        output[(*length + copy < capacity) ? *length + copy : capacity - 1] = '\0';
    }
    *length += piece_length;
}

static int guest_vformat(char *output, size_t capacity, const char *format,
                         const uint32_t *words)
{
    size_t length = 0;
    size_t word = 0;
    if (capacity) output[0] = '\0';
    while (*format) {
        if (*format != '%') {
            const char *start = format;
            while (*format && *format != '%') ++format;
            append_formatted(output, capacity, &length, start,
                             (size_t)(format - start));
            continue;
        }
        const char *start = format++;
        if (*format == '%') {
            append_formatted(output, capacity, &length, "%", 1);
            ++format;
            continue;
        }

        int stars[2] = {0, 0};
        unsigned star_count = 0;
        while (*format && !strchr("diouxXfFeEgGaAcspn", *format)) {
            if (*format == '*' && star_count < 2) stars[star_count++] = (int)words[word++];
            ++format;
        }
        if (!*format) break;
        char conversion = *format++;
        size_t spec_length = (size_t)(format - start);
        if (spec_length >= 64) return -1;
        char spec[64];
        memcpy(spec, start, spec_length);
        spec[spec_length] = '\0';

        bool wide_integer = strstr(spec, "ll") || strchr(spec, 'j');
        char piece[4096];
        int count = 0;
#define FORMAT_VALUE(value) do { \
            if (star_count == 2) count = snprintf(piece, sizeof(piece), spec, stars[0], stars[1], (value)); \
            else if (star_count == 1) count = snprintf(piece, sizeof(piece), spec, stars[0], (value)); \
            else count = snprintf(piece, sizeof(piece), spec, (value)); \
        } while (0)
        if (conversion == 's') {
            const char *value = (const char *)(uintptr_t)words[word++];
            FORMAT_VALUE(value ? value : "(null)");
        } else if (conversion == 'c') {
            int value = (int)words[word++];
            FORMAT_VALUE(value);
        } else if (conversion == 'p') {
            void *value = (void *)(uintptr_t)words[word++];
            FORMAT_VALUE(value);
        } else if (strchr("fFeEgGaA", conversion)) {
            double value;
            memcpy(&value, words + word, sizeof(value));
            word += 2;
            FORMAT_VALUE(value);
        } else if (conversion == 'n') {
            uint32_t address = words[word++];
            if (address) *(int32_t *)(uintptr_t)address = (int32_t)length;
            continue;
        } else if (wide_integer) {
            uint64_t bits;
            memcpy(&bits, words + word, sizeof(bits));
            word += 2;
            if (conversion == 'd' || conversion == 'i') {
                FORMAT_VALUE((long long)(int64_t)bits);
            } else {
                FORMAT_VALUE((unsigned long long)bits);
            }
        } else if (conversion == 'd' || conversion == 'i') {
            int32_t value = (int32_t)words[word++];
            FORMAT_VALUE(value);
        } else {
            uint32_t value = words[word++];
            FORMAT_VALUE(value);
        }
#undef FORMAT_VALUE
        if (count < 0) return count;
        size_t piece_length = (size_t)count;
        if (piece_length >= sizeof(piece)) piece_length = sizeof(piece) - 1;
        append_formatted(output, capacity, &length, piece, piece_length);
    }
    return length > INT_MAX ? -1 : (int)length;
}

static int guest_vscan_consumed(const char *input, const char *format,
                                const uint32_t *destinations,
                                size_t *consumed);

static int guest_vscan(const char *input, const char *format,
                       const uint32_t *destinations)
{
    return guest_vscan_consumed(input, format, destinations, NULL);
}

/*
 * fscanf over a guest stream.  The pending input is scanned from a snapshot
 * of the stream and the file position is moved past exactly what the
 * conversions consumed, which is how the title's config/ini readers use it
 * (short whitespace-delimited tokens per call).
 */
static int guest_fscan(FILE *file, const char *format,
                       const uint32_t *destinations)
{
    enum { snapshot_size = 16384 };
    char *snapshot = malloc(snapshot_size + 1);
    if (!snapshot) return EOF;
    long position = ftell(file);
    if (position < 0) {
        free(snapshot);
        return EOF;
    }
    size_t available = fread(snapshot, 1, snapshot_size, file);
    if (available == 0) {
        free(snapshot);
        return EOF;
    }
    snapshot[available] = '\0';
    size_t consumed = 0;
    int assigned = guest_vscan_consumed(snapshot, format, destinations,
                                        &consumed);
    if (consumed > available) consumed = available;
    fseek(file, position + (long)consumed, SEEK_SET);
    free(snapshot);
    if (assigned == 0 && consumed == available) return EOF;
    return assigned;
}

static int guest_vscan_consumed(const char *input, const char *format,
                                const uint32_t *destinations,
                                size_t *consumed)
{
    const char *start = input;
    size_t destination = 0;
    int assigned = 0;
    while (*format) {
        if (isspace((unsigned char)*format)) {
            while (isspace((unsigned char)*format)) ++format;
            while (isspace((unsigned char)*input)) ++input;
            continue;
        }
        if (*format != '%') {
            if (*input != *format) break;
            ++input;
            ++format;
            continue;
        }
        ++format;
        if (*format == '%') {
            if (*input != '%') break;
            ++input;
            ++format;
            continue;
        }

        bool suppress = false;
        if (*format == '*') { suppress = true; ++format; }
        size_t width = 0;
        while (isdigit((unsigned char)*format)) {
            width = width * 10 + (unsigned)(*format++ - '0');
        }
        enum { length_default, length_hh, length_h, length_l, length_ll } length =
            length_default;
        if (*format == 'h') {
            ++format;
            length = *format == 'h' ? (++format, length_hh) : length_h;
        } else if (*format == 'l') {
            ++format;
            length = *format == 'l' ? (++format, length_ll) : length_l;
        }
        char conversion = *format++;
        if (!conversion) break;

        if (conversion != 'c' && conversion != '[' && conversion != 'n') {
            while (isspace((unsigned char)*input)) ++input;
        }
        char *end = NULL;
        uint32_t destination_address = suppress ? 0 : destinations[destination++];
        if (strchr("diouxX", conversion)) {
            int base = conversion == 'i' ? 0 :
                       (conversion == 'o' ? 8 :
                        (conversion == 'x' || conversion == 'X' ? 16 : 10));
            bool signed_value = conversion == 'd' || conversion == 'i';
            uint64_t value = signed_value ? (uint64_t)strtoll(input, &end, base) :
                                            strtoull(input, &end, base);
            if (end == input) break;
            if (!suppress && destination_address) {
                void *output = (void *)(uintptr_t)destination_address;
                if (length == length_hh) *(uint8_t *)output = (uint8_t)value;
                else if (length == length_h) *(uint16_t *)output = (uint16_t)value;
                else if (length == length_ll) *(uint64_t *)output = value;
                else *(uint32_t *)output = (uint32_t)value;
                ++assigned;
            }
            input = end;
        } else if (strchr("fFeEgGaA", conversion)) {
            double value = strtod(input, &end);
            if (end == input) break;
            if (!suppress && destination_address) {
                if (length == length_l || length == length_ll) {
                    *(double *)(uintptr_t)destination_address = value;
                } else {
                    *(float *)(uintptr_t)destination_address = (float)value;
                }
                ++assigned;
            }
            input = end;
        } else if (conversion == 's') {
            size_t count = 0;
            size_t maximum = width ? width : SIZE_MAX;
            while (input[count] && !isspace((unsigned char)input[count]) &&
                   count < maximum) ++count;
            if (!count) break;
            if (!suppress && destination_address) {
                char *output = (void *)(uintptr_t)destination_address;
                memcpy(output, input, count);
                output[count] = '\0';
                ++assigned;
            }
            input += count;
        } else if (conversion == 'c') {
            size_t count = width ? width : 1;
            size_t available = strlen(input);
            if (available < count) break;
            if (!suppress && destination_address) {
                memcpy((void *)(uintptr_t)destination_address, input, count);
                ++assigned;
            }
            input += count;
        } else if (conversion == 'n') {
            if (!suppress && destination_address) {
                *(uint32_t *)(uintptr_t)destination_address =
                    (uint32_t)(input - start);
            }
        } else if (conversion == '[') {
            /* Scanset: %[set] / %[^set], ']' first is literal, a-z ranges.
               The engine's config reader opens pcconfig.txt with
               "FileVersion%*[ \n\t]%d"; failing this conversion left the
               version at 0, which the game took as an outdated file, so it
               deleted the config and rewrote defaults on every launch. */
            bool negate = false;
            if (*format == '^') { negate = true; ++format; }
            bool set[256] = {false};
            const char *set_start = format;
            while (*format && (*format != ']' || format == set_start)) {
                unsigned char low = (unsigned char)*format;
                if (format[1] == '-' && format[2] && format[2] != ']') {
                    unsigned char high = (unsigned char)format[2];
                    for (unsigned c = low; c <= high; ++c) set[c] = true;
                    format += 3;
                } else {
                    set[low] = true;
                    ++format;
                }
            }
            if (*format != ']') break;
            ++format;
            size_t count = 0;
            size_t maximum = width ? width : SIZE_MAX;
            while (input[count] && count < maximum &&
                   set[(unsigned char)input[count]] != negate) ++count;
            if (!count) break;
            if (!suppress && destination_address) {
                char *output = (void *)(uintptr_t)destination_address;
                memcpy(output, input, count);
                output[count] = '\0';
                ++assigned;
            }
            input += count;
        } else {
            break;
        }
    }
    if (consumed) *consumed = (size_t)(input - start);
    return assigned;
}

/*
 * pthread_once over guest control blocks.  The i386 pthread_once_t is an
 * opaque 8-byte block; only its address matters here.  A host mutex guards
 * the table and the initializer runs on the calling guest thread.
 */
static int guest_pthread_once(uint32_t control, uint32_t initializer)
{
    static pthread_mutex_t once_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
    static uint32_t completed[256];
    static uint32_t completed_count;
    if (!control || !initializer) return EINVAL;
    pthread_mutex_lock(&once_lock);
    for (uint32_t index = 0; index < completed_count; ++index) {
        if (completed[index] == control) {
            pthread_mutex_unlock(&once_lock);
            return 0;
        }
    }
    if (completed_count < sizeof(completed) / sizeof(completed[0])) {
        completed[completed_count++] = control;
    }
    /* Holding the (recursive) lock across the initializer serialises
       concurrent first callers the way pthread_once does. */
    compat_runtime32_call(initializer, NULL, 0);
    pthread_mutex_unlock(&once_lock);
    return 0;
}

/*
 * Import dispatch is a long if-chain over the import name.  The name length
 * is computed once per dispatch and every candidate is rejected by length
 * before any bytes are compared.  import_is also records that the runtime
 * chain matched the name at all: a name that reaches the audio/Objective-C
 * stages without any runtime match is memoized per import id so later calls
 * skip the runtime chain entirely.  A runtime handler that matches and then
 * deliberately falls through keeps the full chain, so behaviour is unchanged.
 */
static _Thread_local size_t dispatch_name_length;
static _Thread_local bool dispatch_name_matched;

#define import_is(name, expected) \
    (LP32_NAME_IS((name), dispatch_name_length, (expected)) ? \
     (dispatch_name_matched = true) : false)

enum {
    kImportStageUnknown = 0,
    kImportStageRuntime = 1,
    kImportStageAudio = 2,
    kImportStageObjC = 3,
};

static uint8_t *import_stage_table;
static uint8_t dynamic_import_stage_table[kDynamicThunkCapacity];

/*
 * Direct-handler memo, one slot per import id.  NULL: not yet resolved;
 * kFastImportNone: resolved, no direct handler, always use the chains;
 * otherwise the handler.  Resolution happens after the first chained
 * dispatch of that id, so the chains still see every import at least once
 * (they also own the side effects of first use, such as dlsym thunk
 * creation).  LP32_NO_FAST_IMPORTS keeps every call on the chains.
 */
#define kFastImportNone ((lp32_fast_import_fn)(uintptr_t)1)
static lp32_fast_import_fn *fast_import_table;
static lp32_fast_import_fn dynamic_fast_import_table[kDynamicThunkCapacity];
static bool fast_imports_disabled;
/* Imports that block on purpose (the game's frame limiter and worker waits);
   LP32_FRAME_STATS reports their time as "sleep" rather than bridge time. */
static uint8_t *import_sleep_table;
static bool import_name_is_sleep(const char *name)
{
    static const char *const prefixes[] = {
        "_usleep", "_nanosleep", "_sleep", "_select", "_sem_wait",
        "_pthread_cond_wait", "_pthread_cond_timedwait", "_mach_wait_until",
        "_MPWaitOnSemaphore", "_MPWaitOnQueue", "_pthread_join", "_poll",
    };
    for (size_t index = 0; index < sizeof(prefixes) / sizeof(prefixes[0]); ++index) {
        size_t length = strlen(prefixes[index]);
        if (strncmp(name, prefixes[index], length) == 0 &&
            (name[length] == '\0' || name[length] == '$')) return true;
    }
    return false;
}

/* Per-import time accounting for LP32_FRAME_STATS (racy across threads by
   design; it is diagnostic only). */
struct import_profile_entry {
    const char *name;
    uint64_t calls;
    uint64_t ns;
};
static struct import_profile_entry *import_profile_table;
static struct import_profile_entry dynamic_import_profile_table[kDynamicThunkCapacity];

static struct import_profile_entry *import_profile_slot(uint32_t import_id)
{
    if ((import_id & UINT32_C(0x80000000)) != 0) {
        uint32_t dynamic_id = import_id & UINT32_C(0x7fffffff);
        return dynamic_id < kDynamicThunkCapacity ?
            &dynamic_import_profile_table[dynamic_id] : NULL;
    }
    if (import_profile_table && current_image &&
        import_id < current_image->import_count) {
        return &import_profile_table[import_id];
    }
    return NULL;
}

static uint8_t *import_stage_slot(uint32_t import_id)
{
    if ((import_id & UINT32_C(0x80000000)) != 0) {
        uint32_t dynamic_id = import_id & UINT32_C(0x7fffffff);
        return dynamic_id < kDynamicThunkCapacity ?
            &dynamic_import_stage_table[dynamic_id] : NULL;
    }
    if (import_stage_table && current_image &&
        import_id < current_image->import_count) {
        return &import_stage_table[import_id];
    }
    return NULL;
}

/*
 * Rosetta's undocumented compatibility-mode path can mis-decode a low thunk
 * as x86_64 when two threads execute i386 code concurrently.  Serialize only
 * the guest instruction intervals.  The assembly import gateway drops this
 * lock before calling any host API (including blocking pthread operations)
 * and reacquires it immediately before returning to i386 code.
 */
int compat_runtime32_frame_profile_enabled;
static _Thread_local struct compat_runtime32_frame_profile frame_profile;

static inline uint64_t profile_now(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void compat_runtime32_take_frame_profile(
    struct compat_runtime32_frame_profile *out)
{
    *out = frame_profile;
    memset(&frame_profile, 0, sizeof(frame_profile));
}

/* Long uninterrupted guest intervals on worker threads stall the render
   thread at its next import.  With profiling enabled, holds longer than this
   are attributed to the import that ended them. */
static _Thread_local uint64_t guest_hold_start_ns;
static _Thread_local uint64_t guest_last_hold_ns;
static bool guest_execution_lock_disabled;
enum { kLongGuestHoldNs = 4000000 };

void lp32_guest_execution_enter(void)
{
    /* The plain global is tested first: the thread-local costs a
       tlv_get_addr call, and this runs twice per import. */
    if (guest_execution_lock_disabled || guest_execution_lock_held) return;
    if (compat_runtime32_frame_profile_enabled) {
        if (pthread_mutex_trylock(&guest_execution_lock) != 0) {
            uint64_t start = profile_now();
            pthread_mutex_lock(&guest_execution_lock);
            frame_profile.lock_wait_ns += profile_now() - start;
            ++frame_profile.lock_waits;
        }
        guest_hold_start_ns = profile_now();
    } else {
        pthread_mutex_lock(&guest_execution_lock);
    }
    guest_execution_lock_held = true;
}

void lp32_guest_execution_leave(void)
{
    if (guest_execution_lock_disabled || !guest_execution_lock_held) return;
    guest_execution_lock_held = false;
    if (compat_runtime32_frame_profile_enabled && guest_hold_start_ns) {
        guest_last_hold_ns = profile_now() - guest_hold_start_ns;
    }
    pthread_mutex_unlock(&guest_execution_lock);
}

static float guest_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static double guest_double(const uint32_t *words)
{
    double value;
    memcpy(&value, words, sizeof(value));
    return value;
}

static uint64_t return_guest_float(float value)
{
    lp32_fp_result.f = value;
    lp32_return_fp_kind = 1;
    return 0;
}

static uint64_t return_guest_double(double value)
{
    lp32_fp_result.d = value;
    lp32_return_fp_kind = 2;
    return 0;
}

uint64_t compat_runtime32_return_double(double value)
{
    return return_guest_double(value);
}

uint64_t compat_runtime32_return_float(float value)
{
    return return_guest_float(value);
}

static void *run_guest_thread(void *opaque)
{
    struct guest_thread_context context = *(struct guest_thread_context *)opaque;
    free(opaque);
    if (getenv("LP32_TRACE_THREADS")) {
        uint64_t thread_id = 0;
        pthread_threadid_np(NULL, &thread_id);
        uint32_t record = *(uint32_t *)(uintptr_t)context.argument;
        fprintf(stderr,
                "compat32: guest thread tid=%llu entry=0x%08x record=0x%08x "
                "fn=0x%08x name=%.31s\n",
                (unsigned long long)thread_id, context.function, record,
                record ? *(uint32_t *)(uintptr_t)record : 0,
                record ? (const char *)(uintptr_t)(record + 8) : "");
    }
    uint32_t result = compat_runtime32_call(context.function, &context.argument, 1);
    if (getenv("LP32_TRACE_THREADS")) {
        uint64_t thread_id = 0;
        pthread_threadid_np(NULL, &thread_id);
        fprintf(stderr, "compat32: guest thread tid=%llu exited\n",
                (unsigned long long)thread_id);
    }
    return (void *)(uintptr_t)result;
}

static int guest_qsort_compare(uint32_t comparator, const void *left,
                               const void *right)
{
    const uint32_t arguments[2] = {
        (uint32_t)(uintptr_t)left,
        (uint32_t)(uintptr_t)right,
    };
    return (int32_t)compat_runtime32_call(comparator, arguments, 2);
}

static void guest_qsort_swap(uint8_t *left, uint8_t *right, size_t width)
{
    uint8_t buffer[256];
    while (width) {
        size_t chunk = width < sizeof(buffer) ? width : sizeof(buffer);
        memcpy(buffer, left, chunk);
        memcpy(left, right, chunk);
        memcpy(right, buffer, chunk);
        left += chunk;
        right += chunk;
        width -= chunk;
    }
}

/*
 * The host libc qsort hands the comparator pointers into a scratch copy of
 * the array for some sizes.  That copy lives in the 64-bit host heap, and the
 * guest comparator only sees the low 32 bits.  Sort in place instead
 * (heapsort: every comparison addresses the guest's own array).
 */
static void guest_qsort(uint8_t *base, size_t count, size_t width,
                        uint32_t comparator)
{
#define GUEST_QSORT_ELEMENT(index) (base + (index) * width)
    for (size_t start = count / 2; start-- > 0;) {
        size_t root = start;
        for (;;) {
            size_t child = root * 2 + 1;
            if (child >= count) break;
            if (child + 1 < count &&
                guest_qsort_compare(comparator, GUEST_QSORT_ELEMENT(child),
                                    GUEST_QSORT_ELEMENT(child + 1)) < 0) {
                ++child;
            }
            if (guest_qsort_compare(comparator, GUEST_QSORT_ELEMENT(root),
                                    GUEST_QSORT_ELEMENT(child)) >= 0) {
                break;
            }
            guest_qsort_swap(GUEST_QSORT_ELEMENT(root),
                             GUEST_QSORT_ELEMENT(child), width);
            root = child;
        }
    }
    for (size_t end = count; end-- > 1;) {
        guest_qsort_swap(GUEST_QSORT_ELEMENT(0), GUEST_QSORT_ELEMENT(end), width);
        size_t root = 0;
        for (;;) {
            size_t child = root * 2 + 1;
            if (child >= end) break;
            if (child + 1 < end &&
                guest_qsort_compare(comparator, GUEST_QSORT_ELEMENT(child),
                                    GUEST_QSORT_ELEMENT(child + 1)) < 0) {
                ++child;
            }
            if (guest_qsort_compare(comparator, GUEST_QSORT_ELEMENT(root),
                                    GUEST_QSORT_ELEMENT(child)) >= 0) {
                break;
            }
            guest_qsort_swap(GUEST_QSORT_ELEMENT(root),
                             GUEST_QSORT_ELEMENT(child), width);
            root = child;
        }
    }
#undef GUEST_QSORT_ELEMENT
}

const struct macho_image32 *compat_runtime32_image(void)
{
    return current_image;
}

static void peggle_apply_relocations(struct macho_image32 *image);

int compat_runtime32_initialize(struct macho_image32 *image)
{
    if (!process_is_translated()) {
        errno = ENOTSUP;
        return runtime_error("x86_64 host is not running through Rosetta");
    }
    current_image = image;
    import_stage_table = calloc(image->import_count ? image->import_count : 1,
                                sizeof(*import_stage_table));
    import_profile_table = calloc(image->import_count ? image->import_count : 1,
                                  sizeof(*import_profile_table));
    fast_import_table = calloc(image->import_count ? image->import_count : 1,
                               sizeof(*fast_import_table));
    import_sleep_table = calloc(image->import_count ? image->import_count : 1,
                                sizeof(*import_sleep_table));
    if (import_sleep_table) {
        for (uint32_t index = 0; index < image->import_count; ++index) {
            import_sleep_table[index] = import_name_is_sleep(image->imports[index].name);
        }
    }
    fast_imports_disabled = getenv("LP32_NO_FAST_IMPORTS") != NULL;
    const char *reuse_text = getenv("LP32_GUEST_HEAP_REUSE");
    guest_heap_reuse = !(reuse_text && strcmp(reuse_text, "0") == 0);
    guest_heap_poison = getenv("LP32_GUEST_HEAP_POISON") != NULL;
    guest_heap_trace = getenv("LP32_TRACE_GUEST_HEAP") != NULL;
    timing_trace = getenv("LP32_TRACE_TIMING") != NULL;
    compat_runtime32_frame_profile_enabled = getenv("LP32_FRAME_STATS") != NULL;
    /* The global guest lock guarded against Rosetta mis-decoding a page that
       held both 32-bit thunks and 64-bit landing pads.  Those now live on
       separate pages, and the lock cost the render thread up to a third of
       its time waiting behind worker threads, so it is off unless asked
       for.  LP32_NO_GUEST_LOCK is still accepted as the explicit form. */
    guest_execution_lock_disabled = getenv("LP32_GUEST_LOCK") == NULL;
    const char *report_interval_text = getenv("LP32_HEAP_REPORT_SWAPS");
    if (report_interval_text && report_interval_text[0]) {
        guest_heap_report_swap_interval =
            strtoull(report_interval_text, NULL, 0);
    }
    guest_heap_statistics.high_water = guest_heap_cursor;
    if (allocate_cs32() != 0) return -1;
    if (build_transition_bridge() != 0) return -1;
    peggle_apply_relocations(image);
    printf("compat32: cs32=0x%04x imports=%" PRIu32 " bridge=0x%08x\n",
           lp32_cs32, image->import_count, kBridgeCodeBase);
    fprintf(stderr,
            "compat32: guest heap validation enabled, reuse=%s poison=%s\n",
            guest_heap_reuse ? "on" : "off",
            guest_heap_poison ? "on" : "off");
    if (getenv("LP32_TRACE_IMPORT_TABLE")) {
        for (uint32_t index = 0; index < image->import_count; ++index) {
            fprintf(stderr, "compat32: import[%" PRIu32 "] 0x%08" PRIx32
                    " %s\n", index, image->imports[index].address,
                    image->imports[index].name);
        }
    }
    return 0;
}

#include "peggle_cpp.inc"
#include "peggle_libc.inc"
#include "peggle_bass.inc"

static uint64_t dispatch_named_import(uint32_t import_id, const char *name,
                                      const uint32_t *arguments,
                                      uint32_t return_address);

static lp32_fast_import_fn *fast_import_slot(uint32_t import_id)
{
    if ((import_id & UINT32_C(0x80000000)) != 0) {
        uint32_t dynamic_id = import_id & UINT32_C(0x7fffffff);
        return dynamic_id < kDynamicThunkCapacity ?
            &dynamic_fast_import_table[dynamic_id] : NULL;
    }
    if (fast_import_table && current_image &&
        import_id < current_image->import_count) {
        return &fast_import_table[import_id];
    }
    return NULL;
}

static lp32_fast_import_fn runtime_fast_import(const char *name);

static const char *import_name_for_id(uint32_t import_id)
{
    if ((import_id & UINT32_C(0x80000000)) != 0) {
        uint32_t dynamic_id = import_id & UINT32_C(0x7fffffff);
        if (dynamic_id < dynamic_symbol_count) {
            return dynamic_symbol_names[dynamic_id];
        }
    } else if (current_image && import_id < current_image->import_count) {
        return current_image->imports[import_id].name;
    }
    return "<invalid import>";
}

static uint64_t dispatch_import_chained(uint32_t import_id, const char *name,
                                        const uint32_t *arguments,
                                        uint32_t return_address,
                                        lp32_fast_import_fn *fast_slot)
{
    uint64_t result = dispatch_named_import(import_id, name, arguments,
                                            return_address);
    if (fast_slot && !*fast_slot && !fast_imports_disabled) {
        lp32_fast_import_fn handler = runtime_fast_import(name);
        if (!handler) handler = objc_bridge32_fast_import(name);
        *fast_slot = handler ? handler : kFastImportNone;
    }
    return result;
}

uint64_t lp32_dispatch_import(uint32_t import_id, const uint32_t *arguments,
                              uint32_t return_address)
{
    lp32_fast_import_fn *fast_slot = fast_import_slot(import_id);
    lp32_fast_import_fn fast = fast_slot ? *fast_slot : NULL;
    if (!compat_runtime32_frame_profile_enabled) {
        if ((uintptr_t)fast > 1) return fast(arguments, return_address);
        return dispatch_import_chained(import_id, import_name_for_id(import_id),
                                       arguments, return_address, fast_slot);
    }
    const char *name = import_name_for_id(import_id);
    if (guest_last_hold_ns >= kLongGuestHoldNs) {
        fprintf(stderr,
                "compat32: guest hold %.1fms on thread %#llx ended by %s "
                "from 0x%08" PRIx32 "\n",
                (double)guest_last_hold_ns / 1e6,
                (unsigned long long)pthread_mach_thread_np(pthread_self()),
                name, return_address);
        guest_last_hold_ns = 0;
    }
    uint64_t start = profile_now();
    uint64_t result = (uintptr_t)fast > 1 ?
        fast(arguments, return_address) :
        dispatch_import_chained(import_id, name, arguments, return_address,
                                fast_slot);
    uint64_t elapsed = profile_now() - start;
    if (import_sleep_table && (import_id & UINT32_C(0x80000000)) == 0 &&
        current_image && import_id < current_image->import_count &&
        import_sleep_table[import_id]) {
        frame_profile.sleep_ns += elapsed;
    } else {
        frame_profile.dispatch_ns += elapsed;
    }
    ++frame_profile.calls;
    struct import_profile_entry *entry = import_profile_slot(import_id);
    if (entry) {
        entry->name = name;
        entry->calls += 1;
        entry->ns += elapsed;
    }
    /* LP32_SLOW_IMPORT_MS=<n>: report any import on any thread that took
       longer than n ms, to find where a worker (audio decoder, loader)
       stalls. */
    static int64_t slow_import_threshold_ns = -1;
    if (slow_import_threshold_ns < 0) {
        const char *text = getenv("LP32_SLOW_IMPORT_MS");
        slow_import_threshold_ns = text && text[0] ?
            (int64_t)(strtod(text, NULL) * 1e6) : 0;
    }
    if (slow_import_threshold_ns > 0 && (int64_t)elapsed >= slow_import_threshold_ns) {
        fprintf(stderr,
                "compat32: slow import %s %.1fms on thread %#llx from 0x%08" PRIx32
                " args=%08x %08x %08x\n",
                name, (double)elapsed / 1e6,
                (unsigned long long)pthread_mach_thread_np(pthread_self()),
                return_address, arguments[0], arguments[1], arguments[2]);
    }
    return result;
}

void compat_runtime32_report_import_profile(unsigned top)
{
    struct import_profile_entry *entries[2] = {
        import_profile_table, dynamic_import_profile_table,
    };
    size_t counts[2] = {
        current_image ? current_image->import_count : 0,
        kDynamicThunkCapacity,
    };
    uint64_t total_ns = 0;
    uint64_t total_calls = 0;
    for (unsigned table = 0; table < 2; ++table) {
        for (size_t index = 0; index < counts[table]; ++index) {
            total_ns += entries[table][index].ns;
            total_calls += entries[table][index].calls;
        }
    }
    fprintf(stderr, "compat32: import profile total calls=%llu time=%.1fms\n",
            (unsigned long long)total_calls, (double)total_ns / 1e6);
    for (unsigned rank = 0; rank < top; ++rank) {
        struct import_profile_entry *best = NULL;
        for (unsigned table = 0; table < 2; ++table) {
            for (size_t index = 0; index < counts[table]; ++index) {
                struct import_profile_entry *entry = &entries[table][index];
                if (entry->calls && (!best || entry->ns > best->ns)) best = entry;
            }
        }
        if (!best) break;
        fprintf(stderr, "compat32:   %-40s calls=%-8llu time=%7.2fms avg=%6.0fns\n",
                best->name ? best->name : "?", (unsigned long long)best->calls,
                (double)best->ns / 1e6, (double)best->ns / (double)best->calls);
        best->calls = 0;
        best->ns = 0;
    }
    for (unsigned table = 0; table < 2; ++table) {
        for (size_t index = 0; index < counts[table]; ++index) {
            entries[table][index].calls = 0;
            entries[table][index].ns = 0;
        }
    }
}

static uint64_t dispatch_bridge_stages(uint32_t import_id, const char *name,
                                       const uint32_t *arguments,
                                       uint32_t return_address,
                                       uint8_t known_stage);

/*
 * Direct handlers for the runtime imports the game issues thousands of
 * times per frame.  The named chain below calls the same functions, so a
 * memoized direct call and a chained call behave identically.
 */
/* LP32_TRACE_ATOMIC=0xaddr[,0xaddr...] logs every OSAtomicAdd32 on the listed
   guest cells with the calling thread and guest return address (diagnostic
   for lock-free queues whose indices go out of step). */
static uint32_t atomic_trace_cells[8];
static unsigned atomic_trace_count;
static bool atomic_trace_parsed;

static void parse_atomic_trace(void)
{
    atomic_trace_parsed = true;
    const char *spec = getenv("LP32_TRACE_ATOMIC");
    if (!spec || !spec[0]) return;
    char buffer[256];
    strlcpy(buffer, spec, sizeof(buffer));
    char *cursor = buffer;
    for (char *item = strsep(&cursor, ","); item; item = strsep(&cursor, ",")) {
        if (atomic_trace_count == sizeof(atomic_trace_cells) /
                                  sizeof(atomic_trace_cells[0])) break;
        uint32_t address = (uint32_t)strtoul(item, NULL, 0);
        if (address) atomic_trace_cells[atomic_trace_count++] = address;
    }
}

static uint64_t fast_OSAtomicAdd32(const uint32_t *arguments,
                                   uint32_t return_address)
{
    int32_t *value = (void *)(uintptr_t)arguments[1];
    int32_t result = __atomic_add_fetch(value, (int32_t)arguments[0],
                                        __ATOMIC_SEQ_CST);
    if (!atomic_trace_parsed) parse_atomic_trace();
    for (unsigned index = 0; index < atomic_trace_count; ++index) {
        if (atomic_trace_cells[index] != arguments[1]) continue;
        uint64_t thread_id = 0;
        pthread_threadid_np(NULL, &thread_id);
        fprintf(stderr,
                "compat32: atomic-add tid=%llu ra=0x%08x [0x%08x] %d -> %d\n",
                (unsigned long long)thread_id, return_address, arguments[1],
                result - (int32_t)arguments[0], result);
        break;
    }
    return (uint32_t)result;
}

static uint64_t fast_OSAtomicCompareAndSwap32(const uint32_t *arguments,
                                              uint32_t return_address)
{
    (void)return_address;
    int32_t expected = (int32_t)arguments[0];
    int32_t *value = (void *)(uintptr_t)arguments[2];
    return __atomic_compare_exchange_n(value, &expected, (int32_t)arguments[1],
                                       false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

static uint64_t fast_memmove(const uint32_t *arguments, uint32_t return_address)
{
    (void)return_address;
    memmove((void *)(uintptr_t)arguments[0], (const void *)(uintptr_t)arguments[1],
            arguments[2]);
    return arguments[0];
}

static uint64_t fast_memset(const uint32_t *arguments, uint32_t return_address)
{
    (void)return_address;
    memset((void *)(uintptr_t)arguments[0], (int)arguments[1], arguments[2]);
    return arguments[0];
}

static uint64_t fast_logf(const uint32_t *arguments, uint32_t return_address)
{
    (void)return_address;
    return return_guest_float(logf(guest_float(arguments[0])));
}

static uint64_t fast_expf(const uint32_t *arguments, uint32_t return_address)
{
    (void)return_address;
    return return_guest_float(expf(guest_float(arguments[0])));
}

static uint64_t fast_pthread_mutex_lock(const uint32_t *arguments,
                                        uint32_t return_address)
{
    pthread_mutex_t *mutex = host_mutex_for_guest(arguments[0], true);
    if (!mutex) return (uint32_t)ENOMEM;
    int status = pthread_mutex_lock(mutex);
    const struct lp32_render_pool *pool = lp32_profile()->render_pool;
    if (!pool) return (uint32_t)status;
    if (return_address == pool->return_address &&
        getenv("LP32_TRACE_RENDER_POOL")) {
        const uint32_t *frame =
            (const void *)((uintptr_t)arguments + UINT32_C(0x68));
        fprintf(stderr,
                "compat32: renderer pool lock address=0x%08" PRIx32
                " status=%d head=0x%08" PRIx32 " count=%" PRIu32
                " create=%" PRIu32 "x%" PRIu32 " format=%" PRIu32
                " levels=%" PRIu32 "\n",
                arguments[0], status,
                *(uint32_t *)(uintptr_t)pool->free_head,
                *(uint32_t *)(uintptr_t)pool->free_count,
                frame[3], frame[4], frame[2], frame[6]);
    }
    if (status == 0 &&
        (arguments[0] == pool->mutex || return_address == pool->return_address)) {
        replenish_legacy_render_pool_if_empty();
    }
    return (uint32_t)status;
}

static uint64_t fast_pthread_mutex_trylock(const uint32_t *arguments,
                                           uint32_t return_address)
{
    (void)return_address;
    pthread_mutex_t *mutex = host_mutex_for_guest(arguments[0], true);
    return mutex ? (uint32_t)pthread_mutex_trylock(mutex) : (uint32_t)ENOMEM;
}

static uint64_t fast_pthread_mutex_unlock(const uint32_t *arguments,
                                          uint32_t return_address)
{
    (void)return_address;
    pthread_mutex_t *mutex = host_mutex_for_guest(arguments[0], false);
    return mutex ? (uint32_t)pthread_mutex_unlock(mutex) : (uint32_t)EINVAL;
}

static lp32_fast_import_fn runtime_fast_import(const char *name)
{
    static const struct {
        const char *name;
        lp32_fast_import_fn handler;
    } table[] = {
        {"_OSAtomicAdd32", fast_OSAtomicAdd32},
        {"_OSAtomicCompareAndSwap32", fast_OSAtomicCompareAndSwap32},
        {"_memcpy", fast_memmove},
        {"_memmove", fast_memmove},
        {"___memcpy_chk", fast_memmove},
        {"_memset", fast_memset},
        {"___memset_chk", fast_memset},
        {"_logf", fast_logf},
        {"_expf", fast_expf},
        {"_pthread_mutex_lock", fast_pthread_mutex_lock},
        {"_pthread_mutex_trylock", fast_pthread_mutex_trylock},
        {"_pthread_mutex_unlock", fast_pthread_mutex_unlock},
    };
    for (size_t index = 0; index < sizeof(table) / sizeof(table[0]); ++index) {
        if (strcmp(name, table[index].name) == 0) return table[index].handler;
    }
    return NULL;
}

static int (*g_named_import_override)(const char *, const uint32_t *,
                                      uint64_t *);

void compat_runtime32_set_named_import_override(
    int (*handler)(const char *name, const uint32_t *arguments,
                   uint64_t *result))
{
    g_named_import_override = handler;
}

static uint64_t dispatch_named_import(uint32_t import_id, const char *name,
                                      const uint32_t *arguments,
                                      uint32_t return_address)
{
    uint64_t peggle_result;
    /* Darwin exports ABI-variant aliases such as "_open$UNIX2003" and
       "_readdir$INODE64" that are behaviourally identical to the base symbol.
       Bejeweled 3 imports 15 "$UNIX2003" variants Peggle never used.  C
       symbol names never contain '$' otherwise, so normalise to the base name
       once here, and every downstream dispatcher matches without per-variant
       cases. */
    if (strchr(name, '$')) {
        static _Thread_local char base[256];
        size_t length = (size_t)(strchr(name, '$') - name);
        if (length < sizeof(base)) {
            memcpy(base, name, length);
            base[length] = '\0';
            name = base;
        }
    }
    if (g_named_import_override &&
        g_named_import_override(name, arguments, &peggle_result)) {
        return peggle_result;
    }
    if (peggle_cpp_dispatch(name, arguments, &peggle_result) ||
        peggle_libc_dispatch(name, arguments, &peggle_result) ||
        peggle_bass_dispatch(name, arguments, &peggle_result)) return peggle_result;
    dispatch_name_length = strlen(name);
    dispatch_name_matched = false;
    const uint8_t *stage = import_stage_slot(import_id);
    if (stage && *stage >= kImportStageAudio) {
        return dispatch_bridge_stages(import_id, name, arguments,
                                      return_address, *stage);
    }
    if (import_is(name, "__keymgr_get_and_lock_processwide_ptr_2")) {
        uint32_t key = arguments[0];
        uint32_t *output = (void *)(uintptr_t)arguments[1];
        uint32_t value = key < 64 ? keymgr_slots[key] : 0;
        if (output) *output = value;
        /* A nonzero status cleanly disables the obsolete libgcc facility. */
        return value ? 0 : 1;
    }
    if (import_is(name, "_dlopen")) return 1;
    if (import_is(name, "_dlsym")) {
        const char *symbol = (const char *)(uintptr_t)arguments[1];
        if (!symbol || !dlsym(RTLD_DEFAULT, symbol)) return 0;
        return guest_thunk_for_dynamic_symbol(symbol);
    }
    /*
     * The game only reaches exit/_exit after its own quit confirmation and
     * save handling.  Returning through the compatibility thunk would unmap
     * the guest while CoreAudio callback threads still reference it, which
     * leaves AppKit visibly frozen.  Process exit is the correct boundary for
     * those quarantined legacy audio objects and does not run incompatible
     * host/guest teardown callbacks.
     */
    if (import_is(name, "_exit") || import_is(name, "__exit")) {
        fprintf(stderr, "compat32: guest requested process exit status=%d\n",
                (int)arguments[0]);
        compat_runtime32_heap_report("guest-exit");
        cg_report("guest-exit");
        fflush(NULL);
        _Exit((int)arguments[0]);
    }

    if (import_is(name, "_cgCreateContext")) {
        typedef void *(*function_type)(void);
        function_type function = (function_type)cg_symbol("cgCreateContext");
        return function ? guest_handle_for_cg_object(function()) : 0;
    }
    if (import_is(name, "_cgCreateProgram")) {
        typedef void *(*function_type)(void *, int, const char *, int,
                                       const char *, const char **);
        function_type function = (function_type)cg_symbol("cgCreateProgram");
        if (!function) return 0;
        const uint32_t *guest_arguments =
            (const void *)(uintptr_t)arguments[5];
        const char *host_arguments[65];
        const char **host_argument_pointer = NULL;
        if (guest_arguments) {
            size_t count = 0;
            while (count < 64 && guest_arguments[count]) {
                host_arguments[count] =
                    (const char *)(uintptr_t)guest_arguments[count];
                ++count;
            }
            host_arguments[count] = NULL;
            host_argument_pointer = host_arguments;
        }
        void *program = function(
            cg_object_for_guest(arguments[0]), (int)arguments[1],
            (const char *)(uintptr_t)arguments[2], (int)arguments[3],
            (const char *)(uintptr_t)arguments[4], host_argument_pointer);
        uint32_t handle = guest_handle_for_cg_object(program);
        __atomic_fetch_add(handle ? &cg_programs_created : &cg_null_programs,
                           1, __ATOMIC_RELAXED);
        if (getenv("LP32_TRACE_CG")) {
            const char *source = (const char *)(uintptr_t)arguments[2];
            const char *entry = (const char *)(uintptr_t)arguments[4];
            fprintf(stderr, "compat32: cgCreateProgram type=%u profile=%u entry=%s "
                    "source=%zu bytes args=", arguments[1], arguments[3],
                    entry ? entry : "(null)", source ? strlen(source) : 0);
            for (size_t index = 0; host_argument_pointer &&
                 host_argument_pointer[index]; ++index) {
                fprintf(stderr, "%s%s", index ? " " : "", host_argument_pointer[index]);
            }
            fprintf(stderr, " -> %#x\n", handle);
            if (!handle) {
                typedef const char *(*listing_type)(void *);
                listing_type listing = (listing_type)cg_symbol("cgGetLastListing");
                const char *text = listing ? listing(cg_object_for_guest(arguments[0])) : NULL;
                fprintf(stderr, "compat32: cg listing: %s\n", text ? text : "(none)");
            }
        }
        return handle;
    }
    if (import_is(name, "_cgDestroyParameter") ||
        import_is(name, "_cgDestroyProgram")) {
        typedef void (*function_type)(void *);
        const char *symbol = import_is(name, "_cgDestroyParameter") ?
                             "cgDestroyParameter" : "cgDestroyProgram";
        function_type function = (function_type)cg_symbol(symbol);
        if (function) function(cg_object_for_guest(arguments[0]));
        if (import_is(name, "_cgDestroyProgram")) {
            release_cg_program_strings(arguments[0]);
            __atomic_fetch_add(&cg_programs_destroyed, 1, __ATOMIC_RELAXED);
        }
        return 0;
    }
    if (import_is(name, "_cgGetArrayParameter")) {
        typedef void *(*function_type)(void *, int);
        function_type function = (function_type)cg_symbol("cgGetArrayParameter");
        return function ? guest_handle_for_cg_object(function(
                              cg_object_for_guest(arguments[0]),
                              (int)arguments[1])) : 0;
    }
    if (import_is(name, "_cgGetFirstParameter")) {
        typedef void *(*function_type)(void *, int);
        function_type function = (function_type)cg_symbol("cgGetFirstParameter");
        uint32_t handle = function ? guest_handle_for_cg_object(function(
                                         cg_object_for_guest(arguments[0]),
                                         (int)arguments[1])) : 0;
        if (!handle) {
            __atomic_fetch_add(&cg_null_first_parameters, 1, __ATOMIC_RELAXED);
        }
        if (getenv("LP32_TRACE_CG")) {
            fprintf(stderr, "compat32: cgGetFirstParameter program=%#x ns=%u -> %#x\n",
                    arguments[0], arguments[1], handle);
        }
        return handle;
    }
    if (import_is(name, "_cgGetNamedParameter")) {
        typedef void *(*function_type)(void *, const char *);
        function_type function = (function_type)cg_symbol("cgGetNamedParameter");
        uint32_t handle = function ? guest_handle_for_cg_object(function(
                              cg_object_for_guest(arguments[0]),
                              (const char *)(uintptr_t)arguments[1])) : 0;
        if (getenv("LP32_TRACE_CG")) {
            fprintf(stderr, "compat32: cgGetNamedParameter program=%#x %s -> %#x\n",
                    arguments[0], (const char *)(uintptr_t)arguments[1], handle);
        }
        return handle;
    }
    if (import_is(name, "_cgGetNextParameter")) {
        typedef void *(*function_type)(void *);
        function_type function = (function_type)cg_symbol("cgGetNextParameter");
        uint32_t handle = function ? guest_handle_for_cg_object(function(
                              cg_object_for_guest(arguments[0]))) : 0;
        if (getenv("LP32_TRACE_CG")) {
            fprintf(stderr, "compat32: cgGetNextParameter %#x -> %#x\n", arguments[0],
                    handle);
        }
        return handle;
    }
    if (import_is(name, "_cgGetLastListing")) {
        typedef const char *(*function_type)(void *);
        function_type function = (function_type)cg_symbol("cgGetLastListing");
        return function ? guest_copy_nullable_cstring(function(
                              cg_object_for_guest(arguments[0]))) : 0;
    }
    if (import_is(name, "_cgGetParameterName")) {
        typedef const char *(*function_type)(void *);
        function_type function = (function_type)cg_symbol("cgGetParameterName");
        const char *parameter_name = function ?
            function(cg_object_for_guest(arguments[0])) : NULL;
        if (getenv("LP32_TRACE_CG")) {
            fprintf(stderr, "compat32: cgGetParameterName %#x -> %s\n", arguments[0],
                    parameter_name ? parameter_name : "(null)");
        }
        return guest_copy_nullable_cstring(parameter_name);
    }
    if (import_is(name, "_cgGetProgramString")) {
        typedef const char *(*function_type)(void *, int);
        function_type function = (function_type)cg_symbol("cgGetProgramString");
        return function ? guest_cg_string_owned(arguments[0], function(
                              cg_object_for_guest(arguments[0]),
                              (int)arguments[1])) : 0;
    }
    if (import_is(name, "_cgGetString")) {
        typedef const char *(*function_type)(int);
        function_type function = (function_type)cg_symbol("cgGetString");
        return function ? guest_copy_nullable_cstring(function(
                              (int)arguments[0])) : 0;
    }
    if (import_is(name, "_cgGetParameterClass") ||
        import_is(name, "_cgGetParameterResourceIndex") ||
        import_is(name, "_cgGetParameterType") ||
        import_is(name, "_cgIsParameterReferenced")) {
        typedef int (*function_type)(void *);
        const char *symbol = name + 1;
        function_type function = (function_type)cg_symbol(symbol);
        uint32_t value = function ?
            (uint32_t)function(cg_object_for_guest(arguments[0])) : 0;
        if (getenv("LP32_TRACE_CG")) {
            fprintf(stderr, "compat32: %s %#x -> %u\n", symbol, arguments[0], value);
        }
        return value;
    }
    if (import_is(name, "__keymgr_get_and_lock_processwide_ptr")) {
        uint32_t key = arguments[0];
        return key < 64 ? keymgr_slots[key] : 0;
    }
    if (import_is(name, "__keymgr_set_and_unlock_processwide_ptr")) {
        uint32_t key = arguments[0];
        if (key < 64) keymgr_slots[key] = arguments[1];
        return 0;
    }
    if (import_is(name, "___keymgr_dwarf2_register_sections")) return 0;

    if (import_is(name, "_malloc")) {
        return guest_allocate_at(arguments[0], false, return_address);
    }
    if (import_is(name, "__Znwm") || import_is(name, "__Znam")) {
        return guest_allocate_at(arguments[0], false, return_address);
    }
    if (import_is(name, "_calloc")) {
        uint64_t size = (uint64_t)arguments[0] * arguments[1];
        return size <= SIZE_MAX ?
            guest_allocate_at((size_t)size, true, return_address) : 0;
    }
    if (import_is(name, "_realloc")) {
        return guest_reallocate(arguments[0], arguments[1], return_address);
    }
    if (import_is(name, "_free") || import_is(name, "__ZdlPv") ||
        import_is(name, "__ZdaPv")) {
        guest_deallocate(arguments[0]);
        return 0;
    }
    if (import_is(name, "__ZNSt8ios_base4InitC1Ev") ||
        import_is(name, "__ZNSt8ios_base4InitD1Ev") ||
        import_is(name, "__ZNSaIcEC2Ev") ||
        import_is(name, "__ZNSaIcED2Ev")) {
        return 0;
    }
    if (import_is(name, "__ZNSsC2Ev")) {
        guest_string_construct(arguments[0], "", 0, return_address);
        return arguments[0];
    }
    if (import_is(name, "__ZNSsC1EPKcRKSaIcE")) {
        const char *source = (const char *)(uintptr_t)arguments[1];
        guest_string_construct(arguments[0], source,
                               source ? strlen(source) : 0, return_address);
        return arguments[0];
    }
    if (import_is(name, "__ZNSsC1ERKSs")) {
        const char *source = guest_string_data(arguments[1]);
        guest_string_construct(arguments[0], source,
                               guest_string_length(arguments[1]),
                               return_address);
        return arguments[0];
    }
    if (import_is(name, "__ZNSsD2Ev")) {
        guest_string_dispose_object(arguments[0]);
        return 0;
    }
    if (import_is(name, "__ZNSs4_Rep10_M_disposeERKSaIcE")) {
        guest_string_dispose_rep(arguments[0]);
        return 0;
    }
    if (import_is(name, "__ZNSs6assignEPKc")) {
        const char *source = (const char *)(uintptr_t)arguments[1];
        return guest_string_assign(arguments[0], source,
                                   source ? strlen(source) : 0,
                                   return_address);
    }
    if (import_is(name, "__ZNSs6assignERKSs")) {
        const char *source = guest_string_data(arguments[1]);
        return guest_string_assign(arguments[0], source,
                                   guest_string_length(arguments[1]),
                                   return_address);
    }
    if (import_is(name, "__ZNSs9push_backEc")) {
        const char *source = guest_string_data(arguments[0]);
        size_t length = guest_string_length(arguments[0]);
        uint32_t old_data = arguments[0] ?
            *(const uint32_t *)(uintptr_t)arguments[0] : 0;
        uint32_t data = guest_string_make(source, length + 1,
                                          return_address);
        if (!data) return 0;
        ((char *)(uintptr_t)data)[length] = (char)arguments[1];
        ((char *)(uintptr_t)data)[length + 1] = '\0';
        *(uint32_t *)(uintptr_t)arguments[0] = data;
        if (old_data >= 12 && old_data != data) {
            guest_string_dispose_rep(old_data - 12);
        }
        return arguments[0];
    }
    if (import_is(name, "__ZNSsixEm")) {
        uint32_t data = arguments[0] ? *(uint32_t *)(uintptr_t)arguments[0] : 0;
        return data ? data + arguments[1] : 0;
    }
    if (import_is(name, "__ZNKSs7compareEPKc")) {
        const char *right = (const char *)(uintptr_t)arguments[1];
        return (uint32_t)strcmp(guest_string_data(arguments[0]), right ? right : "");
    }

    if (import_is(name, "_memcpy") || import_is(name, "_memmove") ||
        import_is(name, "___memcpy_chk")) {
        return fast_memmove(arguments, return_address);
    }
    if (import_is(name, "_memset") || import_is(name, "___memset_chk")) {
        return fast_memset(arguments, return_address);
    }
    if (import_is(name, "_memcmp")) {
        return (uint32_t)memcmp((const void *)(uintptr_t)arguments[0],
                                (const void *)(uintptr_t)arguments[1], arguments[2]);
    }
    if (import_is(name, "_memchr")) {
        return (uint32_t)(uintptr_t)memchr((const void *)(uintptr_t)arguments[0],
                                           (int)arguments[1], arguments[2]);
    }
    if (import_is(name, "_strlen")) {
        return (uint32_t)strlen((const char *)(uintptr_t)arguments[0]);
    }
    if (import_is(name, "_strcmp")) {
        return (uint32_t)strcmp((const char *)(uintptr_t)arguments[0],
                                (const char *)(uintptr_t)arguments[1]);
    }
    if (import_is(name, "_strncmp")) {
        return (uint32_t)strncmp((const char *)(uintptr_t)arguments[0],
                                 (const char *)(uintptr_t)arguments[1], arguments[2]);
    }
    if (import_is(name, "_strcpy")) {
        strcpy((char *)(uintptr_t)arguments[0],
               (const char *)(uintptr_t)arguments[1]);
        return arguments[0];
    }
    if (import_is(name, "_strncpy")) {
        strncpy((char *)(uintptr_t)arguments[0],
                (const char *)(uintptr_t)arguments[1], arguments[2]);
        return arguments[0];
    }
    if (import_is(name, "_strcat")) {
        strcat((char *)(uintptr_t)arguments[0],
               (const char *)(uintptr_t)arguments[1]);
        return arguments[0];
    }
    if (import_is(name, "_strchr")) {
        return (uint32_t)(uintptr_t)strchr((const char *)(uintptr_t)arguments[0],
                                           (int)arguments[1]);
    }
    if (import_is(name, "_strrchr")) {
        return (uint32_t)(uintptr_t)strrchr((const char *)(uintptr_t)arguments[0],
                                            (int)arguments[1]);
    }
    if (import_is(name, "_strstr")) {
        return (uint32_t)(uintptr_t)strstr((const char *)(uintptr_t)arguments[0],
                                           (const char *)(uintptr_t)arguments[1]);
    }
    if (import_is(name, "_strtok")) {
        char *string = (char *)(uintptr_t)arguments[0];
        const char *delimiters = (const char *)(uintptr_t)arguments[1];
        if (!delimiters) return 0;
        return (uint32_t)(uintptr_t)strtok_r(string, delimiters,
                                            &guest_strtok_state);
    }
    if (import_is(name, "_strdup")) {
        const char *source = (const char *)(uintptr_t)arguments[0];
        if (!source) return 0;
        size_t size = strlen(source) + 1;
        uint32_t copy = guest_allocate_at(size, false, return_address);
        if (copy) memcpy((void *)(uintptr_t)copy, source, size);
        return copy;
    }
    if (import_is(name, "_stat") || import_is(name, "_fstat")) {
        const char *path = (const char *)(uintptr_t)arguments[0];
        struct guest_stat32 *guest = (void *)(uintptr_t)arguments[1];
        struct stat host;
        int status = !strcmp(name,"_fstat") ? fstat((int)arguments[0], &host) : stat(path, &host);
        if (status == 0 && guest) {
            memset(guest, 0, sizeof(*guest));
            guest->st_dev = host.st_dev;
            guest->st_ino = (uint32_t)host.st_ino;
            guest->st_mode = host.st_mode;
            guest->st_nlink = host.st_nlink;
            guest->st_uid = host.st_uid;
            guest->st_gid = host.st_gid;
            guest->st_rdev = host.st_rdev;
            guest->st_atime_sec = (int32_t)host.st_atimespec.tv_sec;
            guest->st_atime_nsec = (int32_t)host.st_atimespec.tv_nsec;
            guest->st_mtime_sec = (int32_t)host.st_mtimespec.tv_sec;
            guest->st_mtime_nsec = (int32_t)host.st_mtimespec.tv_nsec;
            guest->st_ctime_sec = (int32_t)host.st_ctimespec.tv_sec;
            guest->st_ctime_nsec = (int32_t)host.st_ctimespec.tv_nsec;
            guest->st_size = host.st_size;
            guest->st_blocks = host.st_blocks;
            guest->st_blksize = host.st_blksize;
            guest->st_flags = host.st_flags;
            guest->st_gen = host.st_gen;
        }
        return (uint32_t)status;
    }
    if (import_is(name, "_getrusage")) {
        /* i386 struct rusage: ru_utime/ru_stime (two 8-byte timevals) then 14
           longs = 72 bytes.  Fill the CPU times; the rest is profiling detail
           the game only logs. */
        struct rusage host;
        int status = getrusage((int)arguments[0], &host);
        uint32_t *guest = (void *)(uintptr_t)arguments[1];
        if (guest) {
            memset(guest, 0, 72);
            if (status == 0) {
                guest[0] = (uint32_t)host.ru_utime.tv_sec;
                guest[1] = (uint32_t)host.ru_utime.tv_usec;
                guest[2] = (uint32_t)host.ru_stime.tv_sec;
                guest[3] = (uint32_t)host.ru_stime.tv_usec;
            }
        }
        return (uint32_t)(status == 0 ? 0 : -1);
    }
    if (import_is(name, "_gettimeofday")) {
        struct timeval host;
        int status = gettimeofday(&host, NULL);
        if (status == 0 && arguments[0]) {
            struct guest_timeval32 *guest =
                (void *)(uintptr_t)arguments[0];
            guest->tv_sec = (int32_t)host.tv_sec;
            guest->tv_usec = (int32_t)host.tv_usec;
        }
        /* struct timezone is obsolete; preserve the legacy ABI with zeros. */
        if (status == 0 && arguments[1]) {
            uint32_t *guest_timezone = (void *)(uintptr_t)arguments[1];
            guest_timezone[0] = 0;
            guest_timezone[1] = 0;
        }
        return (uint32_t)status;
    }
    if (import_is(name, "_localtime_r")) {
        const int32_t *guest_time =
            (const void *)(uintptr_t)arguments[0];
        struct guest_tm32 *guest_result =
            (void *)(uintptr_t)arguments[1];
        if (!guest_time || !guest_result) return 0;
        time_t host_time = (time_t)*guest_time;
        struct tm host_result;
        if (!localtime_r(&host_time, &host_result)) return 0;
        return copy_host_tm_to_guest(&host_result, guest_result);
    }
    if (import_is(name, "_sysctlbyname")) {
        const char *sysctl_name = (const char *)(uintptr_t)arguments[0];
        void *old_value = (void *)(uintptr_t)arguments[1];
        uint32_t *guest_old_size = (void *)(uintptr_t)arguments[2];
        size_t old_size = guest_old_size ? *guest_old_size : 0;
        void *new_value = (void *)(uintptr_t)arguments[3];
        size_t new_size = arguments[4];
        int status = sysctlbyname(sysctl_name, old_value,
                                  guest_old_size ? &old_size : NULL,
                                  new_value, new_size);
        if (guest_old_size) *guest_old_size = (uint32_t)old_size;
        return (uint32_t)status;
    }
    if (import_is(name, "_qsort")) {
        if (arguments[1] < 2 || arguments[2] == 0) return 0;
        if (getenv("LP32_TRACE_QSORT")) {
            fprintf(stderr, "compat32: qsort base=0x%08x count=%u width=%u "
                            "comparator=0x%08x first=0x%08x\n",
                    arguments[0], arguments[1], arguments[2], arguments[3],
                    *(const uint32_t *)(uintptr_t)arguments[0]);
        }
        guest_qsort((void *)(uintptr_t)arguments[0], arguments[1], arguments[2],
                    arguments[3]);
        return 0;
    }
    if (import_is(name, "_vswprintf") || import_is(name, "_swprintf")) {
        /* Wide printf.  Left to bind to the host, the i386 va_list ABI does not
           match and formatted wide output is corrupted (Bejeweled 3's Classic
           level number rendered as "1sf"/"2sf").  Narrow the wide format and
           reuse guest_vformat, then widen the result into the guest buffer.
           swprintf takes inline varargs; vswprintf takes a va_list pointer. */
        wchar_t *destination = (void *)(uintptr_t)arguments[0];
        size_t destination_size = arguments[1];
        const uint32_t *wide_format = (const void *)(uintptr_t)arguments[2];
        const uint32_t *values = import_is(name, "_vswprintf")
            ? (const uint32_t *)(uintptr_t)arguments[3]
            : arguments + 3;
        if (!destination || !wide_format || !values) return (uint32_t)-1;
        char format[2048];
        size_t fi = 0;
        for (; fi + 1 < sizeof(format) && wide_format[fi]; ++fi)
            format[fi] = (char)wide_format[fi];
        format[fi] = '\0';
        char *scratch = malloc(65536);
        if (!scratch) return (uint32_t)-1;
        int formatted = guest_vformat(scratch, 65536, format, values);
        if (destination_size) {
            size_t j = 0;
            for (; j + 1 < destination_size && scratch[j]; ++j)
                destination[j] = (wchar_t)(unsigned char)scratch[j];
            destination[j] = 0;
        }
        free(scratch);
        return (uint32_t)formatted;
    }
    if (import_is(name, "_sprintf") || import_is(name, "_snprintf") ||
        import_is(name, "_vsprintf") || import_is(name, "_vsnprintf") ||
        import_is(name, "_printf")) {
        char *destination = NULL;
        size_t destination_size = 65536;
        const char *format;
        const uint32_t *values;
        bool temporary = import_is(name, "_printf");
        if (import_is(name, "_sprintf")) {
            destination = (void *)(uintptr_t)arguments[0];
            format = (const void *)(uintptr_t)arguments[1];
            values = arguments + 2;
        } else if (import_is(name, "_snprintf")) {
            destination = (void *)(uintptr_t)arguments[0];
            destination_size = arguments[1];
            format = (const void *)(uintptr_t)arguments[2];
            values = arguments + 3;
        } else if (import_is(name, "_vsprintf")) {
            destination = (void *)(uintptr_t)arguments[0];
            format = (const void *)(uintptr_t)arguments[1];
            values = (const void *)(uintptr_t)arguments[2];
        } else if (import_is(name, "_vsnprintf")) {
            destination = (void *)(uintptr_t)arguments[0];
            destination_size = arguments[1];
            format = (const void *)(uintptr_t)arguments[2];
            values = (const void *)(uintptr_t)arguments[3];
        } else {
            format = (const void *)(uintptr_t)arguments[0];
            values = arguments + 1;
        }

        char *scratch = temporary || import_is(name, "_sprintf") ||
                        import_is(name, "_vsprintf") ? malloc(65536) : destination;
        if (!scratch || !format || !values) {
            if (scratch && scratch != destination) free(scratch);
            return (uint32_t)-1;
        }
        int formatted = guest_vformat(scratch,
                                      scratch == destination ? destination_size : 65536,
                                      format, values);
        if (getenv("LP32_TRACE_RESOLUTION") && formatted >= 0 &&
            (strstr(scratch, " x ") || strstr(format, " x "))) {
            fprintf(stderr,
                    "compat32: resolution format caller=0x%08" PRIx32
                    " format=\"%s\" output=\"%s\"\n",
                    return_address, format, scratch);
        }
        if (!temporary && scratch != destination && destination) strcpy(destination, scratch);
        if (temporary && formatted >= 0) fputs(scratch, stdout);
        if (scratch != destination) free(scratch);
        return (uint32_t)formatted;
    }
    if (import_is(name, "_sscanf")) {
        const char *input = (const void *)(uintptr_t)arguments[0];
        const char *format = (const void *)(uintptr_t)arguments[1];
        return input && format ? (uint32_t)guest_vscan(input, format,
                                                        arguments + 2) : 0;
    }
    if (import_is(name, "_swscanf")) {
        /* Bejeweled 3 parses numeric values (leaderboard/config) from wstrings.
           The input and format are ASCII wchar_t (4-byte) strings; narrow them
           and reuse the guest sscanf scanner.  Numeric conversions match
           regardless of char width; the 'l' length modifier is dropped so a
           %ls/%lc collapses to narrow (adequate for the ASCII data seen). */
        const uint32_t *wide_input = (const void *)(uintptr_t)arguments[0];
        const uint32_t *wide_format = (const void *)(uintptr_t)arguments[1];
        if (!wide_input || !wide_format) return 0;
        char input[2048];
        char format[512];
        size_t n = 0;
        for (; n + 1 < sizeof(input) && wide_input[n]; ++n)
            input[n] = (char)wide_input[n];
        input[n] = '\0';
        size_t out = 0;
        for (size_t in = 0; out + 1 < sizeof(format) && wide_format[in]; ++in) {
            uint32_t c = wide_format[in];
            if (c == 'l' && (wide_format[in + 1] == 's' || wide_format[in + 1] == 'c'))
                continue; /* drop the wide-output modifier */
            format[out++] = (char)c;
        }
        format[out] = '\0';
        return (uint32_t)guest_vscan(input, format, arguments + 2);
    }
    if (import_is(name, "_fopen")) {
        const char *path = (const char *)(uintptr_t)arguments[0];
        const char *mode = (const char *)(uintptr_t)arguments[1];
        FILE *file = fopen(path, mode);
        if (!file && getenv("LP32_TRACE_FILES")) {
            fprintf(stderr, "compat32: fopen failed: %s (%s) from 0x%08" PRIx32 "\n",
                    path ? path : "(null)", strerror(errno), return_address);
        } else if (file && getenv("LP32_TRACE_FILES") &&
                   getenv("LP32_TRACE_FILES")[0] == '2') {
            fprintf(stderr, "compat32: fopen %s (%s) from 0x%08" PRIx32 "\n",
                    path ? path : "(null)", mode ? mode : "", return_address);
        }
        if (!file && errno == ENOENT && path && mode && mode[0] == 'r' &&
            path_is_game_config(path)) {
            game_config_missing = 1;
        }
        return guest_handle_for_file(file);
    }
    if (import_is(name, "_fclose")) {
        return (uint32_t)guest_close_file(arguments[0]);
    }
    if (import_is(name, "_fread")) {
        FILE *file = lock_host_file_for_guest(arguments[3]);
        if (!file) return 0;
        uint32_t result = (uint32_t)fread((void *)(uintptr_t)arguments[0],
                                          arguments[1], arguments[2], file);
        unlock_host_file();
        return result;
    }
    if (import_is(name, "_fwrite") || import_is(name, "_fwrite$UNIX2003")) {
        FILE *file = lock_host_file_for_guest(arguments[3]);
        if (!file) return 0;
        uint32_t result = (uint32_t)fwrite(
            (const void *)(uintptr_t)arguments[0], arguments[1], arguments[2], file);
        unlock_host_file();
        return result;
    }
    if (import_is(name, "_fputs")) {
        FILE *file = lock_host_file_for_guest(arguments[1]);
        const char *s = (const char *)(uintptr_t)arguments[0];
        if (!file) return (uint32_t)-1;
        int result = fputs(s ? s : "", file);
        unlock_host_file();
        return (uint32_t)result;
    }
    if (import_is(name, "_fputc")) {
        FILE *file = lock_host_file_for_guest(arguments[1]);
        if (!file) return (uint32_t)-1;
        int result = fputc((int)arguments[0], file);
        unlock_host_file();
        return (uint32_t)result;
    }
    if (import_is(name, "_fputws")) {
        FILE *file = lock_host_file_for_guest(arguments[1]);
        const wchar_t *ws = (const wchar_t *)(uintptr_t)arguments[0];
        if (!file) return (uint32_t)-1;
        int result = fputws(ws ? ws : L"", file);
        unlock_host_file();
        return (uint32_t)result;
    }
    if (import_is(name, "_fseek")) {
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file) return (uint32_t)-1;
        uint32_t result = (uint32_t)fseek(file, (int32_t)arguments[1],
                                          (int)arguments[2]);
        unlock_host_file();
        return result;
    }
    if (import_is(name, "_ftell")) {
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file) return (uint32_t)-1;
        long offset = ftell(file);
        unlock_host_file();
        return (uint32_t)offset;
    }
    if (import_is(name, "_ferror")) {
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file) return 1;
        uint32_t result = (uint32_t)ferror(file);
        unlock_host_file();
        return result;
    }
    if (import_is(name, "_fileno")) {
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file) return (uint32_t)-1;
        uint32_t result = (uint32_t)fileno(file);
        unlock_host_file();
        return result;
    }
    if (import_is(name, "_clearerr")) {
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file) return 0;
        clearerr(file);
        unlock_host_file();
        return 0;
    }
    if (import_is(name, "_getc")) {
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file) return (uint32_t)EOF;
        uint32_t result = (uint32_t)getc(file);
        unlock_host_file();
        return result;
    }
    if (import_is(name, "_fprintf") || import_is(name,"_vfprintf")) {
        const char *format = (const void *)(uintptr_t)arguments[1];
        char *scratch = malloc(65536);
        if (!format || !scratch) {
            free(scratch);
            return (uint32_t)-1;
        }
        int formatted = guest_vformat(scratch, 65536, format, !strcmp(name,"_vfprintf") ? (const uint32_t *)(uintptr_t)arguments[2] : arguments + 2);
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file) {
            free(scratch);
            return (uint32_t)-1;
        }
        if (formatted >= 0) fputs(scratch, file);
        unlock_host_file();
        free(scratch);
        return (uint32_t)formatted;
    }
    if (import_is(name, "_opendir")) {
        const char *path = (const char *)(uintptr_t)arguments[0];
        DIR *directory = path ? opendir(path) : NULL;
        return guest_handle_for_directory(directory);
    }
    if (import_is(name, "_readdir")) {
        pthread_mutex_lock(&guest_directory_lock);
        struct guest_directory_entry *entry =
            guest_directory_for_handle(arguments[0]);
        struct dirent *host = entry ? readdir(entry->directory) : NULL;
        uint32_t result = 0;
        if (host && entry->guest_dirent) {
            struct guest_dirent32 *guest =
                (void *)(uintptr_t)entry->guest_dirent;
            size_t name_length = strnlen(host->d_name, sizeof(guest->d_name) - 1);
            memset(guest, 0, sizeof(*guest));
            guest->d_ino = (uint32_t)host->d_ino;
            guest->d_reclen = (uint16_t)((offsetof(struct guest_dirent32, d_name) +
                                          name_length + 1 + 3) & ~3u);
            guest->d_type = host->d_type;
            guest->d_namlen = (uint8_t)name_length;
            memcpy(guest->d_name, host->d_name, name_length);
            guest->d_name[name_length] = '\0';
            result = entry->guest_dirent;
        }
        pthread_mutex_unlock(&guest_directory_lock);
        return result;
    }
    if (import_is(name, "_closedir")) {
        pthread_mutex_lock(&guest_directory_lock);
        struct guest_directory_entry *entry =
            guest_directory_for_handle(arguments[0]);
        int status = -1;
        if (entry) {
            status = closedir(entry->directory);
            entry->directory = NULL;
            entry->handle = 0;
        } else {
            errno = EBADF;
        }
        pthread_mutex_unlock(&guest_directory_lock);
        return (uint32_t)status;
    }
    if (import_is(name, "_mkdir")) {
        return (uint32_t)mkdir((const char *)(uintptr_t)arguments[0],
                               (mode_t)arguments[1]);
    }
    if (import_is(name, "_remove")) {
        return (uint32_t)remove((const char *)(uintptr_t)arguments[0]);
    }
    if (import_is(name, "_rename")) {
        return (uint32_t)rename((const char *)(uintptr_t)arguments[0],
                                (const char *)(uintptr_t)arguments[1]);
    }
    if (import_is(name, "_rmdir")) {
        return (uint32_t)rmdir((const char *)(uintptr_t)arguments[0]);
    }
    if (import_is(name, "_unlink")) {
        return (uint32_t)unlink((const char *)(uintptr_t)arguments[0]);
    }
    if (import_is(name, "_tmpfile")) {
        return guest_handle_for_file(tmpfile());
    }
    if (import_is(name, "_atoi")) {
        return (uint32_t)atoi((const char *)(uintptr_t)arguments[0]);
    }
    if (import_is(name, "___toupper")) {
        return arguments[0] <= UCHAR_MAX ?
            (uint32_t)toupper((unsigned char)arguments[0]) : arguments[0];
    }
    if (import_is(name, "___tolower")) {
        return arguments[0] <= UCHAR_MAX ?
            (uint32_t)tolower((unsigned char)arguments[0]) : arguments[0];
    }
    if (import_is(name, "_rand")) {
        /* Darwin exposes RAND_MAX == INT_MAX.  A 15-bit ANSI/Windows-style
           result makes the game's rejection samplers impossible to satisfy. */
        return (uint32_t)rand();
    }
    if (import_is(name, "_srand")) {
        srand(arguments[0]);
        return 0;
    }
    if (import_is(name, "___udivdi3") || import_is(name, "___umoddi3")) {
        uint64_t dividend = (uint64_t)arguments[0] |
            ((uint64_t)arguments[1] << 32);
        uint64_t divisor = (uint64_t)arguments[2] |
            ((uint64_t)arguments[3] << 32);
        if (!divisor) return 0;
        return import_is(name, "___udivdi3") ?
            dividend / divisor : dividend % divisor;
    }
    if (import_is(name, "___moddi3")) {
        int64_t dividend = (int64_t)((uint64_t)arguments[0] |
            ((uint64_t)arguments[1] << 32));
        int64_t divisor = (int64_t)((uint64_t)arguments[2] |
            ((uint64_t)arguments[3] << 32));
        return divisor ? (uint64_t)(dividend % divisor) : 0;
    }
    if (import_is(name, "___error")) {
        /* Bejeweled 3 imports the __error() accessor but not the _errno data
           symbol, so guest_errno_address is never assigned by the pointer-bind
           loop; lazily reserve a guest-writable errno cell so *__error() is a
           valid location (the game clears and tests it around file/sound I/O). */
        if (!guest_errno_address)
            guest_errno_address = guest_allocate_at(sizeof(uint32_t), true,
                                                    kGuestHeapHostSite);
        return guest_errno_address;
    }
    if (import_is(name, "_mach_absolute_time")) {
        uint64_t value = mach_absolute_time();
        static uint32_t traced_clocks;
        if (timing_trace && traced_clocks++ < 32) {
            fprintf(stderr,
                    "compat32: mach_absolute_time=%" PRIu64
                    " from 0x%08" PRIx32 "\n",
                    value, return_address);
        }
        return value;
    }
    if (import_is(name, "_mach_timebase_info")) {
        mach_timebase_info_data_t host_info;
        kern_return_t result = mach_timebase_info(&host_info);
        static uint32_t traced_timebases;
        if (timing_trace && traced_timebases++ < 16) {
            fprintf(stderr, "compat32: mach_timebase_info=%u/%u\n",
                    host_info.numer, host_info.denom);
        }
        if (result == KERN_SUCCESS && arguments[0]) {
            uint32_t *guest_info = (void *)(uintptr_t)arguments[0];
            guest_info[0] = host_info.numer;
            guest_info[1] = host_info.denom;
        }
        return (uint32_t)result;
    }
    if (import_is(name, "_task_policy_set") || import_is(name, "_setpriority")) {
        return 0;
    }
    if (import_is(name, "_UpdateSystemActivity")) return 0;
    if (import_is(name, "_sched_get_priority_max")) return 0;
    if (import_is(name, "_usleep")) {
        static uint32_t traced_sleeps;
        if (timing_trace && traced_sleeps++ < 12) {
            fprintf(stderr, "compat32: usleep(%" PRIu32 ") from 0x%08" PRIx32 "\n",
                    arguments[0], return_address);
        }
        /* usleep(3) returns early on any signal; the guest ignores the
           result, so an interrupted sleep would shorten its frame limiter.
           Sleep the full request regardless. */
        struct timespec request = {
            (time_t)(arguments[0] / 1000000u),
            (long)(arguments[0] % 1000000u) * 1000L,
        };
        while (nanosleep(&request, &request) != 0 && errno == EINTR) {
        }
        return 0;
    }
    if (import_is(name, "_OSAtomicAdd32") ||
        import_is(name, "_OSAtomicAdd32Barrier")) {
        return fast_OSAtomicAdd32(arguments, return_address);
    }
    if (import_is(name, "_OSAtomicCompareAndSwap32")) {
        return fast_OSAtomicCompareAndSwap32(arguments, return_address);
    }
    if (import_is(name, "_pthread_once")) {
        return guest_pthread_once(arguments[0], arguments[1]);
    }
    if (import_is(name, "_chdir")) {
        const char *path = (const char *)(uintptr_t)arguments[0];
        return path ? (uint32_t)chdir(path) : (uint32_t)-1;
    }
    if (import_is(name, "_fcntl")) {
        /* F_GETFD/F_SETFD/F_GETFL/F_SETFL carry an int; F_GETPATH and the
           lock commands carry a pointer, which is a 32-bit guest address
           the host can use directly. */
        int command = (int)arguments[1];
        if (command == F_GETFD || command == F_SETFD || command == F_GETFL ||
            command == F_SETFL || command == F_DUPFD || command == F_NOCACHE ||
            command == F_RDAHEAD || command == F_FULLFSYNC) {
            return (uint32_t)fcntl((int)arguments[0], command, (int)arguments[2]);
        }
        return (uint32_t)fcntl((int)arguments[0], command,
                               (void *)(uintptr_t)arguments[2]);
    }
    if (import_is(name, "_fscanf")) {
        const char *format = (const void *)(uintptr_t)arguments[1];
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file || !format) {
            if (file) unlock_host_file();
            return (uint32_t)EOF;
        }
        uint32_t result = (uint32_t)guest_fscan(file, format, arguments + 2);
        unlock_host_file();
        return result;
    }

    if (import_is(name, "_logf")) return fast_logf(arguments, return_address);
    if (import_is(name, "_log10f")) return return_guest_float(log10f(guest_float(arguments[0])));
    if (import_is(name, "_expf")) return fast_expf(arguments, return_address);
    if (import_is(name, "_sinf")) return return_guest_float(sinf(guest_float(arguments[0])));
    if (import_is(name, "_cosf")) return return_guest_float(cosf(guest_float(arguments[0])));
    if (import_is(name, "_ceilf")) return return_guest_float(ceilf(guest_float(arguments[0])));

    if (import_is(name, "_log")) return return_guest_double(log(guest_double(arguments)));
    if (import_is(name, "_exp")) return return_guest_double(exp(guest_double(arguments)));
    if (import_is(name, "_sin")) return return_guest_double(sin(guest_double(arguments)));
    if (import_is(name, "_cos")) return return_guest_double(cos(guest_double(arguments)));
    if (import_is(name, "_atan")) return return_guest_double(atan(guest_double(arguments)));
    if (import_is(name, "_fmod")) {
        return return_guest_double(fmod(guest_double(arguments),
                                        guest_double(arguments + 2)));
    }
    if (import_is(name, "_ceil")) return return_guest_double(ceil(guest_double(arguments)));
    if (import_is(name, "_floor")) return return_guest_double(floor(guest_double(arguments)));
    if (import_is(name, "_rint")) return return_guest_double(rint(guest_double(arguments)));
    if (import_is(name, "_ldexp")) {
        return return_guest_double(ldexp(guest_double(arguments), (int)arguments[2]));
    }
    if (import_is(name, "_pow")) {
        return return_guest_double(pow(guest_double(arguments),
                                       guest_double(arguments + 2)));
    }
    if (import_is(name, "_fmod")) {
        return return_guest_double(fmod(guest_double(arguments),
                                        guest_double(arguments + 2)));
    }
    if (import_is(name, "_difftime")) {
        return return_guest_double((double)(int32_t)arguments[0] -
                                   (double)(int32_t)arguments[1]);
    }
    if (import_is(name, "_CFAbsoluteTimeGetCurrent")) {
        return return_guest_double(CFAbsoluteTimeGetCurrent());
    }

    if (import_is(name, "_pthread_mutexattr_init") ||
        import_is(name, "_pthread_mutexattr_destroy") ||
        import_is(name, "_pthread_mutexattr_settype") ||
        import_is(name, "_pthread_attr_init") ||
        import_is(name, "_pthread_attr_destroy") ||
        import_is(name, "_pthread_attr_setstacksize") ||
        import_is(name, "_pthread_setschedparam")) {
        return 0;
    }
    if (import_is(name, "_pthread_create")) {
        struct guest_thread_context *context = malloc(sizeof(*context));
        if (!context) return (uint32_t)ENOMEM;
        context->function = arguments[2];
        {
            /* The engine's pthread_create site hands the thread entry the address of a
               stack-local task-record pointer.  Native pthread scheduling
               consumed it immediately; a translated host thread can start
               after that slot is reused.  Snapshot the word into stable
               guest memory. */
            uint32_t stable_argument = guest_allocate(sizeof(uint32_t), false);
            if (!stable_argument) {
                free(context);
                return (uint32_t)ENOMEM;
            }
            *(uint32_t *)(uintptr_t)stable_argument =
                *(const uint32_t *)(uintptr_t)arguments[3];
            context->argument = stable_argument;
            if (getenv("LP32_TRACE_THREADS")) {
                uint32_t record = *(uint32_t *)(uintptr_t)stable_argument;
                uint64_t thread_id = 0;
                pthread_threadid_np(NULL, &thread_id);
                fprintf(stderr,
                        "compat32: pthread_create from tid=%llu ra=0x%08x "
                        "entry=0x%08x arg=0x%08x record=0x%08x fn=0x%08x "
                        "name=%.31s\n",
                        (unsigned long long)thread_id, return_address,
                        arguments[2], arguments[3], record,
                        record ? *(uint32_t *)(uintptr_t)record : 0,
                        record ? (const char *)(uintptr_t)(record + 8) : "");
            }
        }
        pthread_t thread;
        int status = pthread_create(&thread, NULL, run_guest_thread, context);
        if (status != 0) {
            free(context);
            return (uint32_t)status;
        }
        uint32_t *guest_thread = (void *)(uintptr_t)arguments[0];
        if (guest_thread) {
            static uint32_t next_handle = 1;
            *guest_thread = __atomic_fetch_add(&next_handle, 1, __ATOMIC_RELAXED);
        }
        return 0;
    }
    if (import_is(name, "_pthread_mutex_init")) {
        return guest_mutex_initialize(arguments[0]);
    }
    if (import_is(name, "_pthread_mutex_destroy")) {
        return guest_mutex_destroy(arguments[0]);
    }
    if (import_is(name, "_pthread_mutex_lock")) {
        return fast_pthread_mutex_lock(arguments, return_address);
    }
    if (import_is(name, "_pthread_mutex_trylock")) {
        return fast_pthread_mutex_trylock(arguments, return_address);
    }
    if (import_is(name, "_pthread_mutex_unlock")) {
        return fast_pthread_mutex_unlock(arguments, return_address);
    }
    if (import_is(name, "_pthread_cond_init")) {
        return guest_cond_initialize(arguments[0]);
    }
    if (import_is(name, "_pthread_cond_destroy")) {
        return guest_cond_destroy(arguments[0]);
    }
    if (import_is(name, "_pthread_cond_signal")) {
        pthread_cond_t *condition = host_cond_for_guest(arguments[0], true);
        return condition ? (uint32_t)pthread_cond_signal(condition) : (uint32_t)ENOMEM;
    }
    if (import_is(name, "_pthread_cond_broadcast")) {
        pthread_cond_t *condition = host_cond_for_guest(arguments[0], true);
        return condition ? (uint32_t)pthread_cond_broadcast(condition) : (uint32_t)ENOMEM;
    }
    if (import_is(name, "_pthread_cond_wait")) {
        pthread_cond_t *condition = host_cond_for_guest(arguments[0], true);
        pthread_mutex_t *mutex = host_mutex_for_guest(arguments[1], false);
        if (!condition || !mutex) return (uint32_t)EINVAL;
        /* If the guest's frame/main thread is the one blocking (e.g.
           CoreAudioSoundInstance::Release waiting for the audio render
           callback to run DoPostRenderMaintenance), let the audio hold yield
           so that callback can proceed; otherwise the hold — which keys off
           the frame clock the blocked thread would advance — never ends. */
        bool frame_thread = audio_bridge32_is_frame_thread();
        if (frame_thread) audio_bridge32_frame_thread_wait_begin();
        uint32_t r = (uint32_t)pthread_cond_wait(condition, mutex);
        if (frame_thread) audio_bridge32_frame_thread_wait_end();
        return r;
    }
    if (import_is(name, "_pthread_exit")) {
        /* Terminate the current host thread that is running this guest thread
           (guest worker threads are host pthreads); mirrors returning from the
           thread's start routine. */
        pthread_exit((void *)(uintptr_t)arguments[0]);
        return 0; /* not reached */
    }
    if (import_is(name, "_pthread_cond_timedwait")) {
        pthread_cond_t *condition = host_cond_for_guest(arguments[0], true);
        pthread_mutex_t *mutex = host_mutex_for_guest(arguments[1], false);
        if (!condition || !mutex) return (uint32_t)EINVAL;
        /* i386 struct timespec is two 32-bit fields; widen to the host's
           16-byte layout.  The guest computes an absolute CLOCK_REALTIME
           deadline from the host-backed gettimeofday, so it is already in
           host terms. */
        const int32_t *guest_time = (const void *)(uintptr_t)arguments[2];
        struct timespec deadline = {0, 0};
        if (guest_time) {
            deadline.tv_sec = (time_t)guest_time[0];
            deadline.tv_nsec = (long)guest_time[1];
        }
        return (uint32_t)pthread_cond_timedwait(condition, mutex, &deadline);
    }
    if (import_is(name, "_pthread_key_create")) {
        uint32_t *key = (void *)(uintptr_t)arguments[0];
        uint32_t next = __atomic_fetch_add(&guest_pthread_next_key, 1,
                                           __ATOMIC_RELAXED);
        if (next >= 128) return (uint32_t)EAGAIN;
        *key = next;
        return 0;
    }
    if (import_is(name, "_pthread_setspecific")) {
        if (arguments[0] >= 128) return (uint32_t)EINVAL;
        guest_pthread_values[arguments[0]] = arguments[1];
        return 0;
    }
    if (import_is(name, "_pthread_getspecific")) {
        return arguments[0] < 128 ? guest_pthread_values[arguments[0]] : 0;
    }

    if (import_is(name, "_MPCreateSemaphore")) {
        uint32_t handle = guest_semaphore_create((int32_t)arguments[0],
                                                 (int32_t)arguments[1]);
        if (!handle) return (uint32_t)-1;
        uint32_t *output = (void *)(uintptr_t)arguments[2];
        if (output) *output = handle;
        return 0;
    }
    if (import_is(name, "_MPDeleteSemaphore")) {
        struct guest_semaphore *semaphore =
            guest_semaphore_acquire(arguments[0]);
        if (!semaphore) return (uint32_t)-1;
        semaphore->initialized = false;
        ++semaphore->generation;
        pthread_cond_broadcast(&semaphore->condition);
        pthread_mutex_unlock(&semaphore->mutex);
        __atomic_fetch_sub(&guest_semaphore_live_count, 1, __ATOMIC_RELAXED);
        return 0;
    }
    if (import_is(name, "_MPSignalSemaphore")) {
        struct guest_semaphore *semaphore =
            guest_semaphore_acquire(arguments[0]);
        if (!semaphore) return (uint32_t)-1;
        if (semaphore->count < semaphore->maximum) ++semaphore->count;
        pthread_cond_signal(&semaphore->condition);
        pthread_mutex_unlock(&semaphore->mutex);
        return 0;
    }
    if (import_is(name, "_MPWaitOnSemaphore")) {
        struct guest_semaphore *semaphore =
            guest_semaphore_acquire(arguments[0]);
        if (!semaphore) return (uint32_t)-1;
        uint32_t generation = semaphore->generation;
        if ((int32_t)arguments[1] == 0 && semaphore->count <= 0) {
            pthread_mutex_unlock(&semaphore->mutex);
            return (uint32_t)-1;
        }
        while (semaphore->initialized &&
               semaphore->generation == generation && semaphore->count <= 0) {
            pthread_cond_wait(&semaphore->condition, &semaphore->mutex);
        }
        if (!semaphore->initialized || semaphore->generation != generation) {
            pthread_mutex_unlock(&semaphore->mutex);
            return (uint32_t)-1;
        }
        --semaphore->count;
        pthread_mutex_unlock(&semaphore->mutex);
        return 0;
    }

    if (import_is(name, "_Gestalt")) {
        int32_t *response = (void *)(uintptr_t)arguments[1];
        int32_t value;
        switch (arguments[0]) {
            case UINT32_C(0x73797331): value = 10; break; /* sys1: major */
            case UINT32_C(0x73797332): value = 7; break;  /* sys2: minor */
            case UINT32_C(0x73797333): value = 5; break;  /* sys3: bugfix */
            default: return (uint32_t)-5551;              /* gestaltUndefSelectorErr */
        }
        if (response) *response = value;
        return 0;
    }

    if (import_is(name, "_IOServiceMatching") ||
        import_is(name, "_IOIteratorNext") ||
        import_is(name, "_IONotificationPortCreate") ||
        import_is(name, "_IONotificationPortGetRunLoopSource")) return 0;
    if (import_is(name, "_IOObjectRelease") ||
        import_is(name, "_IODestroyPlugInInterface")) return 0;
    if (import_is(name, "_IOMasterPort")) {
        uint32_t *port = (void *)(uintptr_t)arguments[1];
        if (port) *port = 0;
        return 0;
    }
    if (import_is(name, "_IORegistryEntryCreateCFProperties")) {
        uint32_t *properties = (void *)(uintptr_t)arguments[1];
        if (properties) *properties = 0;
        return UINT32_C(0xe00002c7);
    }
    if (import_is(name, "_IORegistryEntryGetParentEntry")) {
        uint32_t *parent = (void *)(uintptr_t)arguments[2];
        if (parent) *parent = 0;
        return UINT32_C(0xe00002c7);
    }
    if (import_is(name, "_IOCreatePlugInInterfaceForService")) {
        uint32_t *plugin = (void *)(uintptr_t)arguments[3];
        uint32_t *score = (void *)(uintptr_t)arguments[4];
        if (plugin) *plugin = 0;
        if (score) *score = 0;
        return UINT32_C(0xe00002c7);
    }
    if (import_is(name, "_IOServiceAddInterestNotification") ||
        import_is(name, "_IOServiceAddMatchingNotification")) return UINT32_C(0xe00002c7);
    enum {
        kPowerRootConnection = 0x504d5201u, /* 'PMR\1' sentinel io_connect_t */
        kPowerNotificationPort = 0x504d5202u,
        kPowerNotifier = 0x504d5203u,
    };
    /* Sleep/wake notifications (Peggle Nights registers for these during
       SexyAppBase::Init and ABORTS init if this returns MACH_PORT_NULL).
       Return a non-zero sentinel connection and fabricate a notification port
       so the game proceeds; the run-loop source it fetches is a no-op below, so
       no sleep hooks actually fire. */
    if (import_is(name, "_IORegisterForSystemPower")) {
        uint32_t *port_ref = (void *)(uintptr_t)arguments[1];
        uint32_t *notifier = (void *)(uintptr_t)arguments[3];
        if (port_ref) *port_ref = kPowerNotificationPort;
        if (notifier) *notifier = kPowerNotifier;
        return kPowerRootConnection; /* non-zero io_connect_t */
    }
    if (import_is(name, "_IODeregisterForSystemPower") ||
        import_is(name, "_IOAllowPowerChange") ||
        import_is(name, "_IOCancelPowerChange")) return 0;
    /* Steamworks (Bejeweled 3 links libsteam_api.dylib for stats/achievements/
       cloud, all optional).  Report Steam unavailable so the game runs without
       it; the interface accessors return null and are never used because Init
       reports failure.  The game does not gate launch on Steam. */
    if (import_is(name, "_SteamAPI_Init") ||
        import_is(name, "_SteamAPI_InitSafe")) return 1;      /* true: pretend Steam is up */
    if (import_is(name, "_SteamAPI_Shutdown") ||
        import_is(name, "_SteamAPI_RunCallbacks") ||
        import_is(name, "_SteamAPI_RegisterCallback") ||
        import_is(name, "_SteamAPI_UnregisterCallback") ||
        import_is(name, "_SteamAPI_RegisterCallResult") ||
        import_is(name, "_SteamAPI_UnregisterCallResult")) return 0;
    if (import_is(name, "_pg_steam_noop")) return 0;          /* stub vtable method */
    if (import_is(name, "_SteamClient") || import_is(name, "_SteamUser") ||
        import_is(name, "_SteamUserStats") || import_is(name, "_SteamUtils") ||
        import_is(name, "_SteamFriends") || import_is(name, "_SteamApps") ||
        import_is(name, "_SteamRemoteStorage")) {
        /* Non-null stub interface: the game calls methods on the returned
           pointer without null-checking.  Hand back an object whose vtable
           slots are all a no-op thunk that returns 0 (bool/int/pointer/64-bit
           all read as 0), so Steam stats/achievements calls are harmless. */
        static uint32_t stub_interface;
        if (!stub_interface) {
            uint32_t thunk = compat_runtime32_guest_callback("_pg_steam_noop");
            uint32_t vtable = compat_runtime32_allocate(512 * sizeof(uint32_t), 1);
            uint32_t *slots = (uint32_t *)(uintptr_t)vtable;
            for (int i = 0; i < 512; ++i) slots[i] = thunk;
            stub_interface = compat_runtime32_allocate(16, 1);
            *(uint32_t *)(uintptr_t)stub_interface = vtable;
        }
        return stub_interface;
    }
    if (import_is(name, "_CFRunLoopAddSource") ||
        import_is(name, "_CFRunLoopRemoveSource")) return 0;
    if (import_is(name, "_CFRunLoopContainsSource")) return 0;

    if (import_is(name, "___cxa_guard_acquire")) {
        uint8_t *guard = (void *)(uintptr_t)arguments[0];
        if (*guard) return 0;
        guard[1] = 1;
        return 1;
    }
    if (import_is(name, "___cxa_guard_release")) {
        uint8_t *guard = (void *)(uintptr_t)arguments[0];
        guard[0] = 1;
        guard[1] = 0;
        return 0;
    }
    if (import_is(name, "___cxa_guard_abort")) {
        uint8_t *guard = (void *)(uintptr_t)arguments[0];
        guard[1] = 0;
        return 0;
    }
    /* C++ static-destructor registration (Bejeweled 3's static initializers
       call it).  We terminate the game rather than unwind at exit, so accept
       the registration and drop it; report success. */
    if (import_is(name, "___cxa_atexit") || import_is(name, "_atexit") ||
        import_is(name, "___cxa_finalize")) {
        return 0;
    }

    /* The old Objective-C zero-cost exception API no longer exists.  Normal
       execution only needs the registration calls and setjmp's initial pass. */
    if (import_is(name, "_objc_exception_try_enter") ||
        import_is(name, "_objc_exception_try_exit")) return 0;
    if (import_is(name, "__setjmp")) return 0;
    if (import_is(name, "_objc_exception_match")) return 0;

    return dispatch_bridge_stages(import_id, name, arguments, return_address,
                                  kImportStageUnknown);
}

/* Audio and Objective-C/OpenGL stages.  known_stage is the memoized stage for
   this import id, or kImportStageUnknown on the first call (or for a name the
   runtime chain matched, which is never memoized). */
static uint64_t dispatch_bridge_stages(uint32_t import_id, const char *name,
                                       const uint32_t *arguments,
                                       uint32_t return_address,
                                       uint8_t known_stage)
{
    uint64_t stage_start = compat_runtime32_frame_profile_enabled ?
        profile_now() : 0;
    uint64_t result = 0;
    if (known_stage != kImportStageObjC &&
        audio_bridge32_dispatch(name, arguments, &result)) {
        if (stage_start) {
            uint64_t elapsed = profile_now() - stage_start;
            frame_profile.audio_ns += elapsed;
            if (elapsed >= 1000000) {
                fprintf(stderr, "compat32: slow audio import %s %.2fms\n",
                        name, (double)elapsed / 1e6);
            }
        }
        if (known_stage == kImportStageUnknown && !dispatch_name_matched) {
            uint8_t *slot = import_stage_slot(import_id);
            if (slot) *slot = kImportStageAudio;
        }
        return result;
    }

    if (objc_bridge32_dispatch(name, arguments, &result)) {
        if (stage_start) frame_profile.objc_ns += profile_now() - stage_start;
        if (known_stage == kImportStageUnknown && !dispatch_name_matched) {
            uint8_t *slot = import_stage_slot(import_id);
            if (slot) *slot = kImportStageObjC;
        }
        return result;
    }

    fprintf(stderr,
            "compat32: trapped import[%" PRIu32 "] %s from 0x%08" PRIx32
            " args=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n",
            import_id, name, return_address, arguments[0], arguments[1],
            arguments[2], arguments[3]);
    last_call_trapped = 1;
    lp32_leave_guest = 1;
    return 0;
}

static void release_guest_stack_slot(void *opaque)
{
    if (!opaque) return;
    uint32_t slot = (uint32_t)((uintptr_t)opaque - 1);
    pthread_mutex_lock(&guest_stack_slot_lock);
    guest_stack_slots_in_use &= ~(UINT64_C(1) << slot);
    pthread_mutex_unlock(&guest_stack_slot_lock);
}

static void create_guest_stack_slot_key(void)
{
    (void)pthread_key_create(&guest_stack_slot_key, release_guest_stack_slot);
}

static bool acquire_guest_stack_slot(void)
{
    enum { kGuestStackSlotCount = kGuestStackSize / kGuestStackPerThread };
    pthread_once(&guest_stack_slot_key_once, create_guest_stack_slot_key);

    pthread_mutex_lock(&guest_stack_slot_lock);
    uint32_t slot = 0;
    while (slot < kGuestStackSlotCount &&
           (guest_stack_slots_in_use & (UINT64_C(1) << slot))) {
        ++slot;
    }
    if (slot == kGuestStackSlotCount) {
        pthread_mutex_unlock(&guest_stack_slot_lock);
        return false;
    }
    guest_stack_slots_in_use |= UINT64_C(1) << slot;
    pthread_mutex_unlock(&guest_stack_slot_lock);

    if (pthread_setspecific(guest_stack_slot_key,
                            (void *)(uintptr_t)(slot + 1)) != 0) {
        pthread_mutex_lock(&guest_stack_slot_lock);
        guest_stack_slots_in_use &= ~(UINT64_C(1) << slot);
        pthread_mutex_unlock(&guest_stack_slot_lock);
        return false;
    }
    guest_thread_slot = slot;
    return true;
}

uint32_t compat_runtime32_call(uint32_t function, const uint32_t *arguments,
                               size_t argument_count)
{
    const uintptr_t stack_slice_size = 0x10000;
    if (guest_thread_slot == UINT32_MAX && !acquire_guest_stack_slot()) {
        last_call_trapped = 1;
        return 0;
    }
    if (guest_thread_slot >= kGuestStackSize / kGuestStackPerThread ||
        (guest_call_depth + 1) * stack_slice_size > kGuestStackPerThread) {
        last_call_trapped = 1;
        return 0;
    }
    uintptr_t stack_top = kGuestStackBase + kGuestStackSize -
                          guest_thread_slot * kGuestStackPerThread -
                          guest_call_depth * stack_slice_size;
    /* Entry goes through the i386 landing trampoline, whose `ret` pops the
       function address; the function then sees esp = sp + 4, which must be
       12 mod 16 as after a call. */
    uintptr_t maximum_sp = stack_top -
                           (argument_count + 2) * sizeof(uint32_t);
    uintptr_t sp = (maximum_sp & ~(uintptr_t)0x0f) | 0x08;
    if (sp > maximum_sp) sp -= 0x10;

    uint32_t *guest_stack = (void *)sp;
    guest_stack[0] = function;
    guest_stack[1] = kBridgeCodeBase + kExitThunkOffset;
    for (size_t index = 0; index < argument_count; ++index) {
        guest_stack[index + 2] = arguments[index];
    }

    last_call_trapped = 0;
    lp32_leave_guest = 0;
    ++guest_call_depth;
    lp32_guest_execution_enter();
    uint32_t result = run_compat32(lp32_landing32, (uint32_t)sp, lp32_cs32);
    lp32_guest_execution_leave();
    --guest_call_depth;
    return result;
}

int compat_runtime32_last_call_trapped(void)
{
    return last_call_trapped;
}

struct guest_heap_test_slot {
    uint32_t pointer;
    uint32_t size;
    uint8_t pattern;
};

struct guest_heap_test_worker {
    unsigned index;
    int failed;
};

static bool guest_heap_test_pattern(const struct guest_heap_test_slot *slot)
{
    if (!slot->pointer) return true;
    size_t count = slot->size < 16 ? slot->size : 16;
    const uint8_t *bytes = (const void *)(uintptr_t)slot->pointer;
    for (size_t index = 0; index < count; ++index) {
        if (bytes[index] != slot->pattern) return false;
    }
    return true;
}

static void *guest_heap_test_worker_main(void *opaque)
{
    struct guest_heap_test_worker *worker = opaque;
    struct guest_heap_test_slot slots[32] = {{0}};
    uint32_t random = UINT32_C(0x9e3779b9) ^ (worker->index * 0x10203u);
    uint32_t site = UINT32_C(0xff100000) + worker->index;
    for (unsigned iteration = 0; iteration < 25000; ++iteration) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        unsigned slot_index = random % 32;
        struct guest_heap_test_slot *slot = &slots[slot_index];
        if (slot->pointer) {
            if (!guest_heap_test_pattern(slot)) {
                worker->failed = 1;
                break;
            }
            guest_deallocate(slot->pointer);
            memset(slot, 0, sizeof(*slot));
        }
        uint32_t size = ((random >> 8) % 4096) + 1;
        uint32_t pointer = guest_allocate_at(size, false, site);
        if (!pointer) {
            worker->failed = 1;
            break;
        }
        uint8_t pattern = (uint8_t)(random | 1);
        memset((void *)(uintptr_t)pointer, pattern, size < 16 ? size : 16);
        slot->pointer = pointer;
        slot->size = size;
        slot->pattern = pattern;
        if ((iteration % 7) == 0) {
            uint32_t grown_size = size + 1 + ((random >> 20) % 4096);
            uint32_t grown = guest_reallocate(pointer, grown_size, site);
            if (!grown) {
                worker->failed = 1;
                break;
            }
            size_t preserved = size < 16 ? size : 16;
            const uint8_t *grown_bytes = (const void *)(uintptr_t)grown;
            for (size_t index = 0; index < preserved; ++index) {
                if (grown_bytes[index] != pattern) {
                    worker->failed = 1;
                    break;
                }
            }
            if (worker->failed) break;
            memset((void *)(uintptr_t)grown, pattern,
                   grown_size < 16 ? grown_size : 16);
            slot->pointer = grown;
            slot->size = grown_size;
        }
    }
    for (unsigned index = 0; index < 32; ++index) {
        if (slots[index].pointer) {
            if (!guest_heap_test_pattern(&slots[index])) worker->failed = 1;
            guest_deallocate(slots[index].pointer);
        }
    }
    return NULL;
}

static bool guest_heap_validate_free_lists(void)
{
    bool valid = true;
    uint64_t capacity_sum = 0;
    pthread_mutex_lock(&guest_heap_lock);
    uint64_t maximum_nodes = guest_heap_statistics.valid_frees +
                             guest_heap_statistics.split_blocks + 1;
    for (unsigned bin = 0; bin < kGuestHeapBinCount; ++bin) {
        uint32_t slow = guest_heap_free_bins[bin];
        uint32_t fast = slow;
        uint64_t count = 0;
        while (slow) {
            if (++count > maximum_nodes || slow < kGuestHeapBase ||
                (slow & 15) != 0 ||
                (uintptr_t)slow + kGuestHeapHeaderSize > guest_heap_cursor) {
                valid = false;
                break;
            }
            const uint32_t *metadata = (const void *)(uintptr_t)slow;
            if (metadata[1] != kGuestHeapFreeMagic || metadata[2] < 32 ||
                (metadata[2] & 15) != 0 ||
                guest_heap_bin_for_capacity(metadata[2]) != bin) {
                valid = false;
                break;
            }
            const uint32_t *footer = (const void *)(uintptr_t)(slow +
                kGuestHeapHeaderSize + metadata[2] - 8);
            if (footer[0] != kGuestHeapFreeMagic ||
                footer[1] != metadata[2]) {
                valid = false;
                break;
            }
            capacity_sum += metadata[2];
            slow = metadata[3];

            for (unsigned step = 0; step < 2 && fast; ++step) {
                if (fast < kGuestHeapBase || (fast & 15) != 0 ||
                    (uintptr_t)fast + kGuestHeapHeaderSize >
                        guest_heap_cursor) {
                    valid = false;
                    fast = 0;
                    break;
                }
                const uint32_t *fast_metadata =
                    (const void *)(uintptr_t)fast;
                if (fast_metadata[1] != kGuestHeapFreeMagic) {
                    valid = false;
                    fast = 0;
                    break;
                }
                fast = fast_metadata[3];
            }
            if (fast && slow == fast) {
                valid = false;
                break;
            }
        }
        if (!valid) break;
    }
    if (capacity_sum != guest_heap_statistics.reusable_capacity) valid = false;
    pthread_mutex_unlock(&guest_heap_lock);
    return valid;
}

int compat_runtime32_run_heap_self_test(void)
{
    if (!guest_heap_reuse) {
        fputs("Guest heap self-test: FAIL (reuse is disabled)\n", stderr);
        return -1;
    }
    pthread_mutex_lock(&guest_heap_lock);
    uint64_t initial_live_blocks = guest_heap_statistics.live_blocks;
    uint64_t initial_live_requested = guest_heap_statistics.live_requested;
    uint64_t initial_invalid = guest_heap_statistics.invalid_frees;
    uint64_t initial_double = guest_heap_statistics.double_frees;
    uint64_t initial_reused = guest_heap_statistics.reused_allocations;
    pthread_mutex_unlock(&guest_heap_lock);

    enum { reuse_block_count = 4096 };
    uint32_t blocks[reuse_block_count];
    for (unsigned index = 0; index < reuse_block_count; ++index) {
        blocks[index] = guest_allocate_at(256, false, UINT32_C(0xff200001));
        if (!blocks[index]) goto failure;
        memset((void *)(uintptr_t)blocks[index], (int)(index & 0xff), 256);
    }
    for (unsigned index = 0; index < reuse_block_count; ++index) {
        if (!guest_deallocate(blocks[index])) goto failure;
    }
    uintptr_t first_pass_high_water;
    pthread_mutex_lock(&guest_heap_lock);
    first_pass_high_water = guest_heap_statistics.high_water;
    pthread_mutex_unlock(&guest_heap_lock);
    for (unsigned index = 0; index < reuse_block_count; ++index) {
        blocks[index] = guest_allocate_at(256, false, UINT32_C(0xff200002));
        if (!blocks[index]) goto failure;
    }
    pthread_mutex_lock(&guest_heap_lock);
    bool reuse_grew_cursor = guest_heap_statistics.high_water !=
                             first_pass_high_water;
    pthread_mutex_unlock(&guest_heap_lock);
    if (reuse_grew_cursor) goto failure;
    for (unsigned index = 0; index < reuse_block_count; ++index) {
        guest_deallocate(blocks[index]);
    }

    uint32_t data = guest_allocate_at(4096, false, UINT32_C(0xff200003));
    if (!data) goto failure;
    for (unsigned index = 0; index < 4096; ++index) {
        ((uint8_t *)(uintptr_t)data)[index] = (uint8_t)(index * 17);
    }
    uint32_t grown = guest_reallocate(data, 16384, UINT32_C(0xff200004));
    if (!grown) goto failure;
    for (unsigned index = 0; index < 4096; ++index) {
        if (((const uint8_t *)(uintptr_t)grown)[index] !=
            (uint8_t)(index * 17)) goto failure;
    }
    uint32_t shrunk = guest_reallocate(grown, 128, UINT32_C(0xff200005));
    if (shrunk != grown) goto failure;
    guest_deallocate(shrunk);

    uint32_t dirty = guest_allocate_at(8192, false, UINT32_C(0xff200006));
    if (!dirty) goto failure;
    memset((void *)(uintptr_t)dirty, 0xa5, 8192);
    guest_deallocate(dirty);
    uint32_t zeroed = guest_allocate_at(8192, true, UINT32_C(0xff200007));
    if (!zeroed) goto failure;
    for (unsigned index = 0; index < 8192; ++index) {
        if (((const uint8_t *)(uintptr_t)zeroed)[index] != 0) goto failure;
    }
    guest_deallocate(zeroed);

    uint32_t string_object =
        guest_allocate_at(sizeof(uint32_t), true, UINT32_C(0xff200008));
    if (!string_object ||
        !guest_string_construct(string_object, "seed", 4,
                                UINT32_C(0xff200009))) goto failure;
    for (unsigned iteration = 0; iteration < 50000; ++iteration) {
        char text[64];
        int length = snprintf(text, sizeof(text), "heap-string-%u", iteration);
        if (length < 0 || (size_t)length >= sizeof(text) ||
            !guest_string_assign(string_object, text, (size_t)length,
                                 UINT32_C(0xff20000a)) ||
            strcmp(guest_string_data(string_object), text) != 0) {
            goto failure;
        }
    }
    guest_string_dispose_object(string_object);
    guest_deallocate(string_object);

    enum { worker_count = 4 };
    pthread_t threads[worker_count];
    struct guest_heap_test_worker workers[worker_count] = {{0}};
    unsigned started = 0;
    for (unsigned index = 0; index < worker_count; ++index) {
        workers[index].index = index;
        if (pthread_create(&threads[index], NULL,
                           guest_heap_test_worker_main, &workers[index]) != 0) {
            break;
        }
        ++started;
    }
    if (started != worker_count) {
        for (unsigned index = 0; index < started; ++index) {
            pthread_join(threads[index], NULL);
        }
        goto failure;
    }
    bool worker_failed = false;
    for (unsigned index = 0; index < worker_count; ++index) {
        pthread_join(threads[index], NULL);
        if (workers[index].failed) worker_failed = true;
    }
    if (worker_failed) goto failure;

    pthread_mutex_lock(&guest_heap_lock);
    bool counters_valid =
        guest_heap_statistics.live_blocks == initial_live_blocks &&
        guest_heap_statistics.live_requested == initial_live_requested &&
        guest_heap_statistics.invalid_frees == initial_invalid &&
        guest_heap_statistics.double_frees == initial_double &&
        guest_heap_statistics.reused_allocations > initial_reused;
    pthread_mutex_unlock(&guest_heap_lock);
    if (!counters_valid || !guest_heap_validate_free_lists()) goto failure;

    compat_runtime32_heap_report("self-test");
    puts("Guest heap self-test: PASS "
         "(reuse, split, realloc, calloc, 50000 strings, 4-thread churn)");
    return 0;

failure:
    compat_runtime32_heap_report("self-test-failure");
    fputs("Guest heap self-test: FAIL\n", stderr);
    return -1;
}

struct guest_file_test_worker {
    unsigned iterations;
    bool failed;
};

static void *guest_file_test_worker_main(void *opaque)
{
    struct guest_file_test_worker *worker = opaque;
    for (unsigned iteration = 0; iteration < worker->iterations; ++iteration) {
        FILE *file = fopen("/dev/null", "rb");
        uint32_t handle = guest_handle_for_file(file);
        if (!handle) {
            worker->failed = true;
            break;
        }
        FILE *locked = lock_host_file_for_guest(handle);
        if (!locked) {
            worker->failed = true;
            break;
        }
        (void)getc(locked);
        unlock_host_file();
        if (guest_close_file(handle) != 0) {
            worker->failed = true;
            break;
        }
    }
    return NULL;
}

int compat_runtime32_run_file_self_test(void)
{
    enum {
        sequential_iterations = 20000,
        worker_count = 4,
        worker_iterations = 5000,
    };
    uint32_t handles[kGuestFileCapacity] = {0};
    unsigned opened = 0;

    pthread_mutex_lock(&guest_file_lock);
    bool initially_empty = guest_file_active_count == 0;
    uint64_t initial_exhaustions = guest_file_exhaustion_count;
    pthread_mutex_unlock(&guest_file_lock);
    if (!initially_empty) goto failure;

    FILE *stale_file = fopen("/dev/null", "rb");
    uint32_t stale_handle = guest_handle_for_file(stale_file);
    if (!stale_handle || guest_close_file(stale_handle) != 0) goto failure;
    if (lock_host_file_for_guest(stale_handle)) {
        unlock_host_file();
        goto failure;
    }

    for (unsigned iteration = 0; iteration < sequential_iterations; ++iteration) {
        FILE *file = fopen("/dev/null", "rb");
        uint32_t handle = guest_handle_for_file(file);
        if (!handle || guest_close_file(handle) != 0) goto failure;
    }

    pthread_t threads[worker_count];
    struct guest_file_test_worker workers[worker_count] = {{0}};
    unsigned started = 0;
    for (unsigned index = 0; index < worker_count; ++index) {
        workers[index].iterations = worker_iterations;
        if (pthread_create(&threads[index], NULL,
                           guest_file_test_worker_main, &workers[index]) != 0) {
            break;
        }
        ++started;
    }
    if (started != worker_count) {
        for (unsigned index = 0; index < started; ++index) {
            pthread_join(threads[index], NULL);
        }
        goto failure;
    }
    bool worker_failed = false;
    for (unsigned index = 0; index < worker_count; ++index) {
        pthread_join(threads[index], NULL);
        if (workers[index].failed) worker_failed = true;
    }
    if (worker_failed) goto failure;

    /* Exercise true simultaneous capacity and prove the overflow FILE is
       closed by guest_handle_for_file rather than becoming unreachable. */
    for (; opened < kGuestFileCapacity; ++opened) {
        handles[opened] = guest_handle_for_file(fopen("/dev/null", "rb"));
        if (!handles[opened]) goto failure;
    }
    FILE *overflow = fopen("/dev/null", "rb");
    if (!overflow) goto failure;
    int overflow_descriptor = fileno(overflow);
    if (guest_handle_for_file(overflow) != 0) goto failure;
    errno = 0;
    if (fcntl(overflow_descriptor, F_GETFD) != -1 || errno != EBADF) {
        goto failure;
    }
    while (opened) {
        --opened;
        if (guest_close_file(handles[opened]) != 0) goto failure;
        handles[opened] = 0;
    }

    pthread_mutex_lock(&guest_file_lock);
    bool counters_valid = guest_file_active_count == 0 &&
                          guest_file_peak_count == kGuestFileCapacity &&
                          guest_file_exhaustion_count == initial_exhaustions + 1;
    uint32_t success_peak = guest_file_peak_count;
    pthread_mutex_unlock(&guest_file_lock);
    if (!counters_valid) goto failure;

    /* The scanf formats the engine's readers depend on, pcconfig.txt's
       version probe first (a scanset the guest scanner used to reject). */
    {
        /* Destinations must be guest (32-bit) addresses. */
        uint32_t cells = guest_allocate(64, true);
        if (!cells) goto failure;
        uint32_t *version = (uint32_t *)(uintptr_t)cells;
        char *token = (char *)(uintptr_t)(cells + 16);
        uint32_t destinations[2] = { cells, 0 };
        bool scan_ok =
            guest_vscan("FileVersion                 76\nScreenWidth 1920\n",
                        "FileVersion%*[ \n\t]%d", destinations) == 1 &&
            *version == 76;
        destinations[0] = cells + 16;
        destinations[1] = cells;
        scan_ok = scan_ok &&
            guest_vscan("key-name=42", "%[a-z-]=%u", destinations) == 2 &&
            strcmp(token, "key-name") == 0 && *version == 42;
        scan_ok = scan_ok &&
            guest_vscan("abc]def", "%[^]]", destinations) == 1 &&
            strcmp(token, "abc") == 0;
        scan_ok = scan_ok && guest_vscan("   ", "%[a-z]", destinations) == 0;
        guest_deallocate(cells);
        if (!scan_ok) goto failure;
    }

    printf("Guest FILE bridge self-test: PASS "
           "(%u sequential, %u-thread churn, %u simultaneous, peak=%" PRIu32 ")\n",
           sequential_iterations, worker_count, kGuestFileCapacity, success_peak);
    return 0;

failure:
    while (opened) {
        --opened;
        if (handles[opened]) (void)guest_close_file(handles[opened]);
    }
    pthread_mutex_lock(&guest_file_lock);
    uint32_t active = guest_file_active_count;
    uint32_t failure_peak = guest_file_peak_count;
    uint64_t exhausted = guest_file_exhaustion_count;
    pthread_mutex_unlock(&guest_file_lock);
    fprintf(stderr,
            "Guest FILE bridge self-test: FAIL "
            "(active=%" PRIu32 " peak=%" PRIu32 " exhaustions=%" PRIu64 ")\n",
            active, failure_peak, exhausted);
    return -1;
}

uint32_t compat_runtime32_copy_cstring(const char *string)
{
    size_t size = strlen(string) + 1;
    uint32_t destination = guest_allocate(size, false);
    if (destination) memcpy((void *)(uintptr_t)destination, string, size);
    return destination;
}

uint32_t compat_runtime32_allocate(size_t size, int clear)
{
    return guest_allocate(size, clear != 0);
}

uint32_t compat_runtime32_reallocate(uint32_t pointer, size_t size)
{
    return guest_reallocate(pointer, size, kGuestHeapHostSite);
}

void compat_runtime32_deallocate(uint32_t pointer)
{
    guest_deallocate(pointer);
}

uint64_t compat_runtime32_dispatch_import(const char *name,
                                          const uint32_t *arguments)
{
    uint64_t result = dispatch_named_import(UINT32_MAX, name, arguments,
                                           kGuestHeapHostSite);
    /* Host-side callers never return through the import gateway, which is
       what consumes a pending float result. */
    lp32_return_fp_kind = 0;
    return result;
}

/*
 * Cg self-test.  Compiles far more programs than the retired 4,096-entry
 * handle table could hold and checks that every one of them still yields a
 * program handle, an enumerable parameter list, stable parameter-name
 * strings, and a compiled listing.  Programs are destroyed in waves so the
 * runtime recycles handles and string buffers the same way a campaign does.
 */
int compat_runtime32_run_cg_self_test(void)
{
    enum {
        program_total = 6000,
        live_window = 512,
        CG_SOURCE = 4112,
        CG_PROGRAM = 4109,
        CG_COMPILED_PROGRAM = 4106,
        CG_PROFILE_ARBVP1 = 6150,
    };
    static const char source_text[] =
        "struct Output { float4 position : POSITION; float4 color : COLOR; };\n"
        "Output main(float4 position : POSITION, float4 color : COLOR,\n"
        "            uniform float4x4 modelViewProjection,\n"
        "            uniform float4 tintColor, uniform float4 fogParameters)\n"
        "{\n"
        "    Output result;\n"
        "    result.position = mul(modelViewProjection, position);\n"
        "    result.color = color * tintColor + fogParameters;\n"
        "    return result;\n"
        "}\n";

    if (!ensure_cg_library()) {
        fputs("Cg bridge self-test: SKIP (bundled Cg unavailable)\n", stderr);
        return 0;
    }

    uint32_t source = compat_runtime32_copy_cstring(source_text);
    uint32_t entry = compat_runtime32_copy_cstring("main");
    uint32_t *programs = calloc(live_window, sizeof(*programs));
    uint32_t *first_names = calloc(live_window, sizeof(*first_names));
    if (!source || !entry || !programs || !first_names) {
        fputs("Cg bridge self-test: FAIL (allocation)\n", stderr);
        free(programs);
        free(first_names);
        return -1;
    }

    uint32_t arguments[8] = {0};
    uint32_t context = (uint32_t)compat_runtime32_dispatch_import(
        "_cgCreateContext", arguments);
    if (!context) {
        fputs("Cg bridge self-test: FAIL (cgCreateContext returned 0)\n", stderr);
        free(programs);
        free(first_names);
        return -1;
    }

    uint32_t created = 0, destroyed = 0, parameters_seen = 0;
    uint32_t minimum_parameters = UINT32_MAX;
    uint32_t largest_handle = 0;
    uint32_t strings_after_first_wave = 0;
    uint32_t string_churn = 0;
    const char *failure = NULL;

    for (uint32_t index = 0; index < program_total && !failure; ++index) {
        uint32_t slot = index % live_window;
        if (programs[slot]) {
            arguments[0] = programs[slot];
            compat_runtime32_dispatch_import("_cgDestroyProgram", arguments);
            programs[slot] = 0;
            ++destroyed;
        }
        arguments[0] = context;
        arguments[1] = CG_SOURCE;
        arguments[2] = source;
        arguments[3] = CG_PROFILE_ARBVP1;
        arguments[4] = entry;
        arguments[5] = 0;
        uint32_t program = (uint32_t)compat_runtime32_dispatch_import(
            "_cgCreateProgram", arguments);
        if (!program) {
            failure = "cgCreateProgram returned 0";
            break;
        }
        programs[slot] = program;
        ++created;
        if (program > largest_handle) largest_handle = program;

        arguments[0] = program;
        arguments[1] = CG_COMPILED_PROGRAM;
        uint32_t listing = (uint32_t)compat_runtime32_dispatch_import(
            "_cgGetProgramString", arguments);
        if (!listing || strncmp((const char *)(uintptr_t)listing, "!!ARBvp1.0",
                                10) != 0) {
            failure = "cgGetProgramString did not return an ARBvp1 listing";
            break;
        }

        arguments[0] = program;
        arguments[1] = CG_PROGRAM;
        uint32_t parameter = (uint32_t)compat_runtime32_dispatch_import(
            "_cgGetFirstParameter", arguments);
        if (!parameter) {
            failure = "cgGetFirstParameter returned 0";
            break;
        }
        uint32_t count = 0;
        bool saw_tint = false;
        while (parameter) {
            arguments[0] = parameter;
            uint32_t name = (uint32_t)compat_runtime32_dispatch_import(
                "_cgGetParameterName", arguments);
            if (!name) {
                failure = "cgGetParameterName returned 0";
                break;
            }
            uint32_t again = (uint32_t)compat_runtime32_dispatch_import(
                "_cgGetParameterName", arguments);
            if (again != name) {
                failure = "cgGetParameterName guest pointer is unstable";
                break;
            }
            if (strcmp((const char *)(uintptr_t)name, "tintColor") == 0) {
                saw_tint = true;
            }
            if (count == 0) first_names[slot] = name;
            ++count;
            parameter = (uint32_t)compat_runtime32_dispatch_import(
                "_cgGetNextParameter", arguments);
        }
        if (failure) break;
        if (!saw_tint) {
            failure = "parameter enumeration lost tintColor";
            break;
        }
        parameters_seen += count;
        if (count < minimum_parameters) minimum_parameters = count;

        if (index + 1 == live_window) {
            strings_after_first_wave = compat_runtime32_cg_string_count();
        }
    }

    if (!failure) {
        /* Names of the first program in the window must still read back
           correctly after thousands of recycles through the same slots. */
        for (uint32_t slot = 0; slot < live_window; ++slot) {
            if (!programs[slot]) continue;
            arguments[0] = programs[slot];
            arguments[1] = CG_PROGRAM;
            uint32_t parameter = (uint32_t)compat_runtime32_dispatch_import(
                "_cgGetFirstParameter", arguments);
            arguments[0] = parameter;
            uint32_t name = (uint32_t)compat_runtime32_dispatch_import(
                "_cgGetParameterName", arguments);
            if (name != first_names[slot]) ++string_churn;
        }
    }

    uint32_t strings_at_end = compat_runtime32_cg_string_count();
    uint32_t pointer_handles = compat_runtime32_cg_object_count();
    for (uint32_t slot = 0; slot < live_window; ++slot) {
        if (!programs[slot]) continue;
        arguments[0] = programs[slot];
        compat_runtime32_dispatch_import("_cgDestroyProgram", arguments);
        ++destroyed;
    }
    free(programs);
    free(first_names);

    fprintf(stderr,
            "Cg bridge self-test: created=%" PRIu32 " destroyed=%" PRIu32
            " parameters=%" PRIu32 " min-per-program=%" PRIu32
            " largest-handle=0x%" PRIx32 " pointer-handles=%" PRIu32
            " strings(after-first-wave=%" PRIu32 " end=%" PRIu32
            ") name-churn=%" PRIu32 "\n",
            created, destroyed, parameters_seen, minimum_parameters,
            largest_handle, pointer_handles, strings_after_first_wave,
            strings_at_end, string_churn);
    if (failure) {
        fprintf(stderr, "Cg bridge self-test: FAIL (%s after %" PRIu32
                " programs)\n", failure, created);
        return -1;
    }
    if (created != program_total || minimum_parameters < 5) {
        fputs("Cg bridge self-test: FAIL (incomplete run)\n", stderr);
        return -1;
    }
    /* Interned names must not allocate fresh guest strings per program, and
       compiled listings must be released with their program, so the table
       should be no larger than it was once the live window first filled. */
    if (strings_at_end > strings_after_first_wave + 64) {
        fputs("Cg bridge self-test: FAIL (guest string cache grows unbounded)\n",
              stderr);
        return -1;
    }
    fputs("Cg bridge self-test: PASS\n", stderr);
    return 0;
}

/*
 * Synchronization self-test.  Creates and destroys more guest mutexes,
 * condition variables, and MP semaphores than the retired fixed tables held,
 * verifies host objects are recycled rather than leaked, and checks that
 * stale semaphore handles are rejected once their slot is reused.
 */
struct sync_self_test_waiter {
    uint32_t semaphore;
    uint32_t condition;
    uint32_t mutex;
    uint32_t result;
    uint32_t ready;
};

static void *sync_self_test_semaphore_waiter(void *opaque)
{
    struct sync_self_test_waiter *waiter = opaque;
    uint32_t arguments[4] = {waiter->semaphore, UINT32_C(0x7fffffff), 0, 0};
    __atomic_store_n(&waiter->ready, 1, __ATOMIC_RELEASE);
    waiter->result = (uint32_t)compat_runtime32_dispatch_import(
        "_MPWaitOnSemaphore", arguments);
    return NULL;
}

static void *sync_self_test_condition_waiter(void *opaque)
{
    struct sync_self_test_waiter *waiter = opaque;
    uint32_t arguments[4] = {waiter->mutex, 0, 0, 0};
    compat_runtime32_dispatch_import("_pthread_mutex_lock", arguments);
    __atomic_store_n(&waiter->ready, 1, __ATOMIC_RELEASE);
    arguments[0] = waiter->condition;
    arguments[1] = waiter->mutex;
    waiter->result = (uint32_t)compat_runtime32_dispatch_import(
        "_pthread_cond_wait", arguments);
    arguments[0] = waiter->mutex;
    compat_runtime32_dispatch_import("_pthread_mutex_unlock", arguments);
    return NULL;
}

static uint32_t sync_self_test_free_list_length(struct host_sync_free_node *node)
{
    uint32_t length = 0;
    while (node) {
        ++length;
        node = node->next;
    }
    return length;
}

int compat_runtime32_run_sync_self_test(void)
{
    enum {
        object_total = 8192,
        semaphore_total = kGuestSemaphoreCapacity - 1,
        mutex_base = UINT32_C(0x40000000),
        condition_base = UINT32_C(0x50000000),
    };
    uint32_t arguments[4] = {0};
    const char *failure = NULL;

    pthread_mutex_lock(&guest_sync_table_lock);
    uint32_t initial_mutexes = guest_mutex_count;
    uint32_t initial_conditions = guest_cond_count;
    pthread_mutex_unlock(&guest_sync_table_lock);

    /* Mutexes: init, lock/unlock, destroy, then re-create the same number and
       make sure the second wave came from the free list. */
    for (uint32_t pass = 0; pass < 2 && !failure; ++pass) {
        for (uint32_t index = 0; index < object_total; ++index) {
            arguments[0] = mutex_base + index * 64;
            arguments[1] = 0;
            if (compat_runtime32_dispatch_import("_pthread_mutex_init", arguments)) {
                failure = "pthread_mutex_init failed";
                break;
            }
            if (compat_runtime32_dispatch_import("_pthread_mutex_lock", arguments) ||
                compat_runtime32_dispatch_import("_pthread_mutex_unlock", arguments)) {
                failure = "pthread_mutex lock/unlock failed";
                break;
            }
        }
        if (failure) break;
        pthread_mutex_lock(&guest_sync_table_lock);
        uint32_t live = guest_mutex_count;
        uint32_t free_nodes = sync_self_test_free_list_length(free_host_mutexes);
        pthread_mutex_unlock(&guest_sync_table_lock);
        if (live != initial_mutexes + object_total) {
            failure = "mutex registry count mismatch";
            break;
        }
        if (pass == 1 && free_nodes != 0) {
            failure = "second mutex wave did not drain the free list";
            break;
        }
        /* A locked mutex must survive destroy (EBUSY) instead of being
           recycled underneath its owner. */
        arguments[0] = mutex_base;
        compat_runtime32_dispatch_import("_pthread_mutex_lock", arguments);
        if (compat_runtime32_dispatch_import("_pthread_mutex_destroy", arguments) !=
            (uint32_t)EBUSY) {
            failure = "destroying a locked mutex did not report EBUSY";
            break;
        }
        compat_runtime32_dispatch_import("_pthread_mutex_unlock", arguments);
        for (uint32_t index = 0; index < object_total; ++index) {
            arguments[0] = mutex_base + index * 64;
            if (compat_runtime32_dispatch_import("_pthread_mutex_destroy", arguments)) {
                failure = "pthread_mutex_destroy failed";
                break;
            }
        }
        if (failure) break;
        pthread_mutex_lock(&guest_sync_table_lock);
        live = guest_mutex_count;
        free_nodes = sync_self_test_free_list_length(free_host_mutexes);
        pthread_mutex_unlock(&guest_sync_table_lock);
        if (live != initial_mutexes || free_nodes != object_total) {
            failure = "mutex destroy did not release every entry";
            break;
        }
    }

    /* Condition variables: same shape, plus a real wait/signal round trip. */
    for (uint32_t pass = 0; pass < 2 && !failure; ++pass) {
        for (uint32_t index = 0; index < object_total; ++index) {
            arguments[0] = condition_base + index * 64;
            arguments[1] = 0;
            if (compat_runtime32_dispatch_import("_pthread_cond_init", arguments)) {
                failure = "pthread_cond_init failed";
                break;
            }
        }
        if (failure) break;
        pthread_mutex_lock(&guest_sync_table_lock);
        uint32_t live = guest_cond_count;
        uint32_t free_nodes =
            sync_self_test_free_list_length(free_host_conditions);
        pthread_mutex_unlock(&guest_sync_table_lock);
        if (live != initial_conditions + object_total ||
            (pass == 1 && free_nodes != 0)) {
            failure = "condition registry count mismatch";
            break;
        }

        struct sync_self_test_waiter waiter = {
            .condition = condition_base, .mutex = mutex_base,
        };
        arguments[0] = mutex_base;
        compat_runtime32_dispatch_import("_pthread_mutex_init", arguments);
        pthread_t thread;
        if (pthread_create(&thread, NULL, sync_self_test_condition_waiter,
                           &waiter) != 0) {
            failure = "pthread_create failed";
            break;
        }
        while (!__atomic_load_n(&waiter.ready, __ATOMIC_ACQUIRE)) sched_yield();
        /* Take the mutex so the waiter is guaranteed to be inside cond_wait. */
        arguments[0] = mutex_base;
        compat_runtime32_dispatch_import("_pthread_mutex_lock", arguments);
        arguments[0] = condition_base;
        if (pass == 0) {
            compat_runtime32_dispatch_import("_pthread_cond_signal", arguments);
        } else {
            /* Destroying a waited-on condition must release the waiter rather
               than strand it on a recycled host object. */
            compat_runtime32_dispatch_import("_pthread_cond_destroy", arguments);
        }
        arguments[0] = mutex_base;
        compat_runtime32_dispatch_import("_pthread_mutex_unlock", arguments);
        pthread_join(thread, NULL);
        if (waiter.result != 0) {
            failure = "pthread_cond_wait round trip failed";
            break;
        }
        compat_runtime32_dispatch_import("_pthread_mutex_destroy", arguments);

        for (uint32_t index = 0; index < object_total; ++index) {
            arguments[0] = condition_base + index * 64;
            if (compat_runtime32_dispatch_import("_pthread_cond_destroy", arguments)) {
                failure = "pthread_cond_destroy failed";
                break;
            }
        }
        if (failure) break;
        pthread_mutex_lock(&guest_sync_table_lock);
        live = guest_cond_count;
        free_nodes = sync_self_test_free_list_length(free_host_conditions);
        pthread_mutex_unlock(&guest_sync_table_lock);
        if (live != initial_conditions || free_nodes != object_total) {
            failure = "condition destroy did not release every entry";
            break;
        }
    }

    /* Semaphores: fill the table, delete everything, refill, and confirm the
       first wave's handles are rejected even though their slots were reused. */
    uint32_t *first_wave = calloc(semaphore_total, sizeof(*first_wave));
    uint32_t *second_wave = calloc(semaphore_total, sizeof(*second_wave));
    uint32_t output = compat_runtime32_allocate(sizeof(uint32_t), 1);
    uint32_t stale_rejections = 0;
    uint32_t initial_semaphores =
        __atomic_load_n(&guest_semaphore_live_count, __ATOMIC_RELAXED);
    if (!first_wave || !second_wave || !output) failure = "allocation";
    if (!failure && initial_semaphores != 0) {
        failure = "semaphores already live before test";
    }
    for (uint32_t wave = 0; wave < 2 && !failure; ++wave) {
        uint32_t *handles = wave ? second_wave : first_wave;
        for (uint32_t index = 0; index < semaphore_total; ++index) {
            arguments[0] = 1;
            arguments[1] = 0;
            arguments[2] = output;
            if (compat_runtime32_dispatch_import("_MPCreateSemaphore", arguments) ||
                !*(uint32_t *)(uintptr_t)output) {
                failure = "MPCreateSemaphore failed";
                break;
            }
            handles[index] = *(uint32_t *)(uintptr_t)output;
        }
        if (failure) break;
        arguments[0] = 1;
        arguments[1] = 0;
        arguments[2] = output;
        if (compat_runtime32_dispatch_import("_MPCreateSemaphore", arguments) !=
            (uint32_t)-1) {
            failure = "table overflow was not reported";
            break;
        }
        if (wave == 1) {
            for (uint32_t index = 0; index < semaphore_total; ++index) {
                arguments[0] = first_wave[index];
                if (compat_runtime32_dispatch_import("_MPSignalSemaphore",
                                                     arguments) == (uint32_t)-1) {
                    ++stale_rejections;
                }
            }
            if (stale_rejections != semaphore_total) {
                failure = "stale semaphore handle was accepted";
                break;
            }
        }
        /* Signal/wait round trip and a blocked waiter released by delete. */
        arguments[0] = handles[0];
        if (compat_runtime32_dispatch_import("_MPSignalSemaphore", arguments) ||
            compat_runtime32_dispatch_import("_MPSignalSemaphore", arguments)) {
            failure = "MPSignalSemaphore failed";
            break;
        }
        arguments[1] = 0;
        if (compat_runtime32_dispatch_import("_MPWaitOnSemaphore", arguments) ||
            compat_runtime32_dispatch_import("_MPWaitOnSemaphore", arguments) !=
            (uint32_t)-1) {
            failure = "semaphore count/maximum not honoured";
            break;
        }
        struct sync_self_test_waiter waiter = {.semaphore = handles[1]};
        pthread_t thread;
        if (pthread_create(&thread, NULL, sync_self_test_semaphore_waiter,
                           &waiter) != 0) {
            failure = "pthread_create failed";
            break;
        }
        while (!__atomic_load_n(&waiter.ready, __ATOMIC_ACQUIRE)) sched_yield();
        usleep(20000);
        if (wave == 0) {
            arguments[0] = handles[1];
            compat_runtime32_dispatch_import("_MPSignalSemaphore", arguments);
            pthread_join(thread, NULL);
            if (waiter.result != 0) {
                failure = "blocked semaphore waiter was not released by signal";
                break;
            }
        } else {
            arguments[0] = handles[1];
            compat_runtime32_dispatch_import("_MPDeleteSemaphore", arguments);
            pthread_join(thread, NULL);
            if (waiter.result != (uint32_t)-1) {
                failure = "blocked semaphore waiter was not released by delete";
                break;
            }
            handles[1] = 0;
        }
        for (uint32_t index = 0; index < semaphore_total; ++index) {
            if (!handles[index]) continue;
            arguments[0] = handles[index];
            if (compat_runtime32_dispatch_import("_MPDeleteSemaphore", arguments)) {
                failure = "MPDeleteSemaphore failed";
                break;
            }
        }
        if (failure) break;
        if (__atomic_load_n(&guest_semaphore_live_count, __ATOMIC_RELAXED) != 0) {
            failure = "semaphore live count did not return to zero";
            break;
        }
    }
    free(first_wave);
    free(second_wave);
    if (output) compat_runtime32_deallocate(output);

    if (failure) {
        fprintf(stderr, "Sync bridge self-test: FAIL (%s)\n", failure);
        return -1;
    }
    fprintf(stderr,
            "Sync bridge self-test: PASS (mutexes=%d conditions=%d semaphores=%d"
            " stale-rejections=%" PRIu32 " released=%" PRIu64 ")\n",
            object_total, object_total, semaphore_total, stale_rejections,
            guest_sync_released_count);
    return 0;
}
