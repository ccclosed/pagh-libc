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
# Runtime plugin: no DT_NEEDED on purpose, so dlopen must resolve its undefined
# extra_global/printf against the objects already in the process.
$CC $CFLAGS -fPIC -c examples/libplug.c -o build/libplug.o
$LLD -flavor gnu -shared -soname libplug.so -init plug_dt_init -o build/libplug.so build/libplug.o
# rust-lld emits R_RISCV_64 for libplug's imported data GOT slot; re-tag that
# one test relocation to the loader's legacy type-6 GLOB_DAT-compatible arm so
# the live dlopen exercises it too (see README: RISC-V has no R_RISCV_GLOB_DAT).
python3 tools/patch_reloc.py build/libplug.so 2 6 --expect 1
# Dynamically linked test executable (no crt0; entry main, loader passes args).
$CC $CFLAGS -fPIC -c examples/hello_dyn.c -o build/hello_dyn.o
$LLD -flavor gnu -pie -e main -o build/hello_dyn.elf build/hello_dyn.o \
    build/libextra.so build/libpagh-c.so
for ex in examples/*.c; do
    case "$ex" in
        examples/hello_dyn.c|examples/libextra.c|examples/libplug.c) continue ;;
    esac
    name="$(basename "$ex" .c)"
    $CC $CFLAGS -T link.ld "$ex" build/libpagh-c.a -lgcc -o "build/$name.elf"
done
echo "pagh-libc $(cat VERSION): built:"
ls -l build/*.elf
