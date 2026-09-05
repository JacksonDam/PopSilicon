.text
.p2align 4, 0x90
.globl _run_compat32
_run_compat32:
    /*
     * uint32_t run_compat32(uint32_t eip, uint32_t esp, uint16_t cs32)
     *
     * r14 is invisible to compatibility-mode code, so it carries the host
     * stack across the 32-bit interval.  The far pointer itself lives on the
     * host stack.  The low-address 64-bit return stub reverses the xchg and
     * returns here with eax untouched.
     */
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $24, %rsp
    movl %edi, (%rsp)
    movl %edx, 4(%rsp)
    movl %esi, %r14d
    xchgq %r14, %rsp
    ljmp *(%r14)
    ud2
