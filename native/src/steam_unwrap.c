/*
 * Steam DRM unwrap: run Valve's own i386 decryptor under the compat runtime to
 * recover the cleartext game code, then write a retail-equivalent image.
 *
 * Mechanism (see the reverse-engineering notes): Valve wraps the executable so
 * its __TEXT is encrypted.  At launch, steamloader.dylib (i386) is injected; its
 * CGlobalInitter constructor finds the main image, makes __TEXT writable, and
 * decrypts it in place (calling an appended stub for some variants).  macOS 27
 * cannot run i386, but our runtime can, so we map steamloader's i386 slice into
 * guest space, bind the few libSystem/dyld/mach functions it imports to runtime
 * shims, and call its constructor against the mapped DRM image.  __TEXT is then
 * decrypted in our memory and we serialise a clean image.
 */
#include "steam_unwrap.h"

#include "compat_runtime.h"
#include "macho_loader.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <servers/bootstrap.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/reloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>

/* steamloader's i386 slice is mapped here: above the game image (which ends
   well below 0x00c00000) and below the guest heap at 0x02000000, inside the
   low-2GiB reservation so guest and host addresses are identical. */
enum { kSteamloaderBase = 0x00c00000 };

static const char *const kDefaultSteamloaderPaths[] = {
    "%s/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamloader.dylib",
};

static int fail(const char *message)
{
    fprintf(stderr, "steam_unwrap: %s\n", message);
    return -1;
}

static int fail_errno(const char *what)
{
    fprintf(stderr, "steam_unwrap: %s: %s\n", what, strerror(errno));
    return -1;
}

/* ------------------------------------------------------------------ */
/* Host-service shims the guest decryptor calls.                       */

/* task_info(TASK_DYLD_INFO) hands back this fabricated dyld_all_image_infos;
   steamloader reads its dword at +0x14 into CGlobalInitter+4 (a dyld base used
   only by old-dyld address fix-ups that a natively based image never triggers).
   It must be non-zero (the constructor skips decryption when it is zero) and
   not 0x8fe00000 (the decryptor's "unset" sentinel). */
static uint32_t g_all_image_infos;   /* guest address */
static uint32_t g_dyld_base_value = 0x00001000;

static uint64_t shim_result(uint32_t value) { return value; }

static int g_trace;

/* The in-memory crypto bundle the blob loads via NSCreateObjectFileImageFromMemory
   and links via NSLinkModule.  It is a position-independent i386 MH_BUNDLE that we
   map above the game image and steamloader. */
enum { kBundleBase = 0x00d00000 };
static struct {
    uint32_t image_addr;   /* guest addr of the raw bundle image */
    uint32_t image_size;
    uint32_t base;         /* guest addr where it is mapped (== kBundleBase) */
    int linked;
} g_bundle;

/* Map the bundle at kBundleBase, apply its local relocations, and bind its
   undefined symbols to guest dispatch thunks (libstdc++/libSystem functions the
   runtime already bridges).  Returns 0 on success. */
