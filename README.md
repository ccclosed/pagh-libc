# pagh-libc — C runtime and libc subset for pagh-riscv

A freestanding C library for the **native pagh-riscv ABI** (not Linux compat):
`crt0`, raw `ecall` syscalls, `malloc` over `brk`, `string`, `stdio`/`printf`,
`unistd` (`read/write/open/close/lseek`), `stat`, `dirent`, `time`, `exit`.

It is what lets C programs run on pagh: compile with the xPack RISC-V GCC,
link against this library, and `run /mnt/hello`. It is **not glibc** — it is a
small, auditable C runtime for a hobby OS with a 52-syscall ABI. It also ships
the userland dynamic loader `ld-pagh` and, since 0.9.0, a runtime
`dlopen`/`dlsym` API in the shared libc; 0.10.0 makes that API follow the new
object's `DT_NEEDED` recursively and adds the `RTLD_GLOBAL`/`RTLD_LOCAL` scopes
and `RTLD_DEFAULT`/`RTLD_NEXT` lookups.

Version **0.10.0** (`VERSION`, tag `v0.10.0`).

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
with `link.ld`, and builds the dynamic test set: `build/hello_dyn.elf`, the
static `build/ld-pagh.elf`, and the shared objects `libextra.so`,
`libpagh-c.so`, `libplug.so` (which `DT_NEEDED`s `libplugdep.so`),
`libglobal.so`, `liblate.so`, `liblatebad.so` and `libbadreloc.so`.

## Run in QEMU

