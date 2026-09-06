#include "compat_runtime.h"
#include "game_profile.h"
#include "macho_loader.h"
#include "objc_bridge.h"
#include "steam_unwrap.h"
#include "guest_profiler.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uintptr_t guest_breakpoint_address;
static unsigned char guest_breakpoint_original_byte;
static volatile sig_atomic_t guest_breakpoint_stepping;
static volatile sig_atomic_t guest_breakpoint_count;
static unsigned guest_breakpoint_limit;
static unsigned guest_breakpoint_skip;
static int guest_diagnostic_fd = -1;

/* Finder launches do not retain stderr.  Mirror crash and breakpoint output
   into a persistent per-run file without changing normal console logging. */
#define GUEST_DIAGNOSTIC(...)                                              \
    do {                                                                   \
        dprintf(STDERR_FILENO, __VA_ARGS__);                               \
        if (guest_diagnostic_fd >= 0) {                                    \
            dprintf(guest_diagnostic_fd, __VA_ARGS__);                     \
        }                                                                  \
    } while (0)

static void flush_guest_instruction(uintptr_t address);

static void open_guest_diagnostic_log(const char *executable_path)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;

    char directory[PATH_MAX];
    char path[PATH_MAX];
    int length = snprintf(directory, sizeof(directory),
                          "%s/Library/Logs/%s", home,
                          lp32_profile()->log_directory);
    if (length < 0 || (size_t)length >= sizeof(directory)) return;
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
        perror("compat32: create diagnostic log directory");
        return;
    }
    length = snprintf(path, sizeof(path), "%s/last-run.log", directory);
    if (length < 0 || (size_t)length >= sizeof(path)) return;

    guest_diagnostic_fd = open(path,
        O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0600);
    if (guest_diagnostic_fd < 0) {
        perror("compat32: open diagnostic log");
        return;
    }
    dprintf(guest_diagnostic_fd,
            "compat32: diagnostic session pid=%ld executable=%s\n",
            (long)getpid(), executable_path ? executable_path : "(unknown)");
    fprintf(stderr, "compat32: persistent diagnostic log: %s\n", path);

    /* A Finder or Dock launch has stderr on /dev/null, which loses every
       bridge message a user's run would need to be diagnosed; keep them in
       the session log instead.  Terminal launches keep their stderr. */
    struct stat stderr_stat, null_stat;
    if (fstat(STDERR_FILENO, &stderr_stat) == 0 &&
        stat("/dev/null", &null_stat) == 0 &&
        stderr_stat.st_dev == null_stat.st_dev &&
        stderr_stat.st_ino == null_stat.st_ino) {
        dup2(guest_diagnostic_fd, STDERR_FILENO);
        setvbuf(stderr, NULL, _IONBF, 0);
        /* GUEST_DIAGNOSTIC writes to both; stderr now is the log. */
        close(guest_diagnostic_fd);
        guest_diagnostic_fd = -1;
        fprintf(stderr, "compat32: stderr redirected to the session log\n");
    }
}

static void emit_u8(unsigned char *code, size_t *offset, unsigned char value)
{
    code[(*offset)++] = value;
}

static void emit_u32(unsigned char *code, size_t *offset, uint32_t value)
{
    memcpy(code + *offset, &value, sizeof(value));
    *offset += sizeof(value);
}

static void emit_rel32(unsigned char *code, size_t *offset,
                       uintptr_t code_address, uintptr_t target)
{
    intptr_t displacement = (intptr_t)target -
                            (intptr_t)(code_address + *offset + 4);
    int32_t relative = (int32_t)displacement;
    memcpy(code + *offset, &relative, sizeof(relative));
    *offset += sizeof(relative);
}

/*
 * During frontend state 2 the title maps any physical-input notification to
 * action 6 (the splash dismiss action).  The old input backend only emitted a
 * notification when a control changed.  Its device-independent polling path
 * emits the seven currently sampled controls every frame on this host, which
 * repeatedly restarts the dismiss fade and leaves the legal splash cycling.
 *
 * Keep the original first notification, latch only while state 2 is active,
 * and reset the latch in every other state.  The patch lives solely in the
 * recovered workspace image and the compatibility bridge; the source bundle
 * is never modified.
 */
