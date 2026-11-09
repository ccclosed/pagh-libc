#!/usr/bin/env python3
"""Re-tag one test relocation so the legacy GLOB_DAT-compatible arm is used.

RISC-V has no R_RISCV_GLOB_DAT: the psABI uses R_RISCV_64 for data GOT slots,
and number 6 is R_RISCV_TLS_DTPMOD32.  rust-lld therefore never emits a
GLOB_DAT relocation.  The loader keeps a legacy type-6 arm for objects built
for the pre-0.9 loader (it is the number that loader called GLOB_DAT); this
script re-tags libplug's single real data relocation from 64 to 6 so dlopen
exercises that arm.  It is deliberately narrow: it walks SHT_RELA sections,
re-tags entries of one type, and fails unless exactly `--expect` changed.

Usage: tools/patch_reloc.py OBJECT FROM_TYPE TO_TYPE [--expect N]
"""
import struct
import sys

SHT_RELA = 4


def main() -> int:
    if len(sys.argv) not in (4, 6):
        print(__doc__, file=sys.stderr)
        return 2
    path = sys.argv[1]
    old = int(sys.argv[2], 0)
    new = int(sys.argv[3], 0)
    expect = int(sys.argv[5]) if len(sys.argv) == 6 and sys.argv[4] == "--expect" else 1
    data = bytearray(open(path, "rb").read())
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        print(f"{path}: not a little-endian ELF64 file", file=sys.stderr)
        return 1
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x3A)
    changed = 0
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type, = struct.unpack_from("<I", data, off + 4)
        if sh_type != SHT_RELA:
            continue
        sh_offset, sh_size = struct.unpack_from("<QQ", data, off + 24)
        sh_entsize, = struct.unpack_from("<Q", data, off + 56)
        if sh_entsize != 24:
            continue
        for j in range(sh_size // sh_entsize):
            rel = sh_offset + j * sh_entsize
            info, = struct.unpack_from("<Q", data, rel + 8)
            rtype = info & 0xFFFFFFFF
            if rtype == old:
                struct.pack_into("<Q", data, rel + 8, (info & ~0xFFFFFFFF) | new)
                changed += 1
    if changed != expect:
        print(f"{path}: changed {changed} relocations, expected {expect}", file=sys.stderr)
        return 1
    open(path, "wb").write(data)
    print(f"{path}: re-tagged {changed} R_RISCV_{old} relocation(s) as R_RISCV_{new}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
