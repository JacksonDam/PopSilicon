#include "steam_cloud.h"
#include "pvz_save.h"
#include "compat_runtime.h"
#include "game_profile.h"
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* PvZ ships RemoteStorage002 (seven slots). Confirmed against its i386 calls
   at 0x1ddcf4..0x1de17c; the modern flat API avoids a second C++ vtable ABI. */
static const char *const methods[] = {
    "_lp32_cloud_write", "_lp32_cloud_size", "_lp32_cloud_read",
    "_lp32_cloud_exists", "_lp32_cloud_count", "_lp32_cloud_name",
    "_lp32_cloud_quota"
};
static struct {
    int (*init)(char *);
    void (*shutdown)(void);
    void (*callbacks)(void);
    void *(*storage)(void);
    void *(*utils)(void);
    void *(*user)(void);
    uint32_t (*appid)(void *);
    bool (*logged_on)(void *);
    bool (*account_enabled)(void *);
    bool (*app_enabled)(void *);
    bool (*write)(void *, const char *, const void *, int32_t);
    int32_t (*size)(void *, const char *);
    int32_t (*read)(void *, const char *, void *, int32_t);
    bool (*exists)(void *, const char *);
    int32_t (*count)(void *);
    const char *(*name)(void *, int, int32_t *);
    bool (*quota)(void *, uint64_t *, uint64_t *);
} api;
static bool checked, enabled, initialized;
static void *storage;
static uint32_t guest_interface, guest_name;

static void fail(const char *message)
{
    fprintf(stderr, "Steam Cloud: %s\n", message);
    exit(EXIT_FAILURE);
}

static void shutdown_cloud(void)
{
    if (initialized) {
        api.shutdown();
        initialized = false;
    }
}

static bool enabled_bundle(void)
{
    if (checked) return enabled;
    checked = true;
    if (lp32_profile()->title != LP32_TITLE_PLANTSVSZOMBIES) return false;
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size)) fail("executable path too long");
    char *slash = strrchr(path, '/');
    if (!slash) fail("executable directory unavailable");
    *slash = '\0';
    size_t used = strlen(path);
    const char suffix[] = "/lp32_steam_api.dylib";
    if (used + sizeof(suffix) > sizeof(path)) fail("Steam library path too long");
    memcpy(path + used, suffix, sizeof(suffix));
    char marker[PATH_MAX];
    int n = snprintf(marker, sizeof(marker), "%.*s/../SharedSupport/steam-cloud", (int)used, path);
    if (n < 0 || (size_t)n >= sizeof(marker)) fail("Cloud marker path too long");
    if (access(marker, F_OK)) {
        if (errno == ENOENT) return false;
        fail("cannot inspect Cloud enable marker");
    }
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) fail(dlerror());
#define LOAD(field, symbol) do { \
    *(void **)(&api.field) = dlsym(library, symbol); \
    if (!api.field) fail("required Steam API missing: " symbol); \
} while (0)
    LOAD(init, "SteamAPI_InitFlat");
    LOAD(shutdown, "SteamAPI_Shutdown");
    LOAD(callbacks, "SteamAPI_RunCallbacks");
    LOAD(storage, "SteamAPI_SteamRemoteStorage_v016");
    LOAD(utils, "SteamAPI_SteamUtils_v011");
    LOAD(user, "SteamAPI_SteamUser_v023");
    LOAD(appid, "SteamAPI_ISteamUtils_GetAppID");
    LOAD(logged_on, "SteamAPI_ISteamUser_BLoggedOn");
    LOAD(account_enabled, "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount");
    LOAD(app_enabled, "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp");
    LOAD(write, "SteamAPI_ISteamRemoteStorage_FileWrite");
    LOAD(size, "SteamAPI_ISteamRemoteStorage_GetFileSize");
    LOAD(read, "SteamAPI_ISteamRemoteStorage_FileRead");
    LOAD(exists, "SteamAPI_ISteamRemoteStorage_FileExists");
    LOAD(count, "SteamAPI_ISteamRemoteStorage_GetFileCount");
    LOAD(name, "SteamAPI_ISteamRemoteStorage_GetFileNameAndSize");
    LOAD(quota, "SteamAPI_ISteamRemoteStorage_GetQuota");
#undef LOAD
    enabled = true;
    return true;
}

static void initialize_cloud(void)
{
    if (initialized) return;
    const char *appid = getenv("SteamAppId");
    if (appid && strcmp(appid, "3590")) fail("expected SteamAppId 3590 for PvZ");
    if (!appid && setenv("SteamAppId", "3590", 0)) fail("cannot set SteamAppId");
    char error[1024] = {0};
    if (api.init(error)) fail(error[0] ? error : "Steam initialization failed; start Steam and sign in");
    initialized = true;
    if (atexit(shutdown_cloud)) fail("cannot register Steam shutdown");
    void *utils = api.utils(), *user = api.user();
    storage = api.storage();
    if (!utils || !user || !storage) fail("required Steam interface unavailable");
    if (api.appid(utils) != 3590) fail("Steam authenticated the wrong app");
    if (!api.logged_on(user)) fail("Steam is offline; sign in before playing with Cloud enabled");
    if (!api.account_enabled(storage) || !api.app_enabled(storage))
        fail("Cloud is disabled in Steam; enable it before using this build");
    fprintf(stderr, "Steam Cloud: connected to PvZ (3590), %d files\n", api.count(storage));
}

