# pagh-libc — C runtime and libc subset for pagh-riscv

A freestanding C library for the **native pagh-riscv ABI** (not Linux compat):
`crt0`, raw `ecall` syscalls, `malloc` over `brk`, `string`, `stdio`/`printf`,
`unistd` (`read/write/open/close/lseek`), `stat`, `dirent`, `time`, `exit`.

It is what lets C programs run on pagh: compile with the xPack RISC-V GCC,
link against this library, and `run /mnt/hello`. It is **not glibc** — it is a
small, auditable C runtime for a hobby OS with a 52-syscall ABI. It also ships
the userland dynamic loader `ld-pagh` and, since 0.9.0, a runtime
`dlopen`/`dlsym` API in the shared libc.

Version **0.9.0** (`VERSION`, tag `v0.9.0`).

## Build the toolchain

The build expects `riscv-none-elf-gcc` (xPack GCC 15+) on `PATH`, or set
`CROSS=/path/to/xpack-riscv-none-elf-gcc/bin/riscv-none-elf-`.

## Build

```sh
./build.sh                 # examples + libpagh-c.a/.so + dynamic test objects
CROSS=/opt/xpack/bin/riscv-none-elf- ./build.sh
```

`build.sh` compiles `src/*.S`/`src/*.c` into `build/libpagh-c.a`, builds the
PIC copies into `build/libpagh-c.so`, links each static example at `0x10000`
with `link.ld`, and builds the dynamic test set: `build/hello_dyn.elf`,
`build/libextra.so`, `build/libplug.so` and the static `build/ld-pagh.elf`.

## Run in QEMU

```sh
# copy build/hello into a disk image (mke2fs -d) and boot pagh-riscv 0.16.2
run /mnt/hello
# dynamic:
run /mnt/ld-pagh /mnt/hello_dyn
```

`tools/live-dlopen.sh` builds the disk and drives the dlopen test through a
pagh-riscv checkout's `tools/run.py`; see *Runtime loading* below.

## Status

| Area | Provided |
|---|---|
| start/exit | `_start`, `exit`, `atexit` (LIFO), `abort` |
| syscalls | `read write open close lseek fstat getdents64 brk clock_gettime nanosleep uname readlinkat mkdirat unlinkat symlinkat linkat renameat2 fchmodat ftruncate mmap mprotect exit sched_yield getpid` |
| memory | `malloc/calloc/realloc/free` (bump `brk`, `free` no-op), `memcpy/memset/memmove/memcmp` |
| strings | `strlen/strcmp/strncmp/strcpy/strncpy/strcat/strchr/strrchr/strstr/strdup` |
| stdio | `printf/fprintf/snprintf/vsnprintf/puts/fputs/putchar/getchar`, `FILE`, `fopen/fclose/fflush/fread/fwrite/fgets/fputs/fprintf` with 4 KiB buffering |
| files | `open/read/write/close/lseek`, `stat/fstat`, `opendir/readdir/closedir` |
| env | `getenv/setenv/unsetenv` (mutable layer over the kernel frame) |
| process | `getpid/getppid`, `fork`, `execv/execve`, `waitpid`, `sched_yield`, `sleep/usleep`, `exit` |
| time | `clock_gettime`, `time`, `nanosleep` |
| threads | `pthread_create/join/exit/self`, `pthread_attr_*`, futex-backed `pthread_mutex_*`, `pthread_cond_*`, `pthread_rwlock_*` over clone/futex (shared address space/heap/fds, 64 KiB stack) |
| signals | `signal/sigaction/raise/kill/sigprocmask` with the kernel restorer; handlers run on the native signal frame |
| cwd | `chdir/getcwd` process-local (the ABI has no chdir); relative `open/stat/...` resolve against it |
| dynamic | `ld-pagh` maps ET_DYN executables and up to 8 `DT_NEEDED` objects; `dlopen/dlsym/dlclose/dlerror` add and resolve objects at runtime; `DT_INIT`/`DT_INIT_ARRAY` run |

## Honest limits

The library is deliberately small. It has no TLS (the loader rejects `STT_TLS`
relocations), no locale, no wide characters, no `putenv`, and no kernel `chdir`
(libc keeps a process-local cwd and rewrites relative paths). `malloc` is a bump
allocator: `free` is a no-op, there is no per-thread heap and no locking, and
`realloc` copies `size` bytes without knowing the old size. `FILE` has no
locking. `pthread_join` only works for threads created through this libc (a
16-slot registry).

Dynamic-loader limits are exact:

- `ld-pagh` needs an **ET_DYN** main program and maps objects from anonymous
  pages. `dlopen` needs that same loader: a statically linked program has no
  loaded-object table and gets `dlerror()` = "no loader state".
- Relocations implemented: `R_RISCV_RELATIVE`, `R_RISCV_64`,
  `R_RISCV_JUMP_SLOT`, and the legacy type-6 arm described below. `R_RISCV_32`,
  `COPY`, `IRELATIVE` and TLS relocations fail the load instead of being
  silently misapplied.