static int bundle_link(void)
{
    if (g_bundle.linked) return 0;
    if (!g_bundle.image_addr) return fail("no bundle image");
    const uint8_t *img = (const void *)(uintptr_t)g_bundle.image_addr;
    const struct mach_header *h = (const void *)img;
    if (h->magic != MH_MAGIC || h->cputype != CPU_TYPE_I386 ||
        h->filetype != MH_BUNDLE)
        return fail("bundle is not an i386 MH_BUNDLE");

    uint32_t span = 0;
    const uint8_t *cursor = img + sizeof(*h);
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        const struct load_command *lc = (const void *)cursor;
        if (lc->cmd == LC_SEGMENT) {
            const struct segment_command *seg = (const void *)cursor;
            if (seg->vmaddr + seg->vmsize > span) span = seg->vmaddr + seg->vmsize;
        }
        cursor += lc->cmdsize;
    }
    span = (span + 0xfff) & ~0xfffu;
    void *want = (void *)(uintptr_t)kBundleBase;
    if (mmap(want, span, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != want)
        return fail("could not map bundle span");
    memset(want, 0, span);
    g_bundle.base = kBundleBase;

    const struct symtab_command *symtab = NULL;
    const struct dysymtab_command *dysymtab = NULL;
    cursor = img + sizeof(*h);
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        const struct load_command *lc = (const void *)cursor;
        if (lc->cmd == LC_SEGMENT) {
            const struct segment_command *seg = (const void *)cursor;
            if (seg->filesize)
                memcpy((void *)(uintptr_t)(kBundleBase + seg->vmaddr),
                       img + seg->fileoff, seg->filesize);
        } else if (lc->cmd == LC_SYMTAB) {
            symtab = (const void *)cursor;
        } else if (lc->cmd == LC_DYSYMTAB) {
            dysymtab = (const void *)cursor;
        }
        cursor += lc->cmdsize;
    }
    if (!symtab || !dysymtab) return fail("bundle missing symbol tables");
    const struct nlist *syms = (const void *)(img + symtab->symoff);
    const char *strs = (const char *)(img + symtab->stroff);
    const uint32_t *indirect = (const void *)(img + dysymtab->indirectsymoff);

    /* Local relocations (dyld would slide these). */
    const struct relocation_info *loc =
        (const void *)(img + dysymtab->locreloff);
    for (uint32_t i = 0; i < dysymtab->nlocrel; ++i) {
        if (loc[i].r_address & R_SCATTERED) continue;
        uint32_t *cell = (void *)(uintptr_t)(kBundleBase + loc[i].r_address);
        *cell += kBundleBase;
    }

    /* Bind symbol stubs and pointers to guest dispatch thunks by name. */
    cursor = img + sizeof(*h);
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        const struct load_command *lc = (const void *)cursor;
        if (lc->cmd == LC_SEGMENT) {
            const struct segment_command *seg = (const void *)cursor;
            const struct section *sect = (const void *)(seg + 1);
            for (uint32_t s = 0; s < seg->nsects; ++s) {
                uint32_t type = sect[s].flags & SECTION_TYPE;
                if (type == S_SYMBOL_STUBS) continue; /* handled by local relocs */
                if (type != S_LAZY_SYMBOL_POINTERS &&
                    type != S_NON_LAZY_SYMBOL_POINTERS) continue;
                uint32_t count = sect[s].size / 4;
                for (uint32_t e = 0; e < count; ++e) {
                    uint32_t sym = indirect[sect[s].reserved1 + e];
                    uint32_t *ptr =
                        (void *)(uintptr_t)(kBundleBase + sect[s].addr + e * 4);
                    if (sym == INDIRECT_SYMBOL_LOCAL || sym == INDIRECT_SYMBOL_ABS)
                        continue;
                    const char *nm = strs + syms[sym].n_un.n_strx;
                    /* Data imports must resolve to a cell holding the value, not
                       to a code thunk. */
                    if (strcmp(nm, "_bootstrap_port") == 0) {
                        mach_port_t bp = MACH_PORT_NULL;
                        task_get_special_port(mach_task_self(),
                                              TASK_BOOTSTRAP_PORT, &bp);
                        uint32_t cell = compat_runtime32_allocate(4, 1);
                        *(uint32_t *)(uintptr_t)cell = bp;
                        *ptr = cell;
                    } else if (strcmp(nm, "_mach_task_self_") == 0) {
                        uint32_t cell = compat_runtime32_allocate(4, 1);
                        *(uint32_t *)(uintptr_t)cell = mach_task_self();
                        *ptr = cell;
                    } else {
                        *ptr = compat_runtime32_guest_callback(nm);
                    }
                }
            }
        }
        cursor += lc->cmdsize;
    }
    g_bundle.linked = 1;
    return 0;
}

static uint32_t bundle_symbol_address(const char *name)
{
    if (!g_bundle.linked) return 0;
    const uint8_t *img = (const void *)(uintptr_t)g_bundle.image_addr;
    const struct mach_header *h = (const void *)img;
    const struct symtab_command *symtab = NULL;
    const uint8_t *cursor = img + sizeof(*h);
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        const struct load_command *lc = (const void *)cursor;
        if (lc->cmd == LC_SYMTAB) symtab = (const void *)cursor;
        cursor += lc->cmdsize;
    }
    if (!symtab) return 0;
    const struct nlist *syms = (const void *)(img + symtab->symoff);
    const char *strs = (const char *)(img + symtab->stroff);
    /* The blob may look the symbol up with or without a leading underscore. */
    for (uint32_t i = 0; i < symtab->nsyms; ++i) {
        if ((syms[i].n_type & N_TYPE) != N_SECT) continue;
        const char *sn = strs + syms[i].n_un.n_strx;
        if (strcmp(sn, name) == 0 ||
            (sn[0] == '_' && strcmp(sn + 1, name) == 0) ||
            (name[0] == '_' && strcmp(sn, name + 1) == 0)) {
            return g_bundle.base + (uint32_t)syms[i].n_value;
        }
    }
    return 0;
}

