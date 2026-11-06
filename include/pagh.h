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
