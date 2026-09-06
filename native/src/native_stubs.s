# Native i386 implementations of hot, trivial imports.  Each stub is a
# self-contained cdecl leaf (no relocations, no data references) that the
# runtime copies into the guest-visible bridge page and binds to the matching
# import thunk, so the guest calls it without any mode switch into the host.
# tools/gen_native_stubs.py assembles this file and emits native_stubs.inc.
# Conventions: arguments at 4(%esp), 8(%esp)...; integer results in %eax;
# float/double results in %st(0); %ebx/%esi/%edi/%ebp preserved.
.text
.p2align 4

# int __toupper(int c) -- ASCII, the C locale the host handler implemented
.globl _stub_toupper
_stub_toupper:
    movl 4(%esp), %eax
    leal -0x61(%eax), %ecx
    cmpl $0x19, %ecx
    ja 1f
    subl $0x20, %eax
1:  ret

.globl _stub_tolower
_stub_tolower:
    movl 4(%esp), %eax
    leal -0x41(%eax), %ecx
    cmpl $0x19, %ecx
    ja 1f
    addl $0x20, %eax
1:  ret

# GCC 4 COW std::string / std::wstring: object holds one pointer to the
# character data; _Rep {length, capacity, refcount} precedes the data.
.globl _stub_string_length
_stub_string_length:
    movl 4(%esp), %eax
    movl (%eax), %eax
    movl -12(%eax), %eax
    ret

.globl _stub_string_data
_stub_string_data:
    movl 4(%esp), %eax
    movl (%eax), %eax
    ret

.globl _stub_string_empty
_stub_string_empty:
    movl 4(%esp), %eax
    movl (%eax), %eax
    xorl %ecx, %ecx
    cmpl $0, -12(%eax)
    sete %cl
    movl %ecx, %eax
    ret

.globl _stub_string_index
_stub_string_index:
    movl 4(%esp), %eax
    movl (%eax), %eax
    addl 8(%esp), %eax
    ret

.globl _stub_string_end
_stub_string_end:
    movl 4(%esp), %eax
    movl (%eax), %eax
    addl -12(%eax), %eax
    ret

.globl _stub_wstring_index
_stub_wstring_index:
    movl 4(%esp), %eax
    movl (%eax), %eax
    movl 8(%esp), %ecx
    leal (%eax,%ecx,4), %eax
    ret

.globl _stub_wstring_end
_stub_wstring_end:
    movl 4(%esp), %eax
    movl (%eax), %eax
    movl -12(%eax), %ecx
    leal (%eax,%ecx,4), %eax
    ret

# int string::compare(const string&) const  (strcmp of the two data buffers,
# as the host handler did)
.globl _stub_string_compare
_stub_string_compare:
    pushl %esi
    movl 8(%esp), %eax
    movl (%eax), %ecx
    movl 12(%esp), %eax
    movl (%eax), %esi
1:  movzbl (%ecx), %eax
    movzbl (%esi), %edx
    cmpl %edx, %eax
    jne 2f
    testl %eax, %eax
    je 2f
    incl %ecx
    incl %esi
    jmp 1b
2:  subl %edx, %eax
    popl %esi
    ret

.globl _stub_wstring_compare
_stub_wstring_compare:
    pushl %esi
    movl 8(%esp), %eax
    movl (%eax), %ecx
    movl 12(%esp), %eax
    movl (%eax), %esi
1:  movl (%ecx), %eax
    movl (%esi), %edx
    cmpl %edx, %eax
    jne 2f
    testl %eax, %eax
    je 2f
    addl $4, %ecx
    addl $4, %esi
    jmp 1b
2:  subl %edx, %eax
    popl %esi
    ret

.globl _stub_strlen
_stub_strlen:
    movl 4(%esp), %ecx
    movl %ecx, %eax
1:  cmpb $0, (%eax)
    je 2f
    incl %eax
    jmp 1b
2:  subl %ecx, %eax
    ret

.globl _stub_wcslen
_stub_wcslen:
    movl 4(%esp), %ecx
    movl %ecx, %eax
1:  cmpl $0, (%eax)
    je 2f
    addl $4, %eax
    jmp 1b
2:  subl %ecx, %eax
    shrl $2, %eax
    ret

.globl _stub_strcmp
_stub_strcmp:
    pushl %esi
    movl 8(%esp), %ecx
    movl 12(%esp), %esi
1:  movzbl (%ecx), %eax
    movzbl (%esi), %edx
    cmpl %edx, %eax
    jne 2f
    testl %eax, %eax
    je 2f
    incl %ecx
    incl %esi
    jmp 1b
2:  subl %edx, %eax
    popl %esi
    ret

.globl _stub_wcscmp
_stub_wcscmp:
    pushl %esi
    movl 8(%esp), %ecx
    movl 12(%esp), %esi
