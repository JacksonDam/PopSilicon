/* Itanium C++ ABI pieces the guest expects from libstdc++/libgcc: the RTTI
   vtables and __dynamic_cast, plus the exception entry points (unsupported:
   a throw is reported and the guest escapes, the way a trapped import does). */
static uint32_t pg_ti_vtable[3]; /* __class_type_info, __si_, __vmi_ (guest) */

static uint32_t pg_reloc_value(const char *name)
{
    static struct { char name[96]; uint32_t value; } cache[32];
    static unsigned count;
    for (unsigned i = 0; i < count; ++i) {
        if (!strcmp(cache[i].name, name)) return cache[i].value;
    }
    uint32_t value = 0;
    if (!strcmp(name, "__ZTVN10__cxxabiv117__class_type_infoE")) {
        value = pg_ti_vtable[0] = compat_runtime32_allocate(32, 1);
    } else if (!strcmp(name, "__ZTVN10__cxxabiv120__si_class_type_infoE")) {
        value = pg_ti_vtable[1] = compat_runtime32_allocate(32, 1);
    } else if (!strcmp(name, "__ZTVN10__cxxabiv121__vmi_class_type_infoE")) {
        value = pg_ti_vtable[2] = compat_runtime32_allocate(32, 1);
    } else if (!strcmp(name, "___CFConstantStringClassReference")) {
        value = compat_runtime32_allocate(16, 1);
    } else if (!strcmp(name, "__ZTISt9exception")) {
        value = compat_runtime32_allocate(8, 1);
        uint32_t *info = (void *)(uintptr_t)value;
        info[0] = pg_reloc_value("__ZTVN10__cxxabiv117__class_type_infoE") + 8;
        info[1] = compat_runtime32_copy_cstring("St9exception");
    } else {
        value = guest_thunk_for_dynamic_symbol(name);
    }
    if (count < 32 && strlen(name) < 96) {
        strcpy(cache[count].name, name);
        cache[count].value = value;
        ++count;
    }
    return value;
}

static void peggle_apply_relocations(struct macho_image32 *image)
{
    unsigned unresolved = 0;
    for (uint32_t i = 0; i < image->relocation_count; ++i) {
        const struct macho_reloc32 *reloc = &image->relocations[i];
        uint32_t value = pg_reloc_value(reloc->name);
        if (!value) {
            ++unresolved;
            fprintf(stderr, "compat32: unresolved relocation %s at 0x%08x\n",
                    reloc->name, reloc->address);
            continue;
        }
        *(uint32_t *)(uintptr_t)reloc->address += value;
    }
    if (image->relocation_count) {
        fprintf(stderr, "compat32: bound %u external relocations (%u unresolved)\n",
                image->relocation_count, unresolved);
    }
}

static const char *pg_ti_name(uint32_t ti)
{
    if (ti < 0x1000) return NULL;
    uint32_t name = ((const uint32_t *)(uintptr_t)ti)[1];
    return name >= 0x1000 ? (const char *)(uintptr_t)name : NULL;
}
static bool pg_ti_equal(uint32_t a, uint32_t b)
{
    if (a == b) return true;
    const char *x = pg_ti_name(a), *y = pg_ti_name(b);
    return x && y && !strcmp(x, y);
}
static int pg_ti_kind(uint32_t ti)
{
    uint32_t vtable = ((const uint32_t *)(uintptr_t)ti)[0] - 8;
    for (int kind = 0; kind < 3; ++kind) {
        if (pg_ti_vtable[kind] && vtable == pg_ti_vtable[kind]) return kind;
    }
    return -1;
}
/* Depth-first search of the class hierarchy rooted at (object, ti) for a
   base of type target; the first match wins (the game never casts across
   ambiguous hierarchies). */
