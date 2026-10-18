/* Read-only integration test through the actual i386 gateway. Never calls FileWrite. */
static int steam_cloud_selftest(void)
{
    uint32_t args[4] = {0};
    uint64_t result = 0;
    if (!steam_cloud_dispatch("_SteamAPI_Init", args, &result) || result != 1) return -1;
    if (!steam_cloud_dispatch("_SteamRemoteStorage", args, &result) || !result) return -1;
    args[0] = (uint32_t)result;
    uint32_t *table = (void *)(uintptr_t)*(uint32_t *)(uintptr_t)args[0];
    int32_t count = (int32_t)compat_runtime32_call(table[4], args, 4);
    if (count < 0 || compat_runtime32_last_call_trapped()) return -1;
    uint32_t output = compat_runtime32_allocate(16, 1);
    if (!output) return -1;
    uint32_t *words = (void *)(uintptr_t)output;
    words[0] = words[3] = 0xdeadbeef;
    args[1] = output + 4; args[2] = output + 8;
    result = compat_runtime32_call(table[6], args, 4);
    if (!result || words[0] != 0xdeadbeef || words[3] != 0xdeadbeef) return -1;
    printf("Steam Cloud self-test: quota %u, available %u\n", words[1], words[2]);
    for (int32_t i = 0; i < count; ++i) {
        args[1] = (uint32_t)i; args[2] = output + 4;
        uint32_t name = compat_runtime32_call(table[5], args, 4);
        int32_t size = (int32_t)words[1];
        if (!name || size < 0 || size > 100 * 1024 * 1024) return -1;
        args[1] = name;
        if (!compat_runtime32_call(table[3], args, 4)) return -1;
        if ((int32_t)compat_runtime32_call(table[1], args, 4) != size) return -1;
        uint32_t buffer = compat_runtime32_allocate(size ? (size_t)size : 1, 1);
        if (!buffer) return -1;
        args[2] = buffer; args[3] = (uint32_t)size;
        int32_t read = (int32_t)compat_runtime32_call(table[2], args, 4);
        compat_runtime32_deallocate(buffer);
        if (read != size || compat_runtime32_last_call_trapped()) return -1;
        printf("Steam Cloud self-test: read %s (%d bytes)\n", (char *)(uintptr_t)name, read);
    }
    puts("Steam Cloud read-only i386 gateway self-test: PASS");
    return 0;
}