1:  movl (%ecx), %eax
    movl (%esi), %edx
    cmpl %edx, %eax
    jne 2f
    testl %eax, %eax
    je 2f
    addl $4, %ecx
    addl $4, %esi
    jmp 1b
2:  subl %edx, %eax
    popl %esi
    ret

# int strcasecmp(a, b) -- ASCII case folding
.globl _stub_strcasecmp
_stub_strcasecmp:
    pushl %esi
    pushl %ebx
    movl 12(%esp), %ecx
    movl 16(%esp), %esi
1:  movzbl (%ecx), %eax
    movzbl (%esi), %edx
    leal -0x41(%eax), %ebx
    cmpl $0x19, %ebx
    ja 2f
    addl $0x20, %eax
2:  leal -0x41(%edx), %ebx
    cmpl $0x19, %ebx
    ja 3f
    addl $0x20, %edx
3:  cmpl %edx, %eax
    jne 4f
    testl %eax, %eax
    je 4f
    incl %ecx
    incl %esi
    jmp 1b
4:  subl %edx, %eax
    popl %ebx
    popl %esi
    ret

.globl _stub_memcmp
_stub_memcmp:
    pushl %esi
    pushl %edi
    movl 12(%esp), %esi
    movl 16(%esp), %edi
    movl 20(%esp), %ecx
    xorl %eax, %eax
    testl %ecx, %ecx
    je 3f
    repe cmpsb
    je 3f
    movzbl -1(%esi), %eax
    movzbl -1(%edi), %edx
    subl %edx, %eax
3:  popl %edi
    popl %esi
    ret

.globl _stub_memchr
_stub_memchr:
    pushl %edi
    movl 8(%esp), %edi
    movl 12(%esp), %eax
    movl 16(%esp), %ecx
    testl %ecx, %ecx
    je 1f
    repne scasb
    jne 1f
    leal -1(%edi), %eax
    popl %edi
    ret
1:  xorl %eax, %eax
    popl %edi
    ret

.globl _stub_memcpy
_stub_memcpy:
    pushl %esi
    pushl %edi
    movl 12(%esp), %edi
    movl 16(%esp), %esi
    movl 20(%esp), %ecx
    movl %edi, %eax
    rep movsb
    popl %edi
    popl %esi
    ret

.globl _stub_memmove
_stub_memmove:
    pushl %esi
    pushl %edi
    movl 12(%esp), %edi
    movl 16(%esp), %esi
    movl 20(%esp), %ecx
    movl %edi, %eax
    cmpl %esi, %edi
    jbe 1f
    leal (%esi,%ecx), %edx
    cmpl %edx, %edi
    jae 1f
    leal -1(%esi,%ecx), %esi
    leal -1(%edi,%ecx), %edi
    std
    rep movsb
    cld
    popl %edi
    popl %esi
    ret
1:  rep movsb
    popl %edi
    popl %esi
    ret

.globl _stub_memset
_stub_memset:
    pushl %edi
    movl 8(%esp), %edi
    movl 12(%esp), %eax
    movl 16(%esp), %ecx
    movl %edi, %edx
    rep stosb
    movl %edx, %eax
    popl %edi
    ret

# float floorf(float) / ceilf, double floor / ceil -- SSE2 only.  Values
# outside the int32 range (or NaN) are returned unchanged.
.globl _stub_floorf
_stub_floorf:
    movss 4(%esp), %xmm0
    cvttss2si %xmm0, %eax
    cmpl $0x80000000, %eax
    je 1f
    cvtsi2ss %eax, %xmm1
    ucomiss %xmm0, %xmm1
    jbe 2f
    movl $0x3f800000, %ecx
    movd %ecx, %xmm2
    subss %xmm2, %xmm1
2:  movss %xmm1, 4(%esp)
1:  flds 4(%esp)
    ret

.globl _stub_ceilf
_stub_ceilf:
    movss 4(%esp), %xmm0
    cvttss2si %xmm0, %eax
    cmpl $0x80000000, %eax
    je 1f
    cvtsi2ss %eax, %xmm1
    ucomiss %xmm0, %xmm1
    jae 2f
    movl $0x3f800000, %ecx
    movd %ecx, %xmm2
    addss %xmm2, %xmm1
2:  movss %xmm1, 4(%esp)
1:  flds 4(%esp)
    ret

.globl _stub_floor
_stub_floor:
    movsd 4(%esp), %xmm0
    cvttsd2si %xmm0, %eax
    cmpl $0x80000000, %eax
    je 1f
    cvtsi2sd %eax, %xmm1
    ucomisd %xmm0, %xmm1
    jbe 2f
    movl $0x3ff00000, %ecx
    movd %ecx, %xmm2
    pslldq $4, %xmm2
    subsd %xmm2, %xmm1