static int unwrap_named_import(const char *name, const uint32_t *args,
                               uint64_t *result)
{
    if (g_trace && (strncmp(name, "__dyld", 6) == 0 ||
                    strncmp(name, "_task", 5) == 0 ||
                    strncmp(name, "_mach_vm", 8) == 0 ||
                    strncmp(name, "_vm_", 4) == 0 ||
                    strcmp(name, "_getenv") == 0)) {
        fprintf(stderr, "steam_unwrap: shim %s(%08x,%08x,%08x,%08x,%08x)\n",
                name, args[0], args[1], args[2], args[3], args[4]);
    }
    if (strcmp(name, "__dyld_image_count") == 0) {
        *result = shim_result(1);
        return 1;
    }
    if (strcmp(name, "__dyld_get_image_header") == 0) {
        /* Index 0 is the main (only) image: its mapped Mach header at 0x1000. */
        *result = shim_result(args[0] == 0 ? 0x00001000u : 0u);
        return 1;
    }
    if (strcmp(name, "_task_info") == 0) {
        /* (task, flavor, task_info_out, *count).  Fill a task_dyld_info whose
           all_image_info_addr points at our fabricated struct. */
        uint32_t out = args[2];
        if (out) {
            uint32_t *o = (void *)(uintptr_t)out;
            o[0] = g_all_image_infos;   /* all_image_info_addr (low 32)  */
            o[1] = 0;                   /* (high 32)                     */
            o[2] = 0x100;               /* all_image_info_size (low)     */
            o[3] = 0;
            o[4] = 0;                   /* all_image_info_format         */
        }
        *result = shim_result(KERN_SUCCESS);
        return 1;
    }
    if (strcmp(name, "_vm_protect") == 0) {
        /* (task, address, size, set_maximum, new_protection) */
        uint32_t address = args[1];
        uint32_t size = args[2];
        uint32_t prot = args[4];
        int hostprot = 0;
        if (prot & VM_PROT_READ) hostprot |= PROT_READ;
        if (prot & VM_PROT_WRITE) hostprot |= PROT_WRITE;
        if (prot & VM_PROT_EXECUTE) hostprot |= PROT_EXEC;
        uintptr_t page = (uintptr_t)address & ~(uintptr_t)0xfff;
        uintptr_t end = ((uintptr_t)address + size + 0xfff) & ~(uintptr_t)0xfff;
        if (mprotect((void *)page, end - page, hostprot) != 0) {
            /* Fall back to rwx so the decryptor can keep writing. */
            mprotect((void *)page, end - page, PROT_READ | PROT_WRITE | PROT_EXEC);
        }
        *result = shim_result(KERN_SUCCESS);
        return 1;
    }
    if (strcmp(name, "_mach_vm_region") == 0) {
        /* (task, *address, *size, flavor, info, *count, *object_name).
           Guest memory is host memory, so query the real region. */
        uint32_t addr_ptr = args[1];
        uint32_t size_ptr = args[2];
        uint32_t flavor = args[3];
        uint32_t info_ptr = args[4];
        uint32_t count_ptr = args[5];
        uint32_t object_ptr = args[6];
        mach_vm_address_t haddr = 0;
        if (addr_ptr) {
            uint32_t *p = (void *)(uintptr_t)addr_ptr;
            haddr = ((uint64_t)p[1] << 32) | p[0];
        }
        mach_vm_size_t hsize = 0;
        mach_msg_type_number_t hcount = count_ptr ?
            *(uint32_t *)(uintptr_t)count_ptr : VM_REGION_BASIC_INFO_COUNT_64;
        vm_region_basic_info_data_64_t info;
        if (hcount > VM_REGION_BASIC_INFO_COUNT_64) hcount = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &haddr, &hsize,
                                          flavor, (vm_region_info_t)&info,
                                          &hcount, &object);
        if (kr == KERN_SUCCESS) {
            if (addr_ptr) {
                uint32_t *p = (void *)(uintptr_t)addr_ptr;
                p[0] = (uint32_t)haddr; p[1] = (uint32_t)(haddr >> 32);
            }
            if (size_ptr) {
                uint32_t *p = (void *)(uintptr_t)size_ptr;
                p[0] = (uint32_t)hsize; p[1] = (uint32_t)(hsize >> 32);
            }
            if (info_ptr) memcpy((void *)(uintptr_t)info_ptr, &info, hcount * 4);
            if (count_ptr) *(uint32_t *)(uintptr_t)count_ptr = hcount;
            if (object_ptr) *(uint32_t *)(uintptr_t)object_ptr = 0;
        }
        *result = shim_result(kr);
        return 1;
    }
    if (strcmp(name, "_getenv") == 0) {
        /* steamloader only reads STEAM_PAUSE_LOADER (a debug pause hook); keep
           it unset so it never pauses. */
        *result = 0;
        return 1;
    }
    /* The appended decryptor blob (reached for some DRM variants) resolves the
       functions below via __dyld_lookup_and_bind and then calls them. */
    if (strcmp(name, "__dyld_lookup_and_bind") == 0) {
        /* (symbol_name, *address, *module) */
        const char *sym = (const char *)(uintptr_t)args[0];
        uint32_t addr = sym ? compat_runtime32_guest_callback(sym) : 0;
        if (args[1]) *(uint32_t *)(uintptr_t)args[1] = addr;
        if (args[2]) *(uint32_t *)(uintptr_t)args[2] = addr ? 1u : 0u;
        *result = 0;
        return 1;
    }
    if (strcmp(name, "_mach_task_self") == 0) { *result = mach_task_self(); return 1; }
    if (strcmp(name, "_getpid") == 0) { *result = (uint32_t)getpid(); return 1; }
    if (strcmp(name, "_ptrace") == 0) { *result = 0; return 1; }
    if (strcmp(name, "_memcpy") == 0) {
        if (args[0] && args[1])
            memcpy((void *)(uintptr_t)args[0], (const void *)(uintptr_t)args[1],
                   args[2]);
        *result = args[0];
        return 1;
    }
    if (strcmp(name, "_vm_allocate") == 0) {
        /* (task, *address, size, flags) */
        uint32_t got = compat_runtime32_allocate(args[2], 1);
        if (args[1]) *(uint32_t *)(uintptr_t)args[1] = got;
        *result = got ? KERN_SUCCESS : 3 /* KERN_NO_SPACE */;
        return 1;
    }
    if (strcmp(name, "_vm_deallocate") == 0) { *result = KERN_SUCCESS; return 1; }
    if (strcmp(name, "_NSCreateObjectFileImageFromMemory") == 0) {
        /* (address, size, *objectFileImage): the blob has copied an i386
           MH_BUNDLE (the crypto++ decryptor) into a guest buffer. */
        g_bundle.image_addr = args[0];
        g_bundle.image_size = args[1];
        if (args[2]) *(uint32_t *)(uintptr_t)args[2] = 1; /* opaque handle */
        *result = 1; /* NSObjectFileImageSuccess */
        return 1;
    }
    if (strcmp(name, "_NSLinkModule") == 0) {
        /* (objectFileImage, moduleName, options) -> module handle */
        *result = bundle_link() == 0 ? 1u : 0u;
        return 1;
    }
    if (strcmp(name, "_NSLookupSymbolInModule") == 0) {
        /* (module, symbolName) -> symbol handle (we return the guest address) */
        const char *sym = (const char *)(uintptr_t)args[1];
        *result = sym ? bundle_symbol_address(sym) : 0;
        return 1;
    }
    if (strcmp(name, "_NSAddressOfSymbol") == 0) {
        /* We already returned the address as the symbol handle. */
        *result = args[0];
        return 1;
    }
    if (strcmp(name, "_NSUnLinkModule") == 0) { *result = 1; return 1; }
    /* The crypto bundle verifies ownership by talking to the running Steam
       client over Mach IPC.  Forward these to the host so the guest handshakes
       with the real com.valvesoftware.steam.ipctool service. */
    if (strcmp(name, "_bootstrap_look_up") == 0) {
        mach_port_t sp = MACH_PORT_NULL;
        const char *service = (const char *)(uintptr_t)args[1];
        kern_return_t kr = bootstrap_look_up((mach_port_t)args[0],
                                             (char *)service, &sp);
        if (args[2]) *(uint32_t *)(uintptr_t)args[2] = sp;
        *result = kr;
        return 1;
    }
    if (strcmp(name, "_mach_msg") == 0) {
        /* (msg, option, send_size, rcv_size, rcv_name, timeout, notify) */
        mach_msg_header_t *msg = (void *)(uintptr_t)args[0];
        *result = mach_msg(msg, (mach_msg_option_t)args[1], args[2], args[3],
                           (mach_port_t)args[4], args[5], (mach_port_t)args[6]);
        return 1;
    }
    if (strcmp(name, "_mach_port_allocate") == 0) {
        mach_port_t p = MACH_PORT_NULL;
        kern_return_t kr = mach_port_allocate(mach_task_self(),
                                              (mach_port_right_t)args[1], &p);
        if (args[2]) *(uint32_t *)(uintptr_t)args[2] = p;
        *result = kr;
        return 1;
    }
    if (strcmp(name, "_mach_port_deallocate") == 0) {
        *result = mach_port_deallocate(mach_task_self(), (mach_port_t)args[1]);
        return 1;
    }
    if (strcmp(name, "_launch_msg") == 0) { *result = 1 /* non-zero: error */; return 1; }
    if (strcmp(name, "_kill") == 0 || strcmp(name, "_kill$UNIX2003") == 0) {
        /* The bundle checks the Steam process is alive with kill(pid, 0). */
        *result = (uint32_t)kill((pid_t)args[0], (int)args[1]);
        return 1;
    }
    /* Note: once __TEXT is decrypted, the bundle runs post-decrypt/cleanup code
       (SteamAPI teardown, logging) that is not needed and pulls in more
       libSystem calls.  We deliberately let the first such unhandled import
       trap, which cleanly leaves the guest; the decrypted image is already in
       memory and is dumped by the caller.  Do not add shims that let this
       cleanup run on — it can fault hard and prevent the dump. */
    if (strcmp(name, "___stack_chk_fail") == 0) {
        *result = 0;
        return 1;
    }
    if (strcmp(name, "_pause") == 0 || strcmp(name, "_pause$UNIX2003") == 0) {
        *result = 0;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* steamloader mapping.                                               */

struct mapped_file {
    uint8_t *bytes;
    size_t size;
};

static int map_file(const char *path, struct mapped_file *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return fail_errno("open");
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return fail_errno("fstat"); }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return fail_errno("mmap");
    out->bytes = m;
    out->size = (size_t)st.st_size;
    return 0;
}

