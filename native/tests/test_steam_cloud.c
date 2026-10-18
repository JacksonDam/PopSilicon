/* Run with clang -arch x86_64 -Wl,-pagezero_size,0x1000 -Wall -Wextra -Werror -I native/src
   native/tests/test_steam_cloud.c native/src/pvz_save.c -o /tmp/test-steam-cloud && /tmp/test-steam-cloud */
#include <assert.h>
#include <sys/mman.h>
#include "../src/steam_cloud.c"
static char *arena;
static size_t cursor;
static unsigned calls;
static bool succeed = true;
static const struct lp32_game_profile profile = {.title = LP32_TITLE_PLANTSVSZOMBIES};
const struct lp32_game_profile *lp32_profile(void) { return &profile; }
uint32_t compat_runtime32_allocate(size_t size, int clear)
{
    assert(cursor + size < 4096);
    void *p = arena + cursor;
    cursor += (size + 7) & ~7u;
    if (clear) memset(p, 0, size);
    return (uint32_t)(uintptr_t)p;
}
uint32_t compat_runtime32_guest_callback(const char *name)
{
    for (size_t i = 0; i < 7; ++i) if (!strcmp(name, methods[i])) return (uint32_t)i + 1;
    assert(false); return 0;
}
static bool write_file(void *s, const char *n, const void *p, int32_t len)
{ assert(s == storage && !strcmp(n, "test.dat") && len == 4 && !memcmp(p, "SAVE", 4)); ++calls; return succeed; }
static int32_t size_file(void *s, const char *n)
{ assert(s == storage && !strcmp(n, "test.dat")); return 4; }
static int32_t read_file(void *s, const char *n, void *p, int32_t len)
{ assert(s == storage && !strcmp(n, "test.dat") && len == 4); memcpy(p, "SAVE", 4); return succeed ? 4 : 0; }
static bool exists_file(void *s, const char *n)
{ assert(s == storage && !strcmp(n, "test.dat")); return true; }
static int32_t count_files(void *s) { assert(s == storage); return 1; }
static const char *name_file(void *s, int index, int32_t *size)
{ assert(s == storage && index == 0); *size = 4; return "test.dat"; }
static bool get_quota(void *s, uint64_t *total, uint64_t *available)
{ assert(s == storage); *total = UINT64_MAX; *available = 17; return succeed; }
static uint64_t call(size_t slot, uint32_t x, uint32_t y, uint32_t z)
{
    uint32_t a[] = {guest_interface, x, y, z}; uint64_t result = 999;
    assert(steam_cloud_dispatch(methods[slot], a, &result)); return result;
}
int main(void)
{
    arena = mmap((void *)0x30000000, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(arena != MAP_FAILED && (uintptr_t)arena + 4096 <= UINT32_MAX);
    checked = enabled = initialized = true;
    storage = &api;
    api.write = write_file; api.size = size_file; api.read = read_file;
    api.exists = exists_file; api.count = count_files; api.name = name_file; api.quota = get_quota;
    interface32();
    uint32_t *vtable = (void *)(uintptr_t)*(uint32_t *)(uintptr_t)guest_interface;
    for (unsigned i = 0; i < 7; ++i) assert(vtable[i] == i + 1);
    uint32_t name = compat_runtime32_allocate(16, 1), data = compat_runtime32_allocate(4, 1);
    strcpy((void *)(uintptr_t)name, "test.dat"); memcpy((void *)(uintptr_t)data, "SAVE", 4);
    assert(call(0, name, data, 4) == 1 && calls == 1);
    succeed = false; assert(call(0, name, data, 4) == 0 && calls == 2); succeed = true;
    assert(call(0, name, data, UINT32_MAX) == 0 && calls == 2);
    assert(call(0, name, 0, 4) == 0 && calls == 2);
    assert(call(1, name, 0, 0) == 4);
    memset((void *)(uintptr_t)data, 0, 4);
    assert(call(2, name, data, 4) == 4 && !memcmp((void *)(uintptr_t)data, "SAVE", 4));
    assert(call(3, name, 0, 0) == 1 && call(4, 0, 0, 0) == 1);
    uint32_t outputs = compat_runtime32_allocate(16, 1);
    uint32_t *out = (void *)(uintptr_t)outputs;
    out[0] = out[3] = 0xdeadbeef;
    assert(call(5, 0, outputs + 4, 0) == guest_name && out[1] == 4);
    assert(!strcmp((void *)(uintptr_t)guest_name, "test.dat"));
    assert(call(5, 1, outputs + 4, 0) == 0 && out[1] == 0);
    assert(call(6, outputs + 4, outputs + 8, 0) == 1);
    assert(out[0] == 0xdeadbeef && out[1] == INT32_MAX && out[2] == 17 && out[3] == 0xdeadbeef);
    succeed = false; assert(call(6, outputs + 4, outputs + 8, 0) == 0 && !out[1] && !out[2]);
    uint64_t result = 0; uint32_t a[4] = {0};
    assert(!steam_cloud_dispatch("_unrelated", a, &result));
    enabled = false; assert(!steam_cloud_dispatch("_SteamAPI_Init", a, &result));
    puts("Steam Cloud bridge: seven legacy slots, pointer copy, 32-bit quota and failure propagation PASS");
    return 0;
}
