#!/bin/sh
# Live dlopen test: ld-pagh loads hello_dyn + libextra + libpagh-c, then
# hello_dyn drives the runtime loader:
#
#   dlopen("/mnt/libplug.so")   follows DT_NEEDED -> libplugdep.so (never seen)
#   dlsym(handle)/dlsym(RTLD_DEFAULT)/dlsym(RTLD_NEXT)
#   RTLD_GLOBAL (libglobal) vs RTLD_LOCAL (libplug's group)
#   a later plugin resolving against the RTLD_GLOBAL scope, and negative
#   controls that must fail with "unresolved symbol dep_answer" and
#   "unsupported relocation type 999"
#
#   PAGH_RISCV=../pagh-riscv tools/live-dlopen.sh
#
# The kernel checkout must already be built (cargo build); the default kernel
# is its debug pagh ELF.  The disk is a 64 MiB ext2 image containing the built
# examples, shared objects and plugins.  The guest is driven through that
# checkout's tools/run.py, and the serial transcript is checked line by line.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)

PAGH_RISCV=${PAGH_RISCV:-$ROOT/../pagh-riscv}
KERNEL=${KERNEL:-$PAGH_RISCV/target/riscv64gc-unknown-none-elf/debug/pagh}
DISK=${DISK:-$ROOT/build/pagh-libc.img}
LOG=${LOG:-$ROOT/build/live-dlopen-serial.log}
SHOT=${SHOT:-$ROOT/build/live-dlopen.png}
SOCK=${SOCK:-${TMPDIR:-/tmp}/pagh-libc-serial-$$.sock}
MONSOCK=${MONSOCK:-${TMPDIR:-/tmp}/pagh-libc-mon-$$.sock}

[ -f "$PAGH_RISCV/tools/run.py" ] || {
    echo "live-dlopen: PAGH_RISCV=$PAGH_RISCV is not a pagh-riscv checkout" >&2
    exit 2
}
[ -f "$KERNEL" ] || {
    echo "live-dlopen: kernel $KERNEL not found; run 'cargo build' in $PAGH_RISCV" >&2
    exit 2
}
command -v mke2fs >/dev/null 2>&1 || {
    echo "live-dlopen: mke2fs is required to make the ext2 disk" >&2
    exit 2
}

TMPDIR_CLEANUP=$(mktemp -d)
trap 'rm -rf "$TMPDIR_CLEANUP"; rm -f "$SOCK" "$MONSOCK"' EXIT INT TERM

if ! ./build.sh >"$TMPDIR_CLEANUP/build.log" 2>&1; then
    cat "$TMPDIR_CLEANUP/build.log" >&2
    exit 1
fi

ROOTDIR=$TMPDIR_CLEANUP/diskroot
mkdir -p "$ROOTDIR"
for f in build/*.elf; do
    cp "$f" "$ROOTDIR/$(basename "$f" .elf)"
done
for f in build/*.so; do
    cp "$f" "$ROOTDIR/"
done
printf 'hello libc\n' > "$ROOTDIR/hello.txt"
rm -f "$DISK"
mke2fs -q -t ext2 -b 4096 -d "$ROOTDIR" "$DISK" 64M

version=$(sed -n 's/^version = "\(.*\)"/\1/p' "$PAGH_RISCV/Cargo.toml" | head -1)
echo "live-dlopen: kernel pagh-riscv $version ($KERNEL)"
case "$version" in
    0.16.2|0.18.0) ;;
    *) echo "live-dlopen: warning: validated against 0.16.2/0.18.0, testing $version" >&2 ;;
esac

python3 "$PAGH_RISCV/tools/run.py" \
    --kernel "$KERNEL" --disk "$DISK" \
    --keys 'run /mnt/ld-pagh /mnt/hello_dyn\n' --settle 6 \
    --serial "$SOCK" --monitor "$MONSOCK" \
    --serial-log "$LOG" --shot "$SHOT" --timeout 60

fail=0
for expected in \
    'hello from a dynamically linked C program' \
    'argc=1 argv0=/mnt/hello_dyn extra=42' \
    'libplugdep: DT_INIT_ARRAY dep_global=40' \
    'libplug: DT_INIT' \
    'libplug: DT_INIT_ARRAY extra_global=40' \
    'dlopen: plug_value=42' \
    'dlopen: second dlopen reuses the handle, init count still 2' \
    'dlopen: dlsym(handle) dep_answer=42' \
    'dlopen: RTLD_LOCAL group hidden from RTLD_DEFAULT' \
    'libglobal: DT_INIT_ARRAY' \
    'dlopen: RTLD_GLOBAL global_answer=77' \
    'liblate: DT_INIT_ARRAY global_answer=77' \
    'dlopen: later plugin resolved the RTLD_GLOBAL symbol' \
    'dlopen: RTLD_LOCAL-only symbol rejected: unresolved symbol dep_answer' \
    'dlopen: unsupported relocation rejected: unsupported relocation type 999' \
    'dlopen: RTLD_DEFAULT interpose_value=1, RTLD_NEXT interpose_value=2' \
    'dlopen: RTLD_NEXT skips RTLD_LOCAL-only symbols' \
    'dlopen: RTLD_GLOBAL promotes an already-loaded group' \
    'run: /mnt/ld-pagh exited with code 0'
do
    if ! grep -Fq "$expected" "$LOG"; then
        echo "live-dlopen: MISSING: $expected" >&2
        fail=1
    fi
done
if [ "$fail" -ne 0 ]; then
    echo "live-dlopen: transcript is $LOG" >&2
    exit 1
fi
echo "live-dlopen: PASS (transcript $LOG)"