/* Find the i386 slice of a (possibly fat) Mach-O. */
static int find_i386_slice(const struct mapped_file *f, const uint8_t **slice,
                           size_t *slice_size)
{
    if (f->size < 8) return fail("steamloader too small");
    uint32_t magic = *(const uint32_t *)f->bytes;
    if (magic == 0xbebafeca /* FAT_MAGIC big-endian */) {
        uint32_t nfat = __builtin_bswap32(((const uint32_t *)f->bytes)[1]);
        for (uint32_t i = 0; i < nfat; ++i) {
            const uint32_t *a = (const uint32_t *)(f->bytes + 8 + i * 20);
            uint32_t cputype = __builtin_bswap32(a[0]);
            uint32_t offset = __builtin_bswap32(a[2]);
            uint32_t size = __builtin_bswap32(a[3]);
            if (cputype == CPU_TYPE_I386 && offset + size <= f->size) {
                *slice = f->bytes + offset;
                *slice_size = size;
                return 0;
            }
        }
        return fail("steamloader has no i386 slice");
    }
    if (magic == MH_MAGIC) {
        const struct mach_header *h = (const void *)f->bytes;
        if (h->cputype != CPU_TYPE_I386) return fail("steamloader is not i386");
        *slice = f->bytes;
        *slice_size = f->size;
        return 0;
    }
    return fail("steamloader is not Mach-O");
}

