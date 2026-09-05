#include "macho_loader.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <mach/vm_prot.h>
#include <mach-o/loader.h>
#include <mach-o/reloc.h>
#include <mach-o/nlist.h>
#include <mach/i386/thread_status.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct source_file {
    const uint8_t *bytes;
    size_t size;
};

/*
 * The __LEGACY zerofill in address_space_reserve.S pins the whole low 2 GiB.
 * Only the pages the image actually occupies are released here; everything
 * else in the reservation is carved with MAP_FIXED by the runtime (guest heap
 * at 0x02000000, stacks and bridge pages at 0x7c000000+), so the image must
 * end below the heap base.
 */
enum {
    kLegacyReservationStart = 0x00001000,
    kLegacyReservationEnd = 0x02000000,
    kLegacyPageSize = 0x1000,
};

static int fail_message(const char *message)
{
    fprintf(stderr, "game_loader: %s\n", message);
    return -1;
}

static int fail_errno(const char *what)
{
    fprintf(stderr, "game_loader: %s: %s\n", what, strerror(errno));
    return -1;
}

static int open_source(const char *path, struct source_file *source)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return fail_errno("open recovered Mach-O");

    struct stat status;
    if (fstat(fd, &status) != 0) {
        close(fd);
        return fail_errno("fstat recovered Mach-O");
    }
    if (status.st_size <= 0 || (uint64_t)status.st_size > SIZE_MAX) {
        close(fd);
        return fail_message("invalid recovered Mach-O size");
    }

    void *mapping = mmap(NULL, (size_t)status.st_size, PROT_READ,
                         MAP_PRIVATE, fd, 0);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    if (mapping == MAP_FAILED) return fail_errno("mmap recovered Mach-O");

    source->bytes = mapping;
    source->size = (size_t)status.st_size;
    return 0;
}

static bool range_inside(size_t offset, size_t length, size_t container_size)
{
    return offset <= container_size && length <= container_size - offset;
}

static int verify_address_hole(uint32_t start, uint32_t end)
{
    mach_vm_address_t address = start;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object_name = MACH_PORT_NULL;
    kern_return_t result = mach_vm_region(
        mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
        (vm_region_info_t)&info, &count, &object_name);

    if (object_name != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), object_name);
    }
    if (result != KERN_SUCCESS && result != KERN_INVALID_ADDRESS) {
        fprintf(stderr, "game_loader: mach_vm_region failed: %s\n",
                mach_error_string(result));
        return -1;
    }
    if (result == KERN_SUCCESS && address < end) {
        fprintf(stderr,
                "game_loader: legacy range 0x%08" PRIx32 "-0x%08" PRIx32
                " conflicts with mapping at 0x%016llx-0x%016llx\n",
                start, end, address, address + size);
        return -1;
    }
    return 0;
}

static int release_legacy_reservation(uint32_t image_start, uint32_t image_end)
{
    uintptr_t reservation = kLegacyReservationStart;
    uint32_t release_end = (image_end + kLegacyPageSize - 1) & ~(uint32_t)(kLegacyPageSize - 1);
    if (image_start < kLegacyReservationStart || release_end > kLegacyReservationEnd) {
        return fail_message("host legacy-address reservation does not cover image");
    }
    if (munmap((void *)reservation, release_end - kLegacyReservationStart) != 0) {
        return fail_errno("munmap host legacy-address reservation");
    }
    return 0;
}

static int mmap_protection(vm_prot_t protection)
{
    int result = 0;
    if (protection & VM_PROT_READ) result |= PROT_READ;
    if (protection & VM_PROT_WRITE) result |= PROT_WRITE;
    if (protection & VM_PROT_EXECUTE) result |= PROT_EXEC;
    return result;
}

static const char *fixed_name(const char name[16], char output[17])
{
    memcpy(output, name, 16);
    output[16] = '\0';
    return output;
}

static int inspect_commands(const struct source_file *source,
                            struct macho_image32 *image)
{
    const struct mach_header *header = (const void *)source->bytes;
    if (source->size < sizeof(*header) || header->magic != MH_MAGIC) {
        return fail_message("recovered image is not a native-endian 32-bit Mach-O");
    }
    if (header->cputype != CPU_TYPE_I386 || header->filetype != MH_EXECUTE) {
        return fail_message("recovered image is not an i386 executable");
    }
    if (!range_inside(sizeof(*header), header->sizeofcmds, source->size)) {
        return fail_message("Mach-O load-command region is truncated");
    }

