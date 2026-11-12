#!/usr/bin/env python3
"""Check that every errno this library defines has a strerror() text.

`include/errno.h` is the set of errors a program can see: the kernel's ABI
errors (pagh-riscv `docs/abi.md` §3) plus the socket errors the same ABI adds.
`strerror()` in `src/strerror.c` is the only place a program can turn one of
those numbers into a message, so an `#define` without a `case` silently becomes
"unknown error" — which is what happened to `ESRCH`, `E2BIG`, `ENOEXEC` and
`ECHILD` before this check existed (the kernel returns them from `kill`,
`execv` and `waitpid`).

This parses both files and fails when either side has an entry the other lacks,
or when two names share a number. No compiler or cross toolchain needed.

    python3 tools/check_errno.py

Exit code 0 on agreement, 1 on drift, 2 when a file/pattern is missing.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "include" / "errno.h"
STRERROR = ROOT / "src" / "strerror.c"


def read(path: pathlib.Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise SystemExit(f"check_errno: cannot read {path}: {error}")


def main() -> int:
    header = read(HEADER)
    source = read(STRERROR)

    defined = {
        name: int(number)
        for name, number in re.findall(
            r"#define\s+(E[A-Z0-9_]+)\s+(\d+)", header
        )
    }
    if not defined:
        print("check_errno: no `#define E... <n>` in include/errno.h", file=sys.stderr)
        return 2

    switch = re.search(r"switch\s*\(err\)\s*\{(?P<body>.*?)\n\}", source, re.S)
    if not switch:
        print("check_errno: `switch (err)` not found in src/strerror.c", file=sys.stderr)
        return 2
    cases = set(re.findall(r"case\s+(E[A-Z0-9_]+)\s*:", switch.group("body")))
    if not cases:
        print("check_errno: no `case E...:` in src/strerror.c", file=sys.stderr)
        return 2

    errors: list[str] = []
    for name in sorted(defined):
        if name not in cases:
            errors.append(
                f"include/errno.h defines {name}={defined[name]} but "
                f"src/strerror.c has no case for it"
            )
    for name in sorted(cases - set(defined)):
        errors.append(
            f"src/strerror.c handles {name} but include/errno.h does not define it"
        )

    by_number: dict[int, list[str]] = {}
    for name, number in defined.items():
        by_number.setdefault(number, []).append(name)
    for number, names in sorted(by_number.items()):
        if len(names) > 1:
            errors.append(f"errno {number} is defined twice: {', '.join(sorted(names))}")

    if errors:
        for error in errors:
            print(f"check_errno: {error}", file=sys.stderr)
        return 1

    print(
        f"errno check OK: {len(defined)} values in include/errno.h, "
        "every one has a strerror() text"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())