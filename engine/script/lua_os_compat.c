// What Lua needs from libc that the Minimal-OS runtime does not have:
// setjmp and longjmp (x86-64 System V). Built for Minimal-OS only.
#if defined(FE_MINIMALOS)
__asm__(
    ".text\n"
    ".globl setjmp\n"
    ".type setjmp,@function\n"
    "setjmp:\n"
    "    movq %rbx, 0(%rdi)\n"
    "    movq %rbp, 8(%rdi)\n"
    "    movq %r12, 16(%rdi)\n"
    "    movq %r13, 24(%rdi)\n"
    "    movq %r14, 32(%rdi)\n"
    "    movq %r15, 40(%rdi)\n"
    "    leaq 8(%rsp), %rdx\n"
    "    movq %rdx, 48(%rdi)\n"
    "    movq (%rsp), %rdx\n"
    "    movq %rdx, 56(%rdi)\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".size setjmp, .-setjmp\n"
    ".globl longjmp\n"
    ".type longjmp,@function\n"
    "longjmp:\n"
    "    movl %esi, %eax\n"
    "    testl %eax, %eax\n"
    "    jnz 1f\n"
    "    movl $1, %eax\n"
    "1:  movq 0(%rdi), %rbx\n"
    "    movq 8(%rdi), %rbp\n"
    "    movq 16(%rdi), %r12\n"
    "    movq 24(%rdi), %r13\n"
    "    movq 32(%rdi), %r14\n"
    "    movq 40(%rdi), %r15\n"
    "    movq 48(%rdi), %rsp\n"
    "    jmpq *56(%rdi)\n"
    ".size longjmp, .-longjmp\n"
);
#else
typedef int fe_script_compat_unused;
#endif