static int install_startup_input_latch_patch(void)
{
    const struct lp32_startup_latch_patch *layout = lp32_profile()->startup_latch;
    if (!layout) return 0;

    unsigned char expected[] = {
        0x84, 0xc0,             /* testb %al, %al */
        0x74, 0x00,             /* je original_frontend */
        0x83, 0xfe, 0x02,       /* cmpl $2, %esi */
    };
    expected[3] = (unsigned char)(layout->original_frontend - (layout->hook + 4));
    enum {
        kStubAddress = 0x7f00f000,
        kLatchAddress = 0x7f01fff0,
    };
    const uint32_t kHookAddress = layout->hook;
    const uint32_t kStateTrueAddress = layout->state_true;
    const uint32_t kOriginalFrontendAddress = layout->original_frontend;
    const uint32_t kFunctionEpilogueAddress = layout->function_epilogue;
    const uint32_t kStatePointerAddress = layout->state_pointer;

    unsigned char *hook = (void *)(uintptr_t)kHookAddress;
    unsigned char *stub = (void *)(uintptr_t)kStubAddress;
    volatile unsigned char *latch = (void *)(uintptr_t)kLatchAddress;
    if (memcmp(hook, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "compat32: startup input patch signature mismatch\n");
        return -1;
    }

    unsigned char code[96];
    size_t offset = 0;

    /* Preserve the overwritten state test and branch. */
    emit_u8(code, &offset, 0x84); emit_u8(code, &offset, 0xc0); /* test al,al */
    emit_u8(code, &offset, 0x0f); emit_u8(code, &offset, 0x85); /* jne */
    size_t state_true_relative = offset;
    emit_u32(code, &offset, 0);

    /* The static pointer owns the frontend-state object used by 0x8e6430. */
    emit_u8(code, &offset, 0xa1); /* mov eax, [absolute] */
    emit_u32(code, &offset, kStatePointerAddress);
    emit_u8(code, &offset, 0x83); emit_u8(code, &offset, 0x38);
    emit_u8(code, &offset, 0x02); /* cmp dword ptr [eax], 2 */
    emit_u8(code, &offset, 0x0f); emit_u8(code, &offset, 0x85); /* jne reset */
    size_t reset_relative = offset;
    emit_u32(code, &offset, 0);

    emit_u8(code, &offset, 0x80); emit_u8(code, &offset, 0x3d);
    emit_u32(code, &offset, kLatchAddress);
    emit_u8(code, &offset, 0x00); /* cmp byte ptr [latch], 0 */
    emit_u8(code, &offset, 0x0f); emit_u8(code, &offset, 0x85); /* jne skip */
    size_t skip_relative = offset;
    emit_u32(code, &offset, 0);

    emit_u8(code, &offset, 0xc6); emit_u8(code, &offset, 0x05);
    emit_u32(code, &offset, kLatchAddress);
    emit_u8(code, &offset, 0x01); /* mov byte ptr [latch], 1 */
    emit_u8(code, &offset, 0xe9);
    emit_rel32(code, &offset, kStubAddress, kOriginalFrontendAddress);

    size_t skip_offset = offset;
    emit_u8(code, &offset, 0xb8); emit_u32(code, &offset, 1); /* mov eax, 1 */
    emit_u8(code, &offset, 0xe9);
    emit_rel32(code, &offset, kStubAddress, kFunctionEpilogueAddress);

    size_t reset_offset = offset;
    emit_u8(code, &offset, 0xc6); emit_u8(code, &offset, 0x05);
    emit_u32(code, &offset, kLatchAddress);
    emit_u8(code, &offset, 0x00); /* mov byte ptr [latch], 0 */
    emit_u8(code, &offset, 0xe9);
    emit_rel32(code, &offset, kStubAddress, kOriginalFrontendAddress);

    size_t state_true_offset = offset;
    emit_u8(code, &offset, 0x83); emit_u8(code, &offset, 0xfe);
    emit_u8(code, &offset, 0x02); /* cmp esi, 2 */
    emit_u8(code, &offset, 0xe9);
    emit_rel32(code, &offset, kStubAddress, kStateTrueAddress);

    int32_t relative = (int32_t)((intptr_t)(kStubAddress + state_true_offset) -
                                 (intptr_t)(kStubAddress +
                                            state_true_relative + 4));
    memcpy(code + state_true_relative, &relative, sizeof(relative));
    relative = (int32_t)((intptr_t)(kStubAddress + reset_offset) -
                         (intptr_t)(kStubAddress + reset_relative + 4));
    memcpy(code + reset_relative, &relative, sizeof(relative));
    relative = (int32_t)((intptr_t)(kStubAddress + skip_offset) -
                         (intptr_t)(kStubAddress + skip_relative + 4));
    memcpy(code + skip_relative, &relative, sizeof(relative));

    size_t page_size = (size_t)getpagesize();
    if (mprotect((void *)(uintptr_t)kStubAddress, page_size,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("compat32: mprotect startup input stub");
        return -1;
    }
    memcpy(stub, code, offset);
    flush_guest_instruction(kStubAddress);
    if (mprotect((void *)(uintptr_t)kStubAddress, page_size,
                 PROT_READ | PROT_EXEC) != 0) {
        perror("compat32: protect startup input stub");
        return -1;
    }
    *latch = 0;

    uintptr_t hook_page = kHookAddress & ~(page_size - 1);
    if (mprotect((void *)hook_page, page_size,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("compat32: mprotect startup input hook");
        return -1;
    }
    unsigned char patch[sizeof(expected)] = {0xe9};
    int32_t hook_relative =
        (int32_t)((intptr_t)kStubAddress - (intptr_t)(kHookAddress + 5));
    memcpy(patch + 1, &hook_relative, sizeof(hook_relative));
    patch[5] = 0x90;
    patch[6] = 0x90;
    memcpy(hook, patch, sizeof(patch));
    __builtin___clear_cache((char *)hook, (char *)hook + sizeof(patch));
    if (mprotect((void *)hook_page, page_size, PROT_READ | PROT_EXEC) != 0) {
        perror("compat32: protect startup input hook");
        return -1;
    }
    fprintf(stderr, "compat32: installed startup input repeat latch\n");
    return 0;
}

static int write_guest_code(uintptr_t address, const void *bytes, size_t length,
                            const char *what)
{
    size_t page_size = (size_t)getpagesize();
    uintptr_t page = address & ~(page_size - 1);
    size_t span = (address + length - page + page_size - 1) & ~(page_size - 1);
    if (mprotect((void *)page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        fprintf(stderr, "compat32: mprotect %s: %s\n", what, strerror(errno));
        return -1;
    }
    memcpy((void *)address, bytes, length);
    __builtin___clear_cache((char *)address, (char *)(address + length));
    if (mprotect((void *)page, span, PROT_READ | PROT_EXEC) != 0) {
        fprintf(stderr, "compat32: protect %s: %s\n", what, strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Single SaveCore thread (see lp32_save_worker_patch in game_profile.h).
 * The worker constructor publishes the run flag already set, and the bare
 * thread-spawn call inside the worker's start method goes through a stub that
 * forwards only the first caller (atomic test-and-set on a latch byte); the
 * rest of the start method still runs for every worker object.
 */
static int install_save_worker_patch(void)
{
    const struct lp32_save_worker_patch *layout = lp32_profile()->save_worker;
    if (!layout) return 0;
    enum {
        kStubAddress = 0x7f00f800,
        kLatchAddress = 0x7f01ffe0,
    };

    const struct lp32_code_signature *ctor = &layout->ctor_running_flag;
    unsigned char *ctor_code = (void *)(uintptr_t)ctor->address;
    unsigned char *call_site = (void *)(uintptr_t)layout->start_call;
    unsigned char expected_call[5] = {0xe8};
    int32_t relative = (int32_t)((intptr_t)layout->thread_starter -
                                 (intptr_t)(layout->start_call + 5));
    memcpy(expected_call + 1, &relative, sizeof(relative));
    if (memcmp(ctor_code, ctor->expected, ctor->length) != 0 ||
        memcmp(call_site, expected_call, sizeof(expected_call)) != 0) {
        fprintf(stderr, "compat32: save worker patch signature mismatch\n");
        return -1;
    }

    unsigned char code[32];
    size_t offset = 0;
    emit_u8(code, &offset, 0xb0); emit_u8(code, &offset, 0x01); /* mov al, 1 */
    emit_u8(code, &offset, 0x86); emit_u8(code, &offset, 0x05);
    emit_u32(code, &offset, kLatchAddress);   /* xchg al, byte ptr [latch] */
    emit_u8(code, &offset, 0x84); emit_u8(code, &offset, 0xc0); /* test al, al */
    emit_u8(code, &offset, 0x75); emit_u8(code, &offset, 0x05); /* jne skip */
    emit_u8(code, &offset, 0xe9);                                /* jmp starter */
    emit_rel32(code, &offset, kStubAddress, layout->thread_starter);
    emit_u8(code, &offset, 0x31); emit_u8(code, &offset, 0xc0); /* skip: xor eax, eax */
    emit_u8(code, &offset, 0xc3);                                /* ret (no thread) */

    *(volatile unsigned char *)(uintptr_t)kLatchAddress = 0;
    if (write_guest_code(kStubAddress, code, offset, "save worker stub") != 0) {
        return -1;
    }
    unsigned char ctor_patch[sizeof(ctor->expected)];
    memcpy(ctor_patch, ctor->expected, ctor->length);
    ctor_patch[ctor->length - 1] = 0x01;    /* mov byte [reg+d8], 1 */
    if (write_guest_code(ctor->address, ctor_patch, ctor->length,
                         "save worker constructor") != 0) {
        return -1;
    }
    unsigned char call_patch[5] = {0xe8};
    relative = (int32_t)((intptr_t)kStubAddress -
                         (intptr_t)(layout->start_call + 5));
    memcpy(call_patch + 1, &relative, sizeof(relative));
    if (write_guest_code(layout->start_call, call_patch, sizeof(call_patch),
                         "save worker start call") != 0) {
        return -1;
    }
    fprintf(stderr, "compat32: installed single SaveCore thread patch\n");
    return 0;
}

/*
 * Empty sampler slot guard (see lp32_texture_bind_guard in game_profile.h).
 * The five-byte "mov ecx, [eax+14h]; test ecx, ecx" becomes a jump to a stub
 * that yields ecx = 0 when eax (the bound record) is NULL, so the jnz that
 * follows takes the same skip path the game uses for a record whose field is
 * zero.
 */
static int install_texture_bind_guard(void)
{
    const struct lp32_texture_bind_guard *guard = lp32_profile()->texture_bind_guard;
    if (!guard) return 0;
    enum { kStubAddress = 0x7f00fc00 };

    const struct lp32_code_signature *load = &guard->load;
    if (memcmp((void *)(uintptr_t)load->address, load->expected, load->length) != 0 ||
        load->length != 5 || guard->resume != load->address + 5) {
        fprintf(stderr, "compat32: texture bind guard signature mismatch\n");
        return -1;
    }

    unsigned char code[16];
    size_t offset = 0;
    emit_u8(code, &offset, 0x31); emit_u8(code, &offset, 0xc9); /* xor ecx, ecx */
    emit_u8(code, &offset, 0x85); emit_u8(code, &offset, 0xc0); /* test eax, eax */
    emit_u8(code, &offset, 0x74); emit_u8(code, &offset, 0x03); /* jz skip */
    emit_u8(code, &offset, 0x8b); emit_u8(code, &offset, 0x48);
    emit_u8(code, &offset, 0x14);                                /* mov ecx, [eax+14h] */
    emit_u8(code, &offset, 0x85); emit_u8(code, &offset, 0xc9); /* skip: test ecx, ecx */
    emit_u8(code, &offset, 0xe9);                                /* jmp resume */
    emit_rel32(code, &offset, kStubAddress, guard->resume);
    if (write_guest_code(kStubAddress, code, offset, "texture bind guard stub") != 0) {
        return -1;
    }
    unsigned char hook[5] = {0xe9};
    int32_t relative = (int32_t)((intptr_t)kStubAddress -
                                 (intptr_t)(load->address + 5));
    memcpy(hook + 1, &relative, sizeof(relative));
    if (write_guest_code(load->address, hook, sizeof(hook),
                         "texture bind guard hook") != 0) {
        return -1;
    }
    fprintf(stderr, "compat32: installed empty sampler slot guard\n");
    return 0;
}

static void flush_guest_instruction(uintptr_t address)
{
    __builtin___clear_cache((char *)address, (char *)(address + 1));
}

static void guest_crash_diagnostic(int signal_number, siginfo_t *info,
                                   void *opaque_context)
{
    ucontext_t *context = opaque_context;
    uint64_t rip = context->uc_mcontext->__ss.__rip;
    uint64_t rsp = context->uc_mcontext->__ss.__rsp;
    if (signal_number == SIGTRAP && guest_breakpoint_address) {
        if (guest_breakpoint_stepping) {
            *(volatile unsigned char *)guest_breakpoint_address = 0xcc;
            flush_guest_instruction(guest_breakpoint_address);
            context->uc_mcontext->__ss.__rflags &= ~UINT64_C(0x100);
            guest_breakpoint_stepping = 0;
            return;
        }
        if (rip == guest_breakpoint_address + 1) {
            unsigned count = ++guest_breakpoint_count;
            const uint32_t *words = (const void *)(uintptr_t)rsp;
            if (count > guest_breakpoint_skip) {
            GUEST_DIAGNOSTIC(
                    "compat32: guest breakpoint 0x%08llx hit %u "
                    "eax=%08llx ebx=%08llx ecx=%08llx edx=%08llx "
                    "esi=%08llx edi=%08llx ebp=%08llx esp=%08llx\n",
                    (unsigned long long)guest_breakpoint_address, count,
                    context->uc_mcontext->__ss.__rax & UINT32_MAX,
                    context->uc_mcontext->__ss.__rbx & UINT32_MAX,
                    context->uc_mcontext->__ss.__rcx & UINT32_MAX,
                    context->uc_mcontext->__ss.__rdx & UINT32_MAX,
                    context->uc_mcontext->__ss.__rsi & UINT32_MAX,
                    context->uc_mcontext->__ss.__rdi & UINT32_MAX,
                    context->uc_mcontext->__ss.__rbp & UINT32_MAX,
                    rsp & UINT32_MAX);
            if (rsp >= UINT32_C(0x1000) && rsp < UINT32_C(0x80000000)) {
                for (unsigned index = 0; index < 12; ++index) {
                    GUEST_DIAGNOSTIC(
                            "compat32: breakpoint stack[%u]=0x%08x\n",
                            index, words[index]);
                }
                const char *dump_argument_text =
                    getenv("LP32_TRACE_GUEST_DUMP_ARGUMENT");
                unsigned dump_argument = dump_argument_text ?
                    (unsigned)strtoul(dump_argument_text, NULL, 0) :
                    (getenv("LP32_TRACE_GUEST_DUMP_ARG1") ? 1u : 0u);
                if (dump_argument > 0 && dump_argument < 12 &&
                    words[dump_argument] >= UINT32_C(0x1000) &&
                    words[dump_argument] < UINT32_C(0x80000000)) {
                    const uint32_t *object =
                        (const void *)(uintptr_t)words[dump_argument];
                    for (unsigned index = 0; index < 24; ++index) {
                        GUEST_DIAGNOSTIC(
                                "compat32: breakpoint arg%u+0x%02x=0x%08x\n",
                                dump_argument, index * 4, object[index]);
                    }
                }
                const char *dump_address_text =
                    getenv("LP32_TRACE_GUEST_DUMP_ADDRESS");
                if (dump_address_text && dump_address_text[0]) {
                    uintptr_t dump_address =
                        (uintptr_t)strtoull(dump_address_text, NULL, 0);
                    if (dump_address >= UINT32_C(0x1000) &&
                        dump_address < UINT32_C(0x80000000)) {
                        const uint32_t *dump = (const void *)dump_address;
                        for (unsigned index = 0; index < 16; ++index) {
                            GUEST_DIAGNOSTIC(
                                    "compat32: breakpoint mem[%08llx]=0x%08x\n",
                                    (unsigned long long)(dump_address + index * 4),
                                    dump[index]);
                        }
                        uintptr_t indirect = dump[0];
                        if (indirect >= UINT32_C(0x1000) &&
                            indirect < UINT32_C(0x80000000)) {
                            const uint32_t *object = (const void *)indirect;
                            for (unsigned index = 0; index < 32; ++index) {
                                GUEST_DIAGNOSTIC(
                                        "compat32: breakpoint indirect[%08llx]="
                                        "0x%08x\n",
                                        (unsigned long long)(indirect + index * 4),
                                        object[index]);
                            }
                        }
                    }
                }
                if (getenv("LP32_TRACE_GUEST_DUMP_ESI")) {
                    uintptr_t dump_address =
                        context->uc_mcontext->__ss.__rsi & UINT32_MAX;
                    if (dump_address >= UINT32_C(0x1000) &&
                        dump_address < UINT32_C(0x80000000)) {
                        const uint32_t *dump = (const void *)dump_address;
                        for (unsigned index = 0; index < 20; ++index) {
                            GUEST_DIAGNOSTIC(
                                    "compat32: breakpoint esi+0x%02x="
                                    "0x%08x\n",
                                    index * 4, dump[index]);
                        }
                    }
                }
            }
            }
            *(volatile unsigned char *)guest_breakpoint_address =
                guest_breakpoint_original_byte;
            flush_guest_instruction(guest_breakpoint_address);
            context->uc_mcontext->__ss.__rip = guest_breakpoint_address;
            if (count < guest_breakpoint_limit) {
                context->uc_mcontext->__ss.__rflags |= UINT64_C(0x100);
                guest_breakpoint_stepping = 1;
            }
            return;
        }
    }
    GUEST_DIAGNOSTIC(
            "compat32: signal %d code=%d address=%p "
            "rip=0x%016llx rsp=0x%016llx\n",
            signal_number, info->si_code, info->si_addr, rip, rsp);
    uint32_t mode_to64 = 0, mode_to32 = 0;
    compat_runtime32_mode_guard_counts(&mode_to64, &mode_to32);
    GUEST_DIAGNOSTIC(
            "compat32: crash cs=0x%llx mode-recoveries to64=%u to32=%u\n",
            (unsigned long long)context->uc_mcontext->__ss.__cs,
            mode_to64, mode_to32);
    GUEST_DIAGNOSTIC(
            "compat32: crash rax=%016llx rbx=%016llx rcx=%016llx "
            "rdx=%016llx rsi=%016llx rdi=%016llx rbp=%016llx r14=%016llx\n",
            context->uc_mcontext->__ss.__rax,
            context->uc_mcontext->__ss.__rbx,
            context->uc_mcontext->__ss.__rcx,
            context->uc_mcontext->__ss.__rdx,
            context->uc_mcontext->__ss.__rsi,
            context->uc_mcontext->__ss.__rdi,
            context->uc_mcontext->__ss.__rbp,
            context->uc_mcontext->__ss.__r14);
    if (rsp >= UINT32_C(0x1000) && rsp < UINT32_C(0x80000000)) {
        const uint32_t *words = (const void *)(uintptr_t)rsp;
        for (unsigned index = 0; index < 16; ++index) {
            GUEST_DIAGNOSTIC("compat32: crash stack[%u]=0x%08x\n",
                             index, words[index]);
        }
    }
    /* LP32_CRASH_DUMP=0xaddr:dwords[,...] appends guest cells to the report. */
    const char *dump = getenv("LP32_CRASH_DUMP");
    if (dump && dump[0]) {
        char buffer[256];
        strlcpy(buffer, dump, sizeof(buffer));
        char *cursor = buffer;
        for (char *item = strsep(&cursor, ","); item; item = strsep(&cursor, ",")) {
            char *colon = strchr(item, ':');
            uint32_t address = (uint32_t)strtoul(item, NULL, 0);
            unsigned count = colon ? (unsigned)strtoul(colon + 1, NULL, 0) : 8;
            if (address < 0x1000 || address >= 0x7f000000 || count > 64) continue;
            const uint32_t *words = (const void *)(uintptr_t)address;
            for (unsigned index = 0; index < count; ++index) {
                GUEST_DIAGNOSTIC("compat32: crash mem[0x%08x]=0x%08x\n",
                                 address + 4 * index, words[index]);
            }
        }
    }
    fsync(guest_diagnostic_fd >= 0 ? guest_diagnostic_fd : STDERR_FILENO);
    _exit(128 + signal_number);
}

static void install_guest_crash_diagnostics(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = guest_crash_diagnostic;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &action, NULL);
}

static int configure_guest_breakpoint(const struct macho_image32 *image)
{
    const char *address_text = getenv("LP32_TRACE_GUEST_ADDRESS");
    if (!address_text || !address_text[0]) return 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(address_text, &end, 0);
    if (!end || *end || parsed < image->min_address ||
        parsed >= image->max_address) {
        fprintf(stderr, "game_loader: invalid LP32_TRACE_GUEST_ADDRESS: %s\n",
                address_text);
        return -1;
    }
    const char *limit_text = getenv("LP32_TRACE_GUEST_LIMIT");
    unsigned long parsed_limit = limit_text ? strtoul(limit_text, NULL, 0) : 16;
    guest_breakpoint_limit = parsed_limit > 0 && parsed_limit <= UINT_MAX ?
        (unsigned)parsed_limit : 16;
    const char *skip_text = getenv("LP32_TRACE_GUEST_SKIP");
    unsigned long parsed_skip = skip_text ? strtoul(skip_text, NULL, 0) : 0;
    guest_breakpoint_skip = parsed_skip < guest_breakpoint_limit ?
        (unsigned)parsed_skip : 0;
    guest_breakpoint_address = (uintptr_t)parsed;
    size_t page_size = (size_t)getpagesize();
    uintptr_t page = guest_breakpoint_address & ~(page_size - 1);
    if (mprotect((void *)page, page_size,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("game_loader: mprotect guest breakpoint page");
        guest_breakpoint_address = 0;
        return -1;
    }
    guest_breakpoint_original_byte =
        *(const unsigned char *)guest_breakpoint_address;
    *(volatile unsigned char *)guest_breakpoint_address = 0xcc;
    flush_guest_instruction(guest_breakpoint_address);
    fprintf(stderr,
            "compat32: armed guest breakpoint 0x%08llx original=0x%02x "
            "skip=%u limit=%u\n",
            parsed, guest_breakpoint_original_byte, guest_breakpoint_skip,
            guest_breakpoint_limit);
    return 0;
}

static void runtime_diagnostic_line(const char *line)
{
    GUEST_DIAGNOSTIC("%s", line);
}

/* Bundled launches carry the image in Contents/SharedSupport; pick whichever
   known title's image is present next to the loader. */
static const char *default_image_path(const char *argv0, char *buffer,
                                      size_t size)
{
    const char *slash = strrchr(argv0, '/');
    size_t directory_length = slash ? (size_t)(slash - argv0) : 0;
    size_t profile_count = 0;
    const struct lp32_game_profile *const *profiles = lp32_known_profiles(&profile_count);
    for (size_t index = 0; index < profile_count; ++index) {
        int length = snprintf(buffer, size, "%.*s%s../SharedSupport/%s",
                              (int)directory_length, argv0, slash ? "/" : "",
                              profiles[index]->image_file);
        if (length < 0 || (size_t)length >= size) {
            fprintf(stderr, "game_loader: executable path is too long\n");
            return NULL;
        }
        if (access(buffer, R_OK) == 0) return buffer;
    }
    fprintf(stderr, "game_loader: no game image found in Contents/SharedSupport\n");
    return NULL;
}

int main(int argc, char **argv)
{
    install_guest_crash_diagnostics();
    compat_runtime32_set_diagnostic_sink(runtime_diagnostic_line);
    if (getenv("LP32_CRASH_DIAGNOSTIC_SELFTEST")) {
        raise(SIGSEGV);
        return EXIT_FAILURE;
    }
    if (getenv("LP32_OBJC_PROXY_SELFTEST")) {
        return objc_bridge32_run_proxy_self_test() == 0 ?
            EXIT_SUCCESS : EXIT_FAILURE;
    }
    char image_path_buffer[PATH_MAX];
    const char *image_path = NULL;
    if (argc >= 2 && argv[1][0] != '-') {
        image_path = argv[1];
    } else {
        image_path = default_image_path(argv[0], image_path_buffer,
                                        sizeof(image_path_buffer));
        if (!image_path) return EXIT_FAILURE;
    }

    if (getenv("LP32_PAUSE_FOR_VMMAP")) sleep(30);

    /* Open the session log before the image is examined: a Steam or Finder
       launch has no other way to report an image the loader rejects. */
    open_guest_diagnostic_log(argc > 0 ? argv[0] : NULL);

    /* Steam DRM unwrap: recover a clean image from a DRM-wrapped executable by
       running Valve's own i386 decryptor under the runtime.  This does not run
       the game, so it bypasses the title-profile check and startup patches. */
    const char *unwrap_output = getenv("LP32_UNWRAP_STEAM");

    struct macho_image32 image;
    if (macho_image32_load(image_path, &image) != 0) return EXIT_FAILURE;
    if (!unwrap_output && lp32_profile_select(&image) != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }

    if (!unwrap_output && configure_guest_breakpoint(&image) != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }

    printf("image: segments=%" PRIu32 " range=0x%08" PRIx32 "-0x%08" PRIx32
           " entry=0x%08" PRIx32 " initializers=%" PRIu32 " imports=%" PRIu32 "\n",
           image.segment_count, image.min_address, image.max_address,
           image.entry_eip, image.initializer_count, image.import_count);

    const struct mach_header *mapped_header = image.header;
    if (mapped_header->magic != MH_MAGIC || mapped_header->cputype != CPU_TYPE_I386) {
        fprintf(stderr, "game_loader: mapped header validation failed\n");
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    puts("mapped-image validation: PASS");

    if (compat_runtime32_initialize(&image) != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    if (unwrap_output) {
        int result = steam_unwrap_run(image_path, unwrap_output);
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_HEAP_SELFTEST")) {
        int result = compat_runtime32_run_heap_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_FILE_SELFTEST")) {
        int result = compat_runtime32_run_file_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_CG_SELFTEST")) {
        int result = compat_runtime32_run_cg_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_SYNC_SELFTEST")) {
        int result = compat_runtime32_run_sync_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_GL_PARAMETER_SELFTEST")) {
        int result = objc_bridge32_run_gl_parameter_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_GL_BUFFER_SELFTEST")) {
        int result = objc_bridge32_run_gl_buffer_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_GL_TEXTURE_SELFTEST")) {
        int result = objc_bridge32_run_gl_texture_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (!getenv("LP32_DISABLE_STARTUP_INPUT_LATCH") &&
        install_startup_input_latch_patch() != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    if (!getenv("LP32_DISABLE_SAVE_WORKER_PATCH") &&
        install_save_worker_patch() != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    if (!getenv("LP32_DISABLE_TEXTURE_BIND_GUARD") &&
        install_texture_bind_guard() != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }

    const uint32_t *initializers =
        (const void *)(uintptr_t)image.initializer_address;
    int initialization_trapped = 0;
    for (uint32_t index = 0; index < image.initializer_count; ++index) {
        if (index == 0 || index + 1 == image.initializer_count ||
            (index % 32) == 0) {
            printf("initializer[%" PRIu32 "]: 0x%08" PRIx32 "\n",
                   index, initializers[index]);
        }
        fflush(stdout);
        uint32_t result = compat_runtime32_call(initializers[index], NULL, 0);
        if (compat_runtime32_last_call_trapped()) {
            printf("controlled guest exit after initializer[%" PRIu32
                   "] result=0x%08" PRIx32 "\n", index, result);
            initialization_trapped = 1;
            break;
        }
    }

    if (!initialization_trapped) {
        printf("initializers completed: %" PRIu32 "/%" PRIu32 "\n",
               image.initializer_count, image.initializer_count);

        char host_executable_path[PATH_MAX];
        uint32_t host_executable_path_size = sizeof(host_executable_path);
        if (_NSGetExecutablePath(host_executable_path,
                                 &host_executable_path_size) != 0) {
            fprintf(stderr, "game_loader: executable path is too long\n");
            macho_image32_unload(&image);
            return EXIT_FAILURE;
        }
        uint32_t executable_path =
            compat_runtime32_copy_cstring(host_executable_path);
        const char *extra_argument_text = getenv("LP32_GUEST_EXTRA_ARGUMENT");
        uint32_t extra_argument = extra_argument_text && extra_argument_text[0] ?
            compat_runtime32_copy_cstring(extra_argument_text) : 0;
        uint32_t guest_argc = extra_argument ? 2 : 1;
        uint32_t argv_address = compat_runtime32_allocate(
            (guest_argc + 1) * sizeof(uint32_t), 1);
        uint32_t empty_vector = compat_runtime32_allocate(sizeof(uint32_t), 1);
        if (!executable_path || !argv_address || !empty_vector) {
            fprintf(stderr, "game_loader: could not construct guest process arguments\n");
            macho_image32_unload(&image);
            return EXIT_FAILURE;
        }
        uint32_t *guest_argv = (void *)(uintptr_t)argv_address;
        guest_argv[0] = executable_path;
        if (extra_argument) guest_argv[1] = extra_argument;

        const uint32_t main_arguments[] = {
            guest_argc, argv_address, empty_vector, empty_vector,
        };
        uint32_t main_address = lp32_profile()->main_address;
        if (!main_address) main_address = lp32_profile_main_address(&image);
        if (!main_address) {
            fprintf(stderr, "game_loader: could not locate the game's main\n");
            macho_image32_unload(&image);
            return EXIT_FAILURE;
        }
        lp32_guest_profiler_start();
        printf("entering game main: 0x%08" PRIx32 "\n", main_address);
        fflush(stdout);
        uint32_t result = compat_runtime32_call(
            main_address, main_arguments,
            sizeof(main_arguments) / sizeof(main_arguments[0]));
        printf("game main returned/escaped: 0x%08" PRIx32 " trapped=%d\n",
               result, compat_runtime32_last_call_trapped());
    }
    macho_image32_unload(&image);
    return EXIT_SUCCESS;
}
