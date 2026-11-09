#!/usr/bin/env python3
"""Re-tag test relocations so the loader's compatibility arms are exercised.

RISC-V has no R_RISCV_GLOB_DAT: the psABI uses R_RISCV_64 for data GOT slots,
and number 6 is R_RISCV_TLS_DTPMOD32.  rust-lld therefore never emits a
GLOB_DAT relocation.  The loader keeps a legacy type-6 arm for objects built
for the pre-0.9 loader (it is the number that loader called GLOB_DAT); this
script re-tags specific relocations so the live dlopen test runs through that
arm and through R_RISCV_32, which a PIC object otherwise never uses.

It is deliberately narrow: it walks SHT_RELA sections, re-tags entries of one
type (optionally only those naming one dynamic symbol), and fails unless
exactly `--expect` entries changed.

Usage: tools/patch_reloc.py OBJECT FROM_TYPE TO_TYPE [--expect N] [--symbol NAME]
"""
from __future__ import annotations

import struct
import sys

SHT_RELA = 4
SHT_DYNSYM = 11
# Elf64_Shdr field offsets.
SH_TYPE = 4
SH_OFFSET = 24
SH_SIZE = 32
SH_LINK = 40
SH_ENTSIZE = 56


def sections(data: bytearray) -> list[dict]:
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x3A)
    out = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        out.append({
            "type": struct.unpack_from("<I", data, off + SH_TYPE)[0],
            "offset": struct.unpack_from("<Q", data, off + SH_OFFSET)[0],
            "size": struct.unpack_from("<Q", data, off + SH_SIZE)[0],
            "link": struct.unpack_from("<I", data, off + SH_LINK)[0],
            "entsize": struct.unpack_from("<Q", data, off + SH_ENTSIZE)[0],
        })
    return out


def dynsym_names(data: bytearray, secs: list[dict]) -> list[str] | None:
    """Names of the .dynsym entries, indexed by symbol number."""
    for sh in secs:
        if sh["type"] != SHT_DYNSYM or sh["entsize"] < 24 or sh["link"] >= len(secs):
            continue
        strtab = secs[sh["link"]]
        names = []
        for j in range(sh["size"] // sh["entsize"]):
            name_off, = struct.unpack_from("<I", data, sh["offset"] + j * sh["entsize"])
            start = strtab["offset"] + name_off
            end = data.index(b"\0", start)
            names.append(data[start:end].decode("utf-8", "replace"))
        return names
    return None


def main() -> int:
    expect = 1
    symbol = None
    pos: list[str] = []
    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        if argv[i] == "--expect" and i + 1 < len(argv):
            expect = int(argv[i + 1], 0)
            i += 2
        elif argv[i] == "--symbol" and i + 1 < len(argv):
            symbol = argv[i + 1]
            i += 2
        else:
            pos.append(argv[i])
            i += 1
    if len(pos) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    path = pos[0]
    old = int(pos[1], 0)
    new = int(pos[2], 0)

    data = bytearray(open(path, "rb").read())
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        print(f"{path}: not a little-endian ELF64 file", file=sys.stderr)
        return 1

    secs = sections(data)
    names = dynsym_names(data, secs)
    if symbol is not None and names is None:
        print(f"{path}: no .dynsym, cannot select by symbol", file=sys.stderr)
        return 1

    changed = 0
    for sh in secs:
        if sh["type"] != SHT_RELA or sh["entsize"] != 24:
            continue
        for j in range(sh["size"] // sh["entsize"]):
            rel = sh["offset"] + j * sh["entsize"]
            info, = struct.unpack_from("<Q", data, rel + 8)
            if (info & 0xFFFFFFFF) != old:
                continue
            if symbol is not None:
                si = info >> 32
                if si >= len(names) or names[si] != symbol:
                    continue
            struct.pack_into("<Q", data, rel + 8, (info & ~0xFFFFFFFF) | new)
            changed += 1

    if changed != expect:
        what = f" for symbol {symbol}" if symbol else ""
        print(f"{path}: changed {changed} relocations{what}, expected {expect}", file=sys.stderr)
        return 1
    open(path, "wb").write(data)
    what = f" for {symbol}" if symbol else ""
    print(f"{path}: re-tagged {changed} R_RISCV_{old} relocation(s){what} as R_RISCV_{new}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