- **RISC-V has no `R_RISCV_GLOB_DAT`.** The psABI uses `R_RISCV_64` for data GOT
  slots, and number 6 is `R_RISCV_TLS_DTPMOD32`. The pre-0.9 loader nevertheless
  called type 6 GLOB_DAT; this loader keeps that number as a compatibility arm
  but rejects an `STT_TLS` symbol before applying it. `build.sh` re-tags
  libplug's one real data relocation to type 6 only so the live test exercises
  that arm; normal objects use `R_RISCV_64`.
- No symbol versioning, lazy binding (RTLD_LAZY is accepted but every object is
  bound eagerly), symbol interposition, copy relocations, `DL_FINI`/`FINI_ARRAY`,
  RELRO, `dladdr`, `dlinfo`, `RTLD_NOLOAD` or `RTLD_NEXT`. `RTLD_GLOBAL` and
  `RTLD_LOCAL` are accepted but every loaded object is visible to later
  relocations.
- `dlopen` does not follow the new object's `DT_NEEDED`; its undefined symbols
  must already be in the loaded-object table. `dlsym` searches only the handle's
  own `.dynsym`. A second `dlopen` of the same path returns the same handle
  without running constructors again.
- `dlclose` is a documented no-op: it returns 0 and unloads nothing. The API is
  not thread-safe.
- Object files must retain section headers (`.dynsym`/`.rela*` are read from
  them), be at most 8 MiB, and map at most 64 MiB. `ld-pagh` accepts at most 8
  `DT_NEEDED` entries. The 0.16.2 kernel allows 8 anonymous `mmap` regions per
  process and every object consumes one, so executable + dependencies +
  `dlopen`ed objects must total at most 8.
- A constructor that faults kills the process; there is no recovery path.

## Dynamic linking (working)

`ld-pagh` is a userland ELF loader for the native ABI. The kernel still loads
only a static entry program; `ld-pagh` then maps an ET_DYN executable and its
`DT_NEEDED` shared libraries, resolves `.dynsym` by name, applies
`R_RISCV_RELATIVE`/`R_RISCV_64`/`R_RISCV_JUMP_SLOT`, sets page protections, runs
constructors, and calls the executable's entry.

![dynamic linking](docs/dynamic-linking.svg)

```text
pagh:rv64> run /mnt/ld-pagh /mnt/hello_dyn
hello from a dynamically linked C program
argc=1 argv0=/mnt/hello_dyn extra=42
run: /mnt/ld-pagh exited with code 0
```

The loader follows **multiple `DT_NEEDED`** entries (up to 8), loads each from
the program's directory, resolves symbols across all of them, and protects every
object's pages. Live: `hello_dyn.elf` needs `libextra.so` and `libpagh-c.so`;
`run /mnt/ld-pagh /mnt/hello_dyn` prints `extra=42` and exits 0.

The loader runs **`DT_INIT` and `DT_INIT_ARRAY` constructors** (dependency order
first, executable last) before `e_entry`, so a shared library can initialise its
own state. Live: libextra's constructor sets 7, `extra_answer()` returns 42.

## Runtime loading (`dlopen`)

`libpagh-c.so` exports the dl API from `include/dlfcn.h`:

```c
void *h = dlopen("/mnt/libplug.so", RTLD_NOW);
int (*plug_value)(void) = (int (*)(void))dlsym(h, "plug_value");
int v = plug_value();          /* 42 */
dlclose(h);                    /* no-op */
```

`ld-pagh` publishes a pointer to its loaded-object table through the shared
libc's `__pagh_dl_state` symbol before it runs any constructor. `dlopen` maps
the new ET_DYN object, relocates it against the same executable/libraries that
are already in the process, protects its pages, runs its `DT_INIT` and
`DT_INIT_ARRAY`, and appends it to the table. `dlsym` resolves a name in that
object's `.dynsym`.

Live, QEMU 0.16.2 (`tools/run.py`, full serial log in
[`docs/live-dlopen.txt`](docs/live-dlopen.txt)):

```text
pagh:rv64> run /mnt/ld-pagh /mnt/hello_dyn
hello from a dynamically linked C program
argc=1 argv0=/mnt/hello_dyn extra=42
libplug: DT_INIT
libplug: DT_INIT_ARRAY extra_global=40
dlopen: plug_value=42
run: /mnt/ld-pagh exited with code 0
```

`libplug.so` is linked without `DT_NEEDED`; its `extra_global` (GOT data) and
`printf` (PLT) are undefined until `dlopen` resolves them against libextra.so
and libpagh-c.so. Its two constructors set state that `plug_value()` checks, so
42 proves the runtime load and both constructors ran, rather than a constant
baked into hello_dyn.

#### Reproducing the live test

```sh
./build.sh
PAGH_RISCV=../pagh-riscv tools/live-dlopen.sh
```

The script creates `build/pagh-libc.img` with the built ELFs/shared objects,
boots the kernel from `$PAGH_RISCV` under QEMU, types the command through
`$PAGH_RISCV/tools/run.py`, and checks the transcript for the lines above. It
was run against pagh-riscv tag **0.16.2**; the kernel path can be overridden
with `KERNEL=...`.

Earlier milestones' transcripts remain in `docs/live-dynamic*.txt`; they were
captured before `hello_dyn` gained the dlopen step.