static uint32_t pg_find_subobject(uint32_t object, uint32_t ti, uint32_t target, int depth)
{
    if (ti < 0x1000 || depth > 32) return 0;
    if (pg_ti_equal(ti, target)) return object;
    const uint32_t *info = (const void *)(uintptr_t)ti;
    int kind = pg_ti_kind(ti);
    if (kind == 1) return pg_find_subobject(object, info[2], target, depth + 1);
    if (kind != 2) return 0;
    uint32_t base_count = info[3];
    for (uint32_t i = 0; i < base_count && i < 32; ++i) {
        int32_t offset_flags = (int32_t)info[5 + 2 * i];
        int32_t offset = offset_flags >> 8;
        if (offset_flags & 1) { /* virtual base: offset lives in the vtable */
            uint32_t vptr = *(const uint32_t *)(uintptr_t)object;
            offset = *(const int32_t *)(uintptr_t)(vptr + offset);
        }
        uint32_t found = pg_find_subobject(object + offset, info[4 + 2 * i], target, depth + 1);
        if (found) return found;
    }
    return 0;
}

static void pg_escape_guest(const char *reason)
{
    fprintf(stderr, "compat32: %s; leaving guest\n", reason);
    last_call_trapped = 1;
    lp32_leave_guest = 1;
}

static int peggle_cxx_dispatch(const char *n, const uint32_t *a, uint64_t *r)
{
#define IS(s) (strcmp(n, s) == 0)
#define RET(x) do { *r = (uint64_t)(x); return 1; } while (0)
    if (IS("___dynamic_cast")) {
        if (a[0] < 0x1000) RET(0);
        uint32_t vptr = *(const uint32_t *)(uintptr_t)a[0];
        if (vptr < 0x1000) RET(0);
        int32_t offset_to_top = *(const int32_t *)(uintptr_t)(vptr - 8);
        uint32_t whole_ti = *(const uint32_t *)(uintptr_t)(vptr - 4);
        RET(pg_find_subobject(a[0] + offset_to_top, whole_ti, a[2], 0));
    }
    if (IS("___cxa_allocate_exception")) RET(compat_runtime32_allocate(a[0] ? a[0] : 4, 1));
    if (IS("___cxa_free_exception")) { compat_runtime32_deallocate(a[0]); RET(0); }
    if (IS("___cxa_begin_catch")) RET(a[0]);
    if (IS("___cxa_end_catch")) RET(0);
    if (IS("___cxa_throw")) {
        const char *type = pg_ti_name(a[1]);
        const uint32_t *object = a[0] >= 0x1000 ? (const void *)(uintptr_t)a[0] : NULL;
        const char *text = "";
        if (object && object[1] >= kGuestHeapBase && object[1] < kGuestHeapEnd) {
            text = (const char *)(uintptr_t)object[1]; /* std::string member */
        }
        char reason[512];
        snprintf(reason, sizeof(reason),
                 "guest threw C++ exception %s at 0x%08x (%08x %08x %08x %08x) \"%.200s\"",
                 type ? type : "?", a[0], object ? object[0] : 0, object ? object[1] : 0,
                 object ? object[2] : 0, object ? object[3] : 0, text);
        pg_escape_guest(reason);
        RET(0);
    }
    if (IS("___cxa_rethrow") || IS("___cxa_call_unexpected") || IS("__Unwind_Resume") ||
        IS("__ZSt9terminatev") || IS("__ZSt17__throw_bad_allocv") ||
        IS("__ZSt20__throw_length_errorPKc") || IS("___cxa_pure_virtual") || IS("_longjmp")) {
        char reason[160];
        snprintf(reason, sizeof(reason), "unsupported C++ runtime call %s", n);
        pg_escape_guest(reason);
        RET(0);
    }
    if (IS("__ZNKSt9exception4whatEv")) {
        static uint32_t text;
        if (!text) text = compat_runtime32_copy_cstring("std::exception");
        RET(text);
    }
    if (IS("__ZNSt9exceptionD2Ev") || IS("__ZNSt9exceptionD1Ev")) RET(0);
    return 0;
#undef RET
#undef IS
}