struct steamloader {
    const uint8_t *slice;
    const struct mach_header *header;
    uint32_t constructor;       /* guest address of CGlobalInitter ctor */
};

/* Map steamloader's segments at kSteamloaderBase and bind its imports. */
static int load_steamloader(const uint8_t *slice, size_t slice_size,
                            struct steamloader *out)
{
    (void)slice_size;
    const struct mach_header *h = (const void *)slice;
    /* Reserve/replace the mapping span for all segments. */
    uint32_t span_end = 0;
    const uint8_t *cursor = slice + sizeof(*h);
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        const struct load_command *lc = (const void *)cursor;
        if (lc->cmd == LC_SEGMENT) {
            const struct segment_command *seg = (const void *)cursor;
            if (seg->vmaddr + seg->vmsize > span_end)
                span_end = seg->vmaddr + seg->vmsize;
        }
        cursor += lc->cmdsize;
    }
    uint32_t span = (span_end + 0xfff) & ~0xfffu;
    void *want = (void *)(uintptr_t)kSteamloaderBase;
    void *got = mmap(want, span, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (got != want) return fail("could not map steamloader span");
    memset(want, 0, span);

    /* Copy segment file contents to base + vmaddr. */
    cursor = slice + sizeof(*h);
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        const struct load_command *lc = (const void *)cursor;
        if (lc->cmd == LC_SEGMENT) {
            const struct segment_command *seg = (const void *)cursor;
            if (seg->filesize) {
                memcpy((void *)(uintptr_t)(kSteamloaderBase + seg->vmaddr),
                       slice + seg->fileoff, seg->filesize);
            }
        }
        cursor += lc->cmdsize;
    }

    /* Locate symtab/dysymtab and the stub / pointer sections. */
    const struct symtab_command *symtab = NULL;
    const struct dysymtab_command *dysymtab = NULL;
    cursor = slice + sizeof(*h);
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        const struct load_command *lc = (const void *)cursor;
        if (lc->cmd == LC_SYMTAB) symtab = (const void *)cursor;
        if (lc->cmd == LC_DYSYMTAB) dysymtab = (const void *)cursor;
        cursor += lc->cmdsize;
    }
    if (!symtab || !dysymtab) return fail("steamloader missing symbol tables");
    const struct nlist *syms = (const void *)(slice + symtab->symoff);
    const char *strs = (const char *)(slice + symtab->stroff);
    const uint32_t *indirect =
        (const void *)(slice + dysymtab->indirectsymoff);

    /* Apply local relocations exactly as dyld would for a slid image: add the
       load slide to each absolute dword.  Covers stub/stub_helper operands,
       the lazy pointers' initial values, and __mod_init_func. */
    const struct relocation_info *locrel =
        (const void *)(slice + dysymtab->locreloff);
    for (uint32_t i = 0; i < dysymtab->nlocrel; ++i) {
        if (locrel[i].r_address & R_SCATTERED) continue;
        uint32_t *cell =
            (void *)(uintptr_t)(kSteamloaderBase + locrel[i].r_address);
        *cell += kSteamloaderBase;
    }

    /* Walk sections; bind lazy/non-lazy pointers and rebase stub operands. */
    cursor = slice + sizeof(*h);
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        const struct load_command *lc = (const void *)cursor;
        if (lc->cmd == LC_SEGMENT) {
            const struct segment_command *seg = (const void *)cursor;
            const struct section *sect = (const void *)(seg + 1);
            for (uint32_t s = 0; s < seg->nsects; ++s) {
                uint32_t type = sect[s].flags & SECTION_TYPE;
                uint32_t stride = 0, count = 0;
                if (type == S_SYMBOL_STUBS) {
                    stride = sect[s].reserved2;
                    count = stride ? sect[s].size / stride : 0;
                } else if (type == S_LAZY_SYMBOL_POINTERS ||
                           type == S_NON_LAZY_SYMBOL_POINTERS) {
                    stride = 4;
                    count = sect[s].size / 4;
                }
                for (uint32_t e = 0; e < count; ++e) {
                    uint32_t sym = indirect[sect[s].reserved1 + e];
                    const char *name = NULL;
                    if (sym != INDIRECT_SYMBOL_LOCAL &&
                        sym != INDIRECT_SYMBOL_ABS) {
                        name = strs + syms[sym].n_un.n_strx;
                    }
                    uint32_t slot = kSteamloaderBase + sect[s].addr + e * stride;
                    if (type == S_SYMBOL_STUBS) {
                        /* Stub operands were already slid by the local
                           relocations above. */
                        continue;
                    }
                    uint32_t *ptr = (void *)(uintptr_t)slot;
                    if (!name) {
                        /* The non-lazy INDIRECT_SYMBOL_LOCAL pointer is Valve's
                           __dyld_func_lookup / __dyld_lookup_and_bind bootstrap:
                           steamloader copies it into the decryptor's parameter
                           block, and the appended blob calls it to resolve the
                           functions it uses.  Point it at our shim thunk. */
                        if (type == S_NON_LAZY_SYMBOL_POINTERS) {
                            *ptr = compat_runtime32_guest_callback(
                                "__dyld_lookup_and_bind");
                        }
                        continue;
                    }
                    if (strcmp(name, "___stack_chk_guard") == 0) {
                        /* Point at a guest dword holding a canary. */
                        uint32_t canary = compat_runtime32_allocate(4, 1);
                        *ptr = canary;
                    } else if (strcmp(name, "_mach_task_self_") == 0) {
                        uint32_t cell = compat_runtime32_allocate(4, 1);
                        *(uint32_t *)(uintptr_t)cell = mach_task_self();
                        *ptr = cell;
                    } else {
                        /* Function: dispatch through a guest thunk. */
                        *ptr = compat_runtime32_guest_callback(name);
                    }
                }
            }
        }
        cursor += lc->cmdsize;
    }

    /* Find the constructor symbol. */
    out->constructor = 0;
    for (uint32_t i = 0; i < symtab->nsyms; ++i) {
        const char *name = strs + syms[i].n_un.n_strx;
        if (strcmp(name, "__ZN14CGlobalInitterC2Ev") == 0) {
            out->constructor = kSteamloaderBase + (uint32_t)syms[i].n_value;
            break;
        }
    }
    if (!out->constructor) return fail("steamloader constructor not found");
    out->slice = slice;
    out->header = h;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dump: write a retail-equivalent image from the decrypted memory.    */

