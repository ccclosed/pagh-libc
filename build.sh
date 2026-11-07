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
# Dynamically linked test executable (no crt0; entry main, loader passes args).
$CC $CFLAGS -fPIC -c examples/hello_dyn.c -o build/hello_dyn.o
$LLD -flavor gnu -pie -e main -o build/hello_dyn.elf build/hello_dyn.o build/libpagh-c.so
for ex in examples/*.c; do
    case "$ex" in
        examples/hello_dyn.c) continue ;;  # built as a shared-lib PIE below
    esac
    name="$(basename "$ex" .c)"
    $CC $CFLAGS -T link.ld "$ex" build/libpagh-c.a -lgcc -o "build/$name.elf"
done
echo "built:"
ls -l build/*.elf
