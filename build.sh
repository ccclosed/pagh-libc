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
for ex in examples/*.c; do
    name="$(basename "$ex" .c)"
    $CC $CFLAGS -T link.ld "$ex" build/libpagh-c.a -lgcc -o "build/$name.elf"
done
echo "built:"
ls -l build/*.elf