static int write_clean_image(const char *drm_path, const char *output_path)
{
    struct mapped_file f;
    if (map_file(drm_path, &f) != 0) return -1;
    uint8_t *buf = malloc(f.size);
    if (!buf) { munmap((void *)f.bytes, f.size); return fail("oom"); }
    memcpy(buf, f.bytes, f.size);
    munmap((void *)f.bytes, f.size);

    const struct mach_header *h = (const void *)buf;
    const uint8_t *cursor = buf + sizeof(*h);
    uint32_t text_off = 0, text_size = 0, text_addr = 0;
    uint32_t linkedit_lc = 0, linkedit_fileoff = 0;
    uint32_t strtab_end = 0;
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        const struct load_command *lc = (const void *)cursor;
        if (lc->cmd == LC_SEGMENT) {
            const struct segment_command *seg = (const void *)cursor;
            if (strcmp(seg->segname, SEG_LINKEDIT) == 0) {
                linkedit_lc = (uint32_t)(cursor - buf);
                linkedit_fileoff = seg->fileoff;
            }
            const struct section *sect = (const void *)(seg + 1);
            for (uint32_t s = 0; s < seg->nsects; ++s) {
                if (strcmp(sect[s].sectname, "__text") == 0 &&
                    strcmp(sect[s].segname, SEG_TEXT) == 0) {
                    text_off = sect[s].offset;
                    text_size = sect[s].size;
                    text_addr = sect[s].addr;
                }
            }
        } else if (lc->cmd == LC_SYMTAB) {
            const struct symtab_command *st = (const void *)cursor;
            strtab_end = st->stroff + st->strsize;
        }
        cursor += lc->cmdsize;
    }
    if (!text_off || !linkedit_lc || !strtab_end)
        { free(buf); return fail("could not locate image structures"); }

    /* Overwrite __text with the decrypted bytes from mapped guest memory. */
    memcpy(buf + text_off, (const void *)(uintptr_t)text_addr, text_size);

    /* Shrink __LINKEDIT to the retail layout (drop the appended DRM blob) and
       restore its read-only protection. */
    struct segment_command *le = (void *)(buf + linkedit_lc);
    uint32_t le_size = strtab_end - linkedit_fileoff;
    le->filesize = le_size;
    le->vmsize = (le_size + 0xfff) & ~0xfffu;
    le->initprot = VM_PROT_READ;
    /* Retail keeps __LINKEDIT's maxprot at rwx; match it exactly. */
    le->maxprot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;

    int fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(buf); return fail_errno("open output"); }
    ssize_t wrote = write(fd, buf, strtab_end);
    close(fd);
    free(buf);
    if (wrote != (ssize_t)strtab_end) return fail_errno("write output");
    return 0;
}