    uint32_t min_address = UINT32_MAX;
    uint32_t max_address = 0;
    uint32_t segment_count = 0;
    uint32_t initializer_count = 0;
    uint32_t initializer_address = 0;
    uint32_t entry_eip = 0;
    const uint8_t *cursor = source->bytes + sizeof(*header);
    const uint8_t *commands_end = cursor + header->sizeofcmds;

    for (uint32_t index = 0; index < header->ncmds; ++index) {
        if ((size_t)(commands_end - cursor) < sizeof(struct load_command)) {
            return fail_message("Mach-O load command header is truncated");
        }
        const struct load_command *command = (const void *)cursor;
        if (command->cmdsize < sizeof(*command) ||
            (size_t)(commands_end - cursor) < command->cmdsize) {
            return fail_message("Mach-O load command has invalid size");
        }

        if (command->cmd == LC_SEGMENT) {
            if (command->cmdsize < sizeof(struct segment_command)) {
                return fail_message("LC_SEGMENT is truncated");
            }
            const struct segment_command *segment = (const void *)cursor;
            size_t sections_size = (size_t)segment->nsects * sizeof(struct section);
            if (sections_size > command->cmdsize - sizeof(*segment)) {
                return fail_message("LC_SEGMENT section array is truncated");
            }
            if (segment->vmsize && strncmp(segment->segname, SEG_PAGEZERO, 16) != 0) {
                if (segment->vmaddr > UINT32_MAX - segment->vmsize) {
                    return fail_message("Mach-O segment address overflows 32 bits");
                }
                if (!range_inside(segment->fileoff, segment->filesize, source->size)) {
                    return fail_message("Mach-O segment file range is truncated");
                }
                if (segment->filesize > segment->vmsize) {
                    return fail_message("Mach-O segment filesize exceeds vmsize");
                }
                if (segment->vmaddr < min_address) min_address = segment->vmaddr;
                if (segment->vmaddr + segment->vmsize > max_address) {
                    max_address = segment->vmaddr + segment->vmsize;
                }
                ++segment_count;
            }

            const struct section *section = (const void *)(segment + 1);
            for (uint32_t section_index = 0; section_index < segment->nsects;
                 ++section_index) {
                if ((section[section_index].flags & SECTION_TYPE) ==
                    S_MOD_INIT_FUNC_POINTERS) {
                    initializer_count += section[section_index].size / sizeof(uint32_t);
                    initializer_address = section[section_index].addr;
                }
                if (strncmp(section[section_index].sectname, "__cstring", 16) == 0 &&
                    strncmp(segment->segname, SEG_TEXT, 16) == 0) {
                    image->cstring_start = section[section_index].addr;
                    image->cstring_end = section[section_index].addr +
                                         section[section_index].size;
                }
                if (strncmp(section[section_index].sectname, "__cfstring", 16) == 0) {
                    image->cfstring_start = section[section_index].addr;
                    image->cfstring_end = section[section_index].addr +
                                          section[section_index].size;
                }
            }
        } else if (command->cmd == LC_UNIXTHREAD) {
            const uint32_t *words = (const void *)(cursor + sizeof(*command));
            size_t word_count = (command->cmdsize - sizeof(*command)) / sizeof(uint32_t);
            if (word_count >= 2 && words[0] == x86_THREAD_STATE32 &&
                words[1] == x86_THREAD_STATE32_COUNT &&
                word_count >= 2 + x86_THREAD_STATE32_COUNT) {
                const x86_thread_state32_t *state = (const void *)(words + 2);
                entry_eip = state->__eip;
            }
        }
        cursor += command->cmdsize;
    }

    if (cursor != commands_end || min_address == UINT32_MAX || !entry_eip) {
        return fail_message("Mach-O has incomplete segment or entry-point metadata");
    }

    image->entry_eip = entry_eip;
    image->min_address = min_address;
    image->max_address = max_address;
    image->initializer_count = initializer_count;
    image->initializer_address = initializer_address;
    image->segment_count = segment_count;
    return 0;
}

