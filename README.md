# pagh-libc — C runtime and libc subset for pagh-riscv

A freestanding C library for the **native pagh-riscv ABI** (not Linux compat):
`crt0`, raw `ecall` syscalls, `malloc` over `brk`, `string`, `stdio`/`printf`,
`unistd` (`read/write/open/close/lseek`), `stat`, `dirent`, `time`, `exit`.

It is what lets C programs run on pagh: compile with the xPack RISC-V GCC,
link against this library, and `run /mnt/hello`. It is **not glibc** — it is a
small, auditable C runtime for a hobby OS with a 52-syscall ABI. No dynamic
linking, no threads (yet), no locale, no `FILE` buffering beyond `stdout`.

## Build the toolchain

The build expects `riscv-none-elf-gcc` (xPack GCC 15+) on `PATH`, or set
`CROSS=/path/to/xpack-riscv-none-elf-gcc/bin/riscv-none-elf-`.

## Build

```sh
./build.sh                 # examples -> build/{hello,cat,ls}.elf
CROSS=/opt/xpack/bin/riscv-none-elf- ./build.sh
```

`build.sh` compiles `src/*.S`/`src/*.c` into `build/libpagh-c.a` and links each
example at `0x10000` with `link.ld`.

## Run in QEMU

```sh
# copy build/hello.elf into a disk image (mke2fs -d) and boot pagh-riscv
run /mnt/hello
```

## Status

| Area | Provided |
|---|---|
| start/exit | `_start`, `exit`, `atexit` (LIFO), `abort` |
| syscalls | `read write open close lseek fstat getdents64 brk clock_gettime nanosleep uname readlinkat mkdirat unlinkat symlinkat linkat renameat2 fchmodat ftruncate exit sched_yield getpid` |
| memory | `malloc/calloc/realloc/free` (bump `brk`, `free` no-op), `memcpy/memset/memmove/memcmp` |
| strings | `strlen/strcmp/strncmp/strcpy/strncpy/strcat/strchr/strrchr/strstr/strdup` |
| stdio | `printf/fprintf/snprintf/vsnprintf/puts/fputs/putchar/getchar` |
| files | `open/read/write/close/lseek`, `stat/fstat`, `opendir/readdir/closedir` |
| process | `getpid`, `sched_yield`, `sleep/usleep`, `exit` |
| time | `clock_gettime`, `time`, `nanosleep` |

## Honest limits

No dynamic loader, no pthreads, no signals in libc wrappers yet, no `FILE`
buffering for regular files (only stdout is line-unbuffered), no `chdir`
(the ABI has no chdir), no `fork`/`exec` wrappers yet (raw syscalls exist),
no `getenv`/`setenv` (the kernel passes only `PWD`; `environ` is built from it).
