/* pagh native entry: a0 = argc, a1 = argv (a2 = envp), sp 16-byte aligned.
   The C `main` gets (argc, argv, envp); its return value becomes exit code. */
    .section .text.entry, "ax"
    .globl _start
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    mv s0, a0          /* argc */
    mv s1, a1          /* argv */
    /* envp = argv + (argc + 1) * 8: the kernel's SysV frame is
       [argc][argv0..argvN-1][NULL][env0..envK-1][NULL][strings], and a2 is
       not guaranteed to carry envp at the ELF entry. */
    slli t0, s0, 3
    add s2, s1, t0
    addi s2, s2, 8
    la t0, __bss_start
    la t1, __bss_end
1:  bgeu t0, t1, 2f
    sd zero, 0(t0)
    addi t0, t0, 8
    j 1b
2:
    mv a0, s0
    mv a1, s1
    mv a2, s2
    mv a0, s2
    call __pagh_set_environ
    mv a0, s0
    mv a1, s1
    mv a2, s2
    call main
    mv s0, a0
    call __run_atexit
    mv a0, s0
    call exit
3:  j 3b