/* ------------------------------------------------------------------ */

static int resolve_steamloader_path(char *out, size_t size)
{
    const char *override = getenv("LP32_STEAMLOADER");
    if (override && override[0]) {
        if (access(override, R_OK) == 0) {
            snprintf(out, size, "%s", override);
            return 0;
        }
        return fail("LP32_STEAMLOADER path is not readable");
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        for (size_t i = 0; i < sizeof(kDefaultSteamloaderPaths) /
                               sizeof(kDefaultSteamloaderPaths[0]); ++i) {
            snprintf(out, size, kDefaultSteamloaderPaths[i], home);
            if (access(out, R_OK) == 0) return 0;
        }
    }
    return fail("could not find steamloader.dylib (set LP32_STEAMLOADER)");
}

int steam_unwrap_run(const char *drm_image_path, const char *output_path)
{
    g_trace = getenv("LP32_UNWRAP_TRACE") != NULL;
    const char *slide_text = getenv("LP32_UNWRAP_DYLD_BASE");
    if (slide_text && slide_text[0])
        g_dyld_base_value = (uint32_t)strtoul(slide_text, NULL, 0);

    char steamloader_path[PATH_MAX];
    if (resolve_steamloader_path(steamloader_path, sizeof(steamloader_path)) != 0)
        return -1;
    fprintf(stderr, "steam_unwrap: using %s\n", steamloader_path);

    struct mapped_file f;
    if (map_file(steamloader_path, &f) != 0) return -1;
    const uint8_t *slice;
    size_t slice_size;
    if (find_i386_slice(&f, &slice, &slice_size) != 0) {
        munmap((void *)f.bytes, f.size);
        return -1;
    }

    /* Fabricate the dyld_all_image_infos steamloader's task_info shim hands out
       (version >= 2 at +0, dyld base at +0x14). */
    g_all_image_infos = compat_runtime32_allocate(0x100, 1);
    if (!g_all_image_infos) { munmap((void *)f.bytes, f.size); return fail("oom"); }
    uint32_t *aii = (void *)(uintptr_t)g_all_image_infos;
    aii[0] = 2;                              /* version */
    aii[0x14 / 4] = g_dyld_base_value;       /* dyld base -> CGlobalInitter+4 */

    struct steamloader loader;
    if (load_steamloader(slice, slice_size, &loader) != 0) {
        munmap((void *)f.bytes, f.size);
        return -1;
    }

    compat_runtime32_set_named_import_override(unwrap_named_import);

    /* Call CGlobalInitter(this): a zeroed scratch struct on the guest heap. */
    uint32_t self = compat_runtime32_allocate(0x40, 1);
    uint32_t args[1] = { self };
    fprintf(stderr, "steam_unwrap: running decryptor (ctor=0x%08x self=0x%08x "
            "dyld_base=0x%08x)\n", loader.constructor, self, g_dyld_base_value);
    compat_runtime32_call(loader.constructor, args, 1);
    if (compat_runtime32_last_call_trapped())
        fprintf(stderr, "steam_unwrap: warning: decryptor call trapped\n");

    compat_runtime32_set_named_import_override(NULL);
    munmap((void *)f.bytes, f.size);

    if (write_clean_image(drm_image_path, output_path) != 0) return -1;
    fprintf(stderr, "steam_unwrap: wrote %s\n", output_path);
    return 0;
}