2:  movsd %xmm1, 4(%esp)
1:  fldl 4(%esp)
    ret

.globl _stub_ceil
_stub_ceil:
    movsd 4(%esp), %xmm0
    cvttsd2si %xmm0, %eax
    cmpl $0x80000000, %eax
    je 1f
    cvtsi2sd %eax, %xmm1
    ucomisd %xmm0, %xmm1
    jae 2f
    movl $0x3ff00000, %ecx
    movd %ecx, %xmm2
    pslldq $4, %xmm2
    addsd %xmm2, %xmm1
2:  movsd %xmm1, 4(%esp)
1:  fldl 4(%esp)
    ret

.globl _stub_sqrtf
_stub_sqrtf:
    movss 4(%esp), %xmm0
    sqrtss %xmm0, %xmm0
    movss %xmm0, 4(%esp)
    flds 4(%esp)
    ret

.globl _stub_sqrt
_stub_sqrt:
    movsd 4(%esp), %xmm0
    sqrtsd %xmm0, %xmm0
    movsd %xmm0, 4(%esp)
    fldl 4(%esp)
    ret

# x87 transcendental: fsin/fcos are exact enough for the game's angles
.globl _stub_sinf
_stub_sinf:
    flds 4(%esp)
    fsin
    ret

.globl _stub_cosf
_stub_cosf:
    flds 4(%esp)
    fcos
    ret

.globl _stub_sin
_stub_sin:
    fldl 4(%esp)
    fsin
    ret

.globl _stub_cos
_stub_cos:
    fldl 4(%esp)
    fcos
    ret

# OSSpinLock / OSAtomic
.globl _stub_OSSpinLockTry
_stub_OSSpinLockTry:
    movl 4(%esp), %ecx
    xorl %eax, %eax
    movl $1, %edx
    lock cmpxchgl %edx, (%ecx)
    sete %al
    movzbl %al, %eax
    ret

.globl _stub_OSSpinLockLock
_stub_OSSpinLockLock:
    movl 4(%esp), %ecx
1:  xorl %eax, %eax
    movl $1, %edx
    lock cmpxchgl %edx, (%ecx)
    je 2f
    pause
    jmp 1b
2:  ret

.globl _stub_OSSpinLockUnlock
_stub_OSSpinLockUnlock:
    movl 4(%esp), %ecx
    movl $0, (%ecx)
    ret

.globl _stub_OSAtomicAdd32
_stub_OSAtomicAdd32:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    movl %eax, %edx
    lock xaddl %eax, (%ecx)
    addl %edx, %eax
    ret

.globl _stub_OSAtomicCompareAndSwap32
_stub_OSAtomicCompareAndSwap32:
    movl 4(%esp), %eax
    movl 8(%esp), %edx
    movl 12(%esp), %ecx
    lock cmpxchgl %edx, (%ecx)
    sete %al
    movzbl %al, %eax
    ret

.globl _stub_OSAtomicOr32
_stub_OSAtomicOr32:
    movl 8(%esp), %ecx
    movl (%ecx), %eax
1:  movl %eax, %edx
    orl 4(%esp), %edx
    lock cmpxchgl %edx, (%ecx)
    jne 1b
    movl %edx, %eax
    ret

# libstdc++ red-black tree iteration; node {color@0, parent@4, left@8, right@12}
.globl _stub_rb_tree_increment
_stub_rb_tree_increment:
    movl 4(%esp), %eax
    movl 12(%eax), %ecx
    testl %ecx, %ecx
    je 2f
    movl %ecx, %eax
1:  movl 8(%eax), %ecx
    testl %ecx, %ecx
    je 4f
    movl %ecx, %eax
    jmp 1b
2:  movl 4(%eax), %ecx
3:  cmpl 12(%ecx), %eax
    jne 5f
    movl %ecx, %eax
    movl 4(%ecx), %ecx
    jmp 3b
5:  cmpl %ecx, 12(%eax)
    je 4f
    movl %ecx, %eax
4:  ret

.globl _stub_rb_tree_decrement
_stub_rb_tree_decrement:
    movl 4(%esp), %eax
    cmpl $0, (%eax)
    jne 1f
    movl 4(%eax), %ecx
    cmpl %eax, 4(%ecx)
    jne 1f
    movl 12(%eax), %eax
    ret
1:  movl 8(%eax), %ecx
    testl %ecx, %ecx
    je 3f
2:  movl %ecx, %eax
    movl 12(%eax), %ecx
    testl %ecx, %ecx
    jne 2b
    ret
3:  movl 4(%eax), %ecx
4:  cmpl 8(%ecx), %eax
    jne 5f
    movl %ecx, %eax
    movl 4(%ecx), %ecx
    jmp 4b
5:  movl %ecx, %eax
    ret
