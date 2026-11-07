#ifndef PAGH_H
#define PAGH_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* Syscall numbers (native pagh ABI, RISC-V generic numbering). */
#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_LSEEK 62
#define SYS_FSTAT 80
#define SYS_GETDENTS64 61
#define SYS_BRK 214
#define SYS_CLOCK_GETTIME 113
#define SYS_NANOSLEEP 101
#define SYS_UNAME 160
#define SYS_READLINKAT 78
#define SYS_GETPID 172
#define SYS_SCHED_YIELD 124
#define SYS_EXIT 93
#define SYS_MKDIRAT 34
#define SYS_UNLINKAT 35
#define SYS_SYMLINKAT 36
#define SYS_LINKAT 37
#define SYS_FCHMODAT 53
#define SYS_FTRUNCATE 46
#define SYS_RENAMEAT2 276
#define SYS_FORK 1000
#define SYS_EXECVE 221
#define SYS_WAIT4 260
#define SYS_GETPPID 173

#define AT_FDCWD (-100)
#define AT_REMOVEDIR 0x200

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x40
#define O_TRUNC 0x80
#define O_APPEND 0x100

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct pagh_stat {
    uint32_t mode;
    uint32_t kind;
    uint64_t size;
    uint64_t ino;
};

long __pagh_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
long __pagh_syscall3(long nr, long a0, long a1, long a2);

#endif

/* Threads, futex, signals, memory. */
#define SYS_CLONE 220
#define SYS_FUTEX 98
#define SYS_GETTID 178
#define SYS_TGKILL 131
#define SYS_KILL 129
#define SYS_RT_SIGACTION 134
#define SYS_RT_SIGPROCMASK 135
#define SYS_RT_SIGRETURN 139
#define SYS_MMAP 222
#define SYS_MUNMAP 215
#define CLONE_VM 0x100
#define CLONE_FS 0x200
#define CLONE_FILES 0x400
#define CLONE_SIGHAND 0x800
#define CLONE_THREAD 0x10000
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SA_RESTORER 0x04000000
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define SIGUSR1 10
#define SIGUSR2 12
#define SIGTERM 15
#define SIGKILL 9
#define SIGINT 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
