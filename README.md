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
| process | `getpid/getppid`, `fork`, `execv/execve`, `waitpid`, `sched_yield`, `sleep/usleep`, `exit` |
| time | `clock_gettime`, `time`, `nanosleep` |
| threads | `pthread_create/join/exit/self`, `pthread_attr_*`, futex-backed `pthread_mutex_*` over clone/futex (shared address space/heap/fds, 64 KiB stack) |
| signals | `signal/sigaction/raise/kill/sigprocmask` with the kernel restorer; handlers run on the native signal frame |
| cwd | `chdir/getcwd` process-local (the ABI has no chdir); relative `open/stat/...` resolve against it |
| stdio | `FILE`, `fopen/fclose/fflush/fread/fwrite/fgets/fputs/fprintf` with 4 KiB buffering |

## Honest limits

No dynamic loader, no TLS, `fork/execv/waitpid` wrappers exist (0.16 ABI),
no `setenv`/`putenv` (environ is a read-only snapshot from the kernel frame),
no locale, no wide chars, no `chdir` in the kernel (libc keeps a process-local
cwd and rewrites relative paths), no per-thread malloc (the brk heap is shared
and unsynchronized — document this for multithreaded C), no `FILE` locking,
and `pthread_join` requires the thread to be created through this libc (a
16-slot registry).


## Dynamic linking (working)

`ld-pagh` is a userland ELF loader for the native ABI. The kernel still loads
only a static entry program; `ld-pagh` then maps an ET_DYN executable and its
`DT_NEEDED` shared library, applies `R_RISCV_RELATIVE`/`R_RISCV_64`/
`R_RISCV_JUMP_SLOT`, resolves `.dynsym` by name, sets page protections, and
calls the executable's entry.

![dynamic linking](docs/dynamic-linking.svg)

```text
pagh:rv64> run /mnt/ld-pagh /mnt/hello_dyn
hello from a dynamically linked C program
argc=1 argv0=/mnt/hello_dyn
run: /mnt/ld-pagh exited with code 0
```

Build inputs: `build/libpagh-c.so` (LLD `-shared`), `build/hello_dyn.elf`
(PIE `-e main`, `DT_NEEDED libpagh-c.so`) and `build/ld-pagh.elf` (static).
Limits: one `DT_NEEDED` library (the loader also accepts the same directory or
`/mnt`), no symbol versioning, no TLS, no `init`/`fini` arrays, no lazy
binding, no `dlopen`. A statically linked binary does not need the loader.