static int collect_imports(struct macho_image32 *image)
{
    const struct mach_header *header = image->header;
    const struct symtab_command *symtab = NULL;
    const struct dysymtab_command *dysymtab = NULL;
    const struct segment_command *linkedit = NULL;
    const uint8_t *cursor = (const uint8_t *)(header + 1);

    for (uint32_t index = 0; index < header->ncmds; ++index) {
        const struct load_command *command = (const void *)cursor;
        if (command->cmd == LC_SYMTAB) symtab = (const void *)cursor;
        if (command->cmd == LC_DYSYMTAB) dysymtab = (const void *)cursor;
        if (command->cmd == LC_SEGMENT) {
            const struct segment_command *segment = (const void *)cursor;
            if (strncmp(segment->segname, SEG_LINKEDIT, 16) == 0) linkedit = segment;
        }
        cursor += command->cmdsize;
    }
    if (!symtab || !dysymtab || !linkedit || linkedit->vmaddr < linkedit->fileoff) {
        return fail_message("Mach-O import metadata is incomplete");
    }

    uintptr_t linkedit_base = linkedit->vmaddr - linkedit->fileoff;
    const struct nlist *symbols = (const void *)(linkedit_base + symtab->symoff);
    const char *strings = (const void *)(linkedit_base + symtab->stroff);
    const uint32_t *indirect = (const void *)(linkedit_base + dysymtab->indirectsymoff);
    cursor = (const uint8_t *)(header + 1);

    for (uint32_t command_index = 0; command_index < header->ncmds; ++command_index) {
        const struct load_command *command = (const void *)cursor;
        if (command->cmd == LC_SEGMENT) {
            const struct segment_command *segment = (const void *)cursor;
            const struct section *sections = (const void *)(segment + 1);
            for (uint32_t section_index = 0; section_index < segment->nsects;
                 ++section_index) {
                uint32_t type = sections[section_index].flags & SECTION_TYPE;
                uint32_t stride = 0;
                enum macho_import32_kind kind;
                if (type == S_NON_LAZY_SYMBOL_POINTERS ||
                    type == S_LAZY_SYMBOL_POINTERS) {
                    stride = sizeof(uint32_t);
                    kind = MACHO_IMPORT32_POINTER;
                } else if (type == S_SYMBOL_STUBS && sections[section_index].reserved2) {
                    stride = sections[section_index].reserved2;
                    kind = MACHO_IMPORT32_STUB;
                } else {
                    continue;
                }

                uint32_t count = sections[section_index].size / stride;
                if (sections[section_index].reserved1 > dysymtab->nindirectsyms ||
                    count > dysymtab->nindirectsyms - sections[section_index].reserved1) {
                    return fail_message("Mach-O indirect-symbol section is invalid");
                }
                for (uint32_t item = 0; item < count; ++item) {
                    uint32_t symbol_index = indirect[sections[section_index].reserved1 + item];
                    if (symbol_index & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) continue;
                    if (symbol_index >= symtab->nsyms ||
                        symbols[symbol_index].n_un.n_strx >= symtab->strsize) {
                        return fail_message("Mach-O indirect symbol index is invalid");
                    }
                    if ((symbols[symbol_index].n_type & N_TYPE) == N_SECT) {
                        uint32_t address = sections[section_index].addr + item * stride;
                        uint32_t target = symbols[symbol_index].n_value;
                        if (kind == MACHO_IMPORT32_POINTER) {
                            memcpy((void *)(uintptr_t)address, &target, 4);
                        } else {
                            uint8_t *stub = (void *)(uintptr_t)address;
                            uint32_t rel = target - (address + 5);
                            stub[0] = 0xe9;
                            memcpy(stub + 1, &rel, 4);
                        }
                        continue;
                    }
                    if ((symbols[symbol_index].n_type & N_TYPE) != N_UNDF) continue;
                    if (image->import_count >= MACHO_IMAGE32_MAX_IMPORTS) {
                        return fail_message("Mach-O import table exceeds loader capacity");
                    }
                    const char *name = strings + symbols[symbol_index].n_un.n_strx;
                    if (!memchr(name, '\0', symtab->strsize - symbols[symbol_index].n_un.n_strx)) {
                        return fail_message("Mach-O import name is unterminated");
                    }
                    struct macho_import32 *import = &image->imports[image->import_count++];
                    import->name = name;
                    import->address = sections[section_index].addr + item * stride;
                    import->kind = kind;
                }
            }
        }
        cursor += command->cmdsize;
    }

    /* Non-PIE i386 executables keep absolute external relocations for weak
       (coalesced) definitions and for data references to undefined symbols;
       dyld adds the symbol address to the word in place.  Defined symbols
       resolve here; undefined ones are recorded for the runtime. */
    const struct relocation_info *relocations =
        (const void *)(linkedit_base + dysymtab->extreloff);
    for (uint32_t index = 0; index < dysymtab->nextrel; ++index) {
        const struct relocation_info *reloc = &relocations[index];
        if (!reloc->r_extern || reloc->r_pcrel || reloc->r_length != 2 ||
            reloc->r_type != GENERIC_RELOC_VANILLA) {
            return fail_message("Mach-O external relocation kind is unsupported");
        }
        uint32_t address = (uint32_t)reloc->r_address;
        if (address < image->min_address || address + 4 > image->max_address ||
            reloc->r_symbolnum >= symtab->nsyms) {
            return fail_message("Mach-O external relocation is out of range");
        }
        const struct nlist *symbol = &symbols[reloc->r_symbolnum];
        uint32_t *slot = (void *)(uintptr_t)address;
        if ((symbol->n_type & N_TYPE) == N_SECT) {
            *slot += symbol->n_value;
            continue;
        }
        if ((symbol->n_type & N_TYPE) != N_UNDF) continue;
        if (image->relocation_count >= MACHO_IMAGE32_MAX_RELOCATIONS) {
            return fail_message("Mach-O relocation table exceeds loader capacity");
        }
        struct macho_reloc32 *record = &image->relocations[image->relocation_count++];
        record->name = strings + symbol->n_un.n_strx;
        record->address = address;
    }
    return 0;
}