```sh
# copy build/hello into a disk image (mke2fs -d) and boot pagh-riscv 0.16.2/0.18.0
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
| dynamic | `ld-pagh` maps ET_DYN executables and their `DT_NEEDED` tree (recursively, up to 16 objects); `dlopen/dlsym/dlclose/dlerror` add, scope and resolve objects at runtime (`RTLD_GLOBAL`/`RTLD_LOCAL`, `RTLD_DEFAULT`/`RTLD_NEXT`); `DT_INIT`/`DT_INIT_ARRAY` run |

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
- Relocations implemented: `R_RISCV_RELATIVE`, `R_RISCV_32`, `R_RISCV_64`,
  `R_RISCV_JUMP_SLOT`, and the legacy type-6 arm described below. `R_RISCV_32`
  writes `S + A` into a 32-bit field, truncating like any ELF64 32-bit
  relocation does (the psABI expects the linker to guarantee the value fits;
  both test kernels map objects below 4 GiB). `COPY`, `IRELATIVE` and TLS
  relocations fail the load instead of being silently misapplied, and an
  unassigned type fails loudly with `unsupported relocation type N`.
- **RISC-V has no `R_RISCV_GLOB_DAT`.** The psABI uses `R_RISCV_64` for data GOT
  slots, and number 6 is `R_RISCV_TLS_DTPMOD32`. The pre-0.9 loader nevertheless
  called type 6 GLOB_DAT; this loader keeps that number as a compatibility arm
  but rejects an `STT_TLS` symbol before applying it. `build.sh` re-tags
  libplug's `extra_global` relocation to type 6 and its `dep_global` relocation
  to `R_RISCV_32` only so the live test exercises both arms; normal objects use
  `R_RISCV_64`.
- No symbol versioning, no lazy binding (`RTLD_LAZY` is accepted but every
  object is bound eagerly, so there is no PLT resolver to hand a symbol to), no
  copy relocations, no `DL_FINI`/`FINI_ARRAY`, RELRO, `dladdr`, `dlinfo`,
  interposition beyond scope order, or `dlopen(NULL)`. Of the `dlopen` flag
  bits, only `RTLD_GLOBAL` changes behaviour; `RTLD_NOLOAD`/`RTLD_DEEPBIND` are
  not implemented and the other bits are ignored.
- Scope is the group, not the whole process: a relocation in object X is
  resolved against the global scope (the program's `DT_NEEDED` tree plus every
  group opened with `RTLD_GLOBAL`) and then against X's own `dlopen()` group.
  `RTLD_LOCAL` (the default) keeps a group out of the global scope, so later
  objects cannot resolve against it; `dlsym(handle)` still searches the
  handle's group, root first.
- `dlopen` follows the new object's `DT_NEEDED` recursively, matching already
  loaded objects by absolute path, so a shared dependency is mapped once. A
  second `dlopen` of the same path returns the same handle without running the
  constructors again; passing `RTLD_GLOBAL` then still promotes that existing
  group. `dlsym(RTLD_NEXT)` identifies its caller from the return address, so
  it only works from code inside a loaded object.
- `dlclose` is a documented no-op: it returns 0 and unloads nothing. The API is
  not thread-safe.
- Object files must retain section headers (`.dynsym`/`.rela*` are read from
  them), be at most 8 MiB, map at most 64 MiB, carry at most 8 `DT_NEEDED`
  entries, nest at most 8 dependency levels and total at most 16 objects. The
  0.16.2/0.18.0 kernel allows 8 anonymous `mmap` regions per process and every
  object consumes one, so executable + dependencies + live `dlopen`ed objects
  must total at most 8; a `dlopen` that fails after mapping unmaps what it
  mapped, so a rejected object does not permanently consume a region.
- A constructor that faults kills the process; there is no recovery path.

## Dynamic linking (working)

`ld-pagh` is a userland ELF loader for the native ABI. The kernel still loads
only a static entry program; `ld-pagh` then maps an ET_DYN executable and its
`DT_NEEDED` shared libraries, resolves `.dynsym` by name, applies
`R_RISCV_RELATIVE`/`R_RISCV_32`/`R_RISCV_64`/`R_RISCV_JUMP_SLOT`, sets page
protections, runs constructors, and calls the executable's entry.

![dynamic linking](docs/dynamic-linking.svg)

```text
pagh:rv64> run /mnt/ld-pagh /mnt/hello_dyn
hello from a dynamically linked C program
argc=1 argv0=/mnt/hello_dyn extra=42
run: /mnt/ld-pagh exited with code 0
```

The loader follows **multiple `DT_NEEDED`** entries (up to 8 per object,
recursively), loads each from the depending object's directory, resolves
symbols across the whole tree, and protects every object's pages. Live:
`hello_dyn.elf` needs `libextra.so` and `libpagh-c.so`; running
`run /mnt/ld-pagh /mnt/hello_dyn` prints `extra=42` and exits 0.

The loader runs **`DT_INIT` and `DT_INIT_ARRAY` constructors** (dependency order
first, executable last) before `e_entry`, so a shared library can initialise its
own state. Live: libextra's constructor sets 7, `extra_answer()` returns 42.

## Runtime loading (`dlopen`)

`libpagh-c.so` exports the dl API from `include/dlfcn.h`:

```c
void *h = dlopen("/mnt/libplug.so", RTLD_NOW);   /* + its DT_NEEDED tree */
int (*plug_value)(void) = (int (*)(void))dlsym(h, "plug_value");
int v = plug_value();          /* 42 */
dlclose(h);                    /* no-op */
```

`ld-pagh` publishes a pointer to its loaded-object table through the shared
libc's `__pagh_dl_state` symbol before it runs any constructor. `dlopen` then
maps the new ET_DYN object, follows its `DT_NEEDED` entries recursively
(reusing an object that is already loaded, matched by absolute path),
relocates the whole group, protects every object's pages, runs each object's
`DT_INIT`/`DT_INIT_ARRAY` (dependencies first) and appends the group to the
table. A load that fails after mapping unmaps what it mapped, so a rejected
object does not permanently consume one of the kernel's eight anonymous
mappings.

The API is scope-aware:

| call | searches |
|---|---|
| `dlsym(handle, name)` | the handle's object and the dependencies `dlopen` loaded with it, root first |
| `dlsym(RTLD_DEFAULT, name)` | the global scope: the program's `DT_NEEDED` tree plus every `RTLD_GLOBAL` group, in load order |
| `dlsym(RTLD_NEXT, name)` | the caller's scope after the caller's own object (the caller is identified from the return address) |
| a relocation in a newly loaded object | the global scope first, then the object's own group |

So `RTLD_GLOBAL` means "later objects and later `dlsym(RTLD_DEFAULT)` calls can
resolve against this group", and `RTLD_LOCAL` (the default) means they cannot.
`RTLD_LAZY` is accepted, but binding is eager in both cases.

Live, on both test kernels (`tools/run.py`; full serial logs in
[`docs/live-dlopen-0.16.2.txt`](docs/live-dlopen-0.16.2.txt) and
[`docs/live-dlopen-0.18.0.txt`](docs/live-dlopen-0.18.0.txt)):

```text
pagh:rv64> run /mnt/ld-pagh /mnt/hello_dyn
hello from a dynamically linked C program
argc=1 argv0=/mnt/hello_dyn extra=42
libplugdep: DT_INIT_ARRAY dep_global=40
libplug: DT_INIT
libplug: DT_INIT_ARRAY extra_global=40
dlopen: plug_value=42
dlopen: second dlopen reuses the handle, init count still 2
dlopen: dlsym(handle) dep_answer=42
dlopen: RTLD_LOCAL group hidden from RTLD_DEFAULT
libglobal: DT_INIT_ARRAY
dlopen: RTLD_GLOBAL global_answer=77
liblate: DT_INIT_ARRAY global_answer=77
dlopen: later plugin resolved the RTLD_GLOBAL symbol
dlopen: RTLD_LOCAL-only symbol rejected: unresolved symbol dep_answer
dlopen: unsupported relocation rejected: unsupported relocation type 999
dlopen: RTLD_DEFAULT interpose_value=1, RTLD_NEXT interpose_value=2
dlopen: RTLD_NEXT skips RTLD_LOCAL-only symbols
dlopen: RTLD_GLOBAL promotes an already-loaded group
run: /mnt/ld-pagh exited with code 0
```

What each line proves (hello_dyn returns non-zero on the first failed check, so
`exited with code 0` is the aggregate verdict):

- `libplugdep: DT_INIT_ARRAY dep_global=40` — nothing but `libplug.so`'s
  `DT_NEEDED` names `libplugdep.so`; `dlopen("/mnt/libplug.so")` mapped it,
  relocated it and ran its constructor.
- `plug_value=42` — libplug's imported `extra_global` (GOT data, re-tagged to
  the legacy type-6 arm), `dep_global` (re-tagged to `R_RISCV_32`, so only its
  low 4 bytes are written) and `printf`/`dep_is_ready` (PLT) all resolved, and
  both objects' constructors ran: 40 + 40 - 38.
- `second dlopen reuses the handle, init count still 2` — re-opening the same
  path returns the same handle, and libplug's own constructor counter (bumped
  by `DT_INIT` and `DT_INIT_ARRAY`) is still 2.
- `dlsym(handle) dep_answer=42` — the handle searches the dependency too.
- `RTLD_LOCAL group hidden from RTLD_DEFAULT` — the same group is *not* in the
  global scope.
- `RTLD_GLOBAL global_answer=77` — after
  `dlopen("/mnt/libglobal.so", RTLD_NOW | RTLD_GLOBAL)`, `dlsym(RTLD_DEFAULT,
  ...)` finds its symbol.
- `liblate: DT_INIT_ARRAY global_answer=77` — a later plugin with no
  `DT_NEEDED` resolved its undefined symbol against that global scope.
- `RTLD_LOCAL-only symbol rejected: unresolved symbol dep_answer` — a second
  late plugin asking for the local group's symbol fails, naming it.
- `unsupported relocation rejected: unsupported relocation type 999` —
  `build.sh` re-tags one relocation of `libbadreloc.so` to an unassigned type;
  the load fails loudly instead of writing garbage. Two rejected loads in a row
  at the eight-mapping limit also prove the rollback unmaps.
- `RTLD_DEFAULT interpose_value=1, RTLD_NEXT interpose_value=2` — both the
  executable and libglobal define `interpose_value`; the default search starts
  at the executable, `RTLD_NEXT` skips it.
- `RTLD_NEXT skips RTLD_LOCAL-only symbols` — `RTLD_NEXT` obeys the scope.
- `RTLD_GLOBAL promotes an already-loaded group` — re-opening the RTLD_LOCAL
  libplug with `RTLD_GLOBAL` adds the existing group to the global scope
  without reloading it.

`libplug.so` itself has no link-time reference to `libextra.so`; its
`extra_global` (GOT data) and `printf` (PLT) are undefined until `dlopen`
resolves them against libextra.so and libpagh-c.so. Its two constructors and
its dependency's constructor set state that `plug_value()` checks, so 42 proves
the recursive runtime load and all three constructors ran, rather than a
constant baked into hello_dyn.

#### Reproducing the live test

```sh
./build.sh
PAGH_RISCV=../pagh-riscv-0.16.2 tools/live-dlopen.sh   # checkout at tag 0.16.2
PAGH_RISCV=../pagh-riscv        tools/live-dlopen.sh   # checkout at tag 0.18.0
```

The script creates `build/pagh-libc.img` with the built ELFs/shared objects,
boots the kernel from `$PAGH_RISCV` under QEMU, types the command through
`$PAGH_RISCV/tools/run.py`, and checks the transcript for the lines above. The
committed transcripts were captured with `qemu-system-riscv64` 10.2.2, xPack
GCC 15.2.0-1 and rust-lld from nightly-2026-07-12, from pagh-riscv tag
**0.16.2** (`1969d2c`) and tag **0.18.0** (`1c243e3`); the kernel path can be
overridden with `KERNEL=...`.

Earlier milestones' transcripts remain in `docs/live-dynamic*.txt` and
`docs/live-dlopen-0.9.0.txt`.