static uint32_t interface32(void)
{
    if (guest_interface) return guest_interface;
    uint32_t table = compat_runtime32_allocate(sizeof(methods) / sizeof(*methods) * 4, 1);
    guest_interface = compat_runtime32_allocate(4, 1);
    guest_name = compat_runtime32_allocate(260, 1);
    if (!table || !guest_interface || !guest_name) fail("guest interface allocation failed");
    uint32_t *slots = (void *)(uintptr_t)table;
    for (size_t i = 0; i < sizeof(methods) / sizeof(*methods); ++i) {
        slots[i] = compat_runtime32_guest_callback(methods[i]);
        if (!slots[i]) fail("guest callback allocation failed");
    }
    *(uint32_t *)(uintptr_t)guest_interface = table;
    return guest_interface;
}

static bool valid_filename(const char *name)
{
    return name && name[0] && strnlen(name, 260) < 260;
}

static void *read_profile(const char *name, size_t *size)
{
    int32_t bytes = api.size(storage, name);
    if (bytes <= 0 || bytes > 100 * 1024 * 1024) fail("invalid PvZ Cloud profile size");
    void *raw = malloc((size_t)bytes);
    if (!raw) fail("profile allocation failed");
    if (api.read(storage, name, raw, bytes) != bytes) { free(raw); fail("incomplete Cloud profile read"); }
    void *converted = pvz_save_convert(raw, (size_t)bytes, true, size);
    free(raw);
    if (!converted) fail("unsupported or lossy PvZ Cloud profile format; original Cloud data unchanged");
    return converted;
}

static uint64_t dispatch_method(size_t slot, const uint32_t *a)
{
    if (!initialized || !guest_interface || a[0] != guest_interface)
        fail("storage called without its initialized guest interface");
    const char *name = (const void *)(uintptr_t)a[1];
    if (slot <= 3 && !valid_filename(name)) return 0;
    switch (slot) {
    case 0:
        if ((int32_t)a[3] < 0 || (!a[2] && a[3])) return 0;
        {
            const void *data = (const void *)(uintptr_t)a[2];
            void *converted = NULL;
            size_t bytes = a[3];
            if (pvz_save_is_profile(name)) {
                converted = pvz_save_convert(data, bytes, false, &bytes);
                if (!converted || bytes > INT32_MAX) { free(converted); fail("refusing to upload an invalid PvZ profile"); }
                data = converted;
            }
            bool ok = api.write(storage, name, data, (int32_t)bytes);
            free(converted);
            fprintf(stderr, "Steam Cloud: write %s (%u bytes): %s\n", name, a[3], ok ? "accepted" : "FAILED");
            return ok;
        }
    case 1:
        if (pvz_save_is_profile(name) && api.exists(storage, name)) {
            size_t bytes; void *data = read_profile(name, &bytes); free(data); return bytes;
        }
        return (uint32_t)api.size(storage, name);
    case 2:
        if ((int32_t)a[3] < 0 || (!a[2] && a[3])) return 0;
        if (pvz_save_is_profile(name) && api.exists(storage, name)) {
            size_t bytes; void *data = read_profile(name, &bytes);
            if (bytes > a[3]) bytes = a[3];
            if (bytes) memcpy((void *)(uintptr_t)a[2], data, bytes);
            free(data); return bytes;
        }
        return (uint32_t)api.read(storage, name, (void *)(uintptr_t)a[2], (int32_t)a[3]);
    case 3: return api.exists(storage, name);
    case 4: return (uint32_t)api.count(storage);
    case 5: {
        if (!a[2]) return 0;
        int32_t *size = (void *)(uintptr_t)a[2];
        *size = 0;
        if ((int32_t)a[1] < 0 || (int32_t)a[1] >= api.count(storage)) return 0;
        const char *file = api.name(storage, (int32_t)a[1], size);
        if (!valid_filename(file)) return 0;
        memcpy((void *)(uintptr_t)guest_name, file, strlen(file) + 1);
        if (pvz_save_is_profile(file)) {
            size_t bytes; void *data = read_profile((const char *)(uintptr_t)guest_name, &bytes);
            free(data); *size = (int32_t)bytes;
        }
        return guest_name;
    }
    case 6: {
        if (!a[1] || !a[2]) return 0;
        uint64_t total = 0, available = 0;
        bool ok = api.quota(storage, &total, &available);
        /* RemoteStorage002 has signed 32-bit quota outputs, modern Steam has 64-bit. */
        *(int32_t *)(uintptr_t)a[1] = ok ? (total > INT32_MAX ? INT32_MAX : (int32_t)total) : 0;
        *(int32_t *)(uintptr_t)a[2] = ok ? (available > INT32_MAX ? INT32_MAX : (int32_t)available) : 0;
        return ok;
    }
    default: fail("unknown legacy storage method");
    }
    return 0;
}

int steam_cloud_dispatch(const char *name, const uint32_t *a, uint64_t *result)
{
    if (strncmp(name, "_Steam", 6) && strncmp(name, "_lp32_cloud_", 12)) return 0;
    if (!enabled_bundle()) return 0;
    if (!strcmp(name, "_SteamAPI_Init") || !strcmp(name, "_SteamAPI_InitSafe")) {
        initialize_cloud();
        *result = 1;
    } else if (!strcmp(name, "_SteamRemoteStorage")) {
        if (!initialized) fail("SteamRemoteStorage requested before SteamAPI_Init");
        *result = interface32();
    } else if (!strcmp(name, "_SteamAPI_RunCallbacks")) {
        if (initialized) api.callbacks();
        *result = 0;
    } else if (!strcmp(name, "_SteamAPI_Shutdown")) {
        shutdown_cloud();
        *result = 0;
    } else {
        for (size_t i = 0; i < sizeof(methods) / sizeof(*methods); ++i) {
            if (!strcmp(name, methods[i])) {
                *result = dispatch_method(i, a);
                return 1;
            }
        }
        return 0;
    }
    return 1;
}
