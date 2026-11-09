#!/bin/sh
set -eu
cd "$(dirname "$0")"
CROSS="${CROSS:-/home/oleg/toolchains/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-}"
CC="${CROSS}gcc"
AR="${CROSS}ar"
CFLAGS="-march=rv64gc -mabi=lp64d -mcmodel=medany -ffreestanding -fno-stack-protector -fno-builtin -nostdlib -O2 -Wall -Wextra -Iinclude"
mkdir -p build
OBJS=""
for f in src/*.c; do
    o="build/$(basename "$f" .c).o"
    $CC $CFLAGS -c "$f" -o "$o"
    OBJS="$OBJS $o"
done
for f in src/*.S; do
    o="build/$(basename "$f" .S).o"
    $CC $CFLAGS -c "$f" -o "$o"
    OBJS="$OBJS $o"
done
$AR rcs build/libpagh-c.a $OBJS
# Shared libc for the experimental userland dynamic loader (crt0 is not in a .so).
PIC_OBJS=""
for f in src/*.c; do
    o="build/pic_$(basename "$f" .c).o"
    $CC $CFLAGS -fPIC -c "$f" -o "$o"
    PIC_OBJS="$PIC_OBJS $o"
done
$CC -march=rv64gc -mabi=lp64d -mcmodel=medany -ffreestanding -fno-stack-protector -fno-builtin -nostdlib -O2 -fPIC -Iinclude -c src/syscall.S -o build/pic_syscall.o
PIC_OBJS="$PIC_OBJS build/pic_syscall.o"
LLD="${LLD:-$(find /home/oleg/.rustup -name rust-lld -path '*nightly*' | head -1)}"
$LLD -flavor gnu -shared -soname libpagh-c.so -o build/libpagh-c.so $PIC_OBJS
# Second shared library for the multi-DT_NEEDED test.
$CC $CFLAGS -fPIC -c examples/libextra.c -o build/libextra.o
$LLD -flavor gnu -shared -soname libextra.so -o build/libextra.so build/libextra.o
# Dependency of the runtime plugin: nothing names it at link time except
# libplug's DT_NEEDED, so only dlopen's recursive dependency walk can load it.
$CC $CFLAGS -fPIC -c examples/libplugdep.c -o build/libplugdep.o
$LLD -flavor gnu -shared -soname libplugdep.so -o build/libplugdep.so build/libplugdep.o
# RTLD_GLOBAL plugin plus later objects: liblate must resolve against it, and
# liblatebad (local-only symbol) / libbadreloc (unknown relocation) are the
# negative controls.
$CC $CFLAGS -fPIC -c examples/libglobal.c -o build/libglobal.o
$LLD -flavor gnu -shared -soname libglobal.so -o build/libglobal.so build/libglobal.o
$CC $CFLAGS -fPIC -c examples/liblate.c -o build/liblate.o
$LLD -flavor gnu -shared -soname liblate.so -o build/liblate.so build/liblate.o
$CC $CFLAGS -fPIC -c examples/liblatebad.c -o build/liblatebad.o
$LLD -flavor gnu -shared -soname liblatebad.so -o build/liblatebad.so build/liblatebad.o
$CC $CFLAGS -fPIC -c examples/libbadreloc.c -o build/libbadreloc.o
$LLD -flavor gnu -shared -soname libbadreloc.so -o build/libbadreloc.so build/libbadreloc.o
python3 tools/patch_reloc.py build/libbadreloc.so 2 999 --symbol extra_global --expect 1
# Runtime plugin: DT_NEEDED names libplugdep.so (linked here, not copied), and
# extra_global/printf stay undefined, so dlopen must both follow the dependency
# and resolve the rest against objects already in the process.
$CC $CFLAGS -fPIC -c examples/libplug.c -o build/libplug.o
$LLD -flavor gnu -shared -soname libplug.so -init plug_dt_init -o build/libplug.so \
    build/libplug.o build/libplugdep.so
# rust-lld emits R_RISCV_64 for libplug's imported data GOT slots; re-tag
# extra_global's to the loader's legacy type-6 GLOB_DAT-compatible arm and
# dep_global's to R_RISCV_32, so the live dlopen exercises both (see README:
# RISC-V has no R_RISCV_GLOB_DAT, and PIC objects never emit R_RISCV_32).
python3 tools/patch_reloc.py build/libplug.so 2 6 --symbol extra_global --expect 1
python3 tools/patch_reloc.py build/libplug.so 2 1 --symbol dep_global --expect 1
# Dynamically linked test executable (no crt0; entry main, loader passes args).
# --export-dynamic puts the executable's own globals in .dynsym so the loader
# can resolve them (dlsym(RTLD_DEFAULT)/RTLD_NEXT interposition test).
$CC $CFLAGS -fPIC -c examples/hello_dyn.c -o build/hello_dyn.o
$LLD -flavor gnu -pie -e main --export-dynamic -o build/hello_dyn.elf build/hello_dyn.o \
    build/libextra.so build/libpagh-c.so
for ex in examples/*.c; do
    case "$ex" in
        examples/hello_dyn.c|examples/libextra.c|examples/libplug.c|examples/libplugdep.c|examples/libglobal.c|examples/liblate.c|examples/liblatebad.c|examples/libbadreloc.c) continue ;;
    esac
    name="$(basename "$ex" .c)"
    $CC $CFLAGS -T link.ld "$ex" build/libpagh-c.a -lgcc -o "build/$name.elf"
done
echo "pagh-libc $(cat VERSION): built:"
ls -l build/*.elf
