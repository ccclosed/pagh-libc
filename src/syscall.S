    .text
    .globl __pagh_syscall6
    .type __pagh_syscall6, @function
__pagh_syscall6:
    mv a7, a0
    mv a0, a1
    mv a1, a2
    mv a2, a3
    mv a3, a4
    mv a4, a5
    mv a5, a6
    ecall
    ret
    .size __pagh_syscall6, .-__pagh_syscall6

    .globl __pagh_syscall3
    .type __pagh_syscall3, @function
__pagh_syscall3:
    mv a7, a0
    mv a0, a1
    mv a1, a2
    mv a2, a3
    ecall
    ret
    .size __pagh_syscall3, .-__pagh_syscall3