static int map_segments(const struct source_file *source,
                        struct macho_image32 *image)
{
    const struct mach_header *header = (const void *)source->bytes;
    const uint8_t *cursor = source->bytes + sizeof(*header);

    for (uint32_t index = 0; index < header->ncmds; ++index) {
        const struct load_command *command = (const void *)cursor;
        if (command->cmd == LC_SEGMENT) {
            const struct segment_command *segment = (const void *)cursor;
            if (segment->vmsize && strncmp(segment->segname, SEG_PAGEZERO, 16) != 0) {
                void *target = (void *)(uintptr_t)segment->vmaddr;
                void *mapping = mmap(target, segment->vmsize,
                                     PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
                if (mapping == MAP_FAILED) return fail_errno("mmap legacy segment");
                if (mapping != target) return fail_message("mmap returned wrong legacy address");
                memcpy(mapping, source->bytes + segment->fileoff, segment->filesize);

                char segment_name[17];
                printf("segment: %-10s 0x%08x-0x%08x file=%u prot=%c%c%c\n",
                       fixed_name(segment->segname, segment_name), segment->vmaddr,
                       segment->vmaddr + segment->vmsize, segment->filesize,
                       segment->initprot & VM_PROT_READ ? 'r' : '-',
                       segment->initprot & VM_PROT_WRITE ? 'w' : '-',
                       segment->initprot & VM_PROT_EXECUTE ? 'x' : '-');
            }
        }
        cursor += command->cmdsize;
    }

    cursor = source->bytes + sizeof(*header);
    for (uint32_t index = 0; index < header->ncmds; ++index) {
        const struct load_command *command = (const void *)cursor;
        if (command->cmd == LC_SEGMENT) {
            const struct segment_command *segment = (const void *)cursor;
            if (segment->vmsize && strncmp(segment->segname, SEG_PAGEZERO, 16) != 0 &&
                mprotect((void *)(uintptr_t)segment->vmaddr, segment->vmsize,
                         mmap_protection(segment->initprot)) != 0) {
                return fail_errno("mprotect legacy segment");
            }
        }
        cursor += command->cmdsize;
    }

    image->header = (const void *)(uintptr_t)image->min_address;
    return 0;
}

int macho_image32_load(const char *path, struct macho_image32 *image)
{
    memset(image, 0, sizeof(*image));
    struct source_file source = {0};
    if (open_source(path, &source) != 0) return -1;

    int result = inspect_commands(&source, image);
    if (result == 0) {
        result = release_legacy_reservation(image->min_address, image->max_address);
    }
    if (result == 0) result = verify_address_hole(image->min_address, image->max_address);
    if (result == 0) result = map_segments(&source, image);
    if (result == 0) result = collect_imports(image);

    int saved_errno = errno;
    munmap((void *)source.bytes, source.size);
    errno = saved_errno;
    return result;
}

void macho_image32_unload(const struct macho_image32 *image)
{
    if (image->header && image->max_address > image->min_address) {
        munmap((void *)(uintptr_t)image->min_address,
               image->max_address - image->min_address);
    }
}
