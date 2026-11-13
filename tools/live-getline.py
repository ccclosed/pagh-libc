#!/usr/bin/env python3
"""Live acceptance for getline(3)/getdelim(3) on the pagh-riscv kernel.

This is the evidence harness behind `docs/live-getline.txt`. It boots the
kernel from a pagh-riscv checkout's own ``tools/run.py`` (the kernel repository
is never modified), with an ext2 image built by ``mke2fs -d`` that contains
``build/getline.elf`` as ``/getline`` (the guest sees ``/mnt/getline``). The
guest program writes a text file on the real filesystem, reads it back with
``getline`` (including an empty line, a 300-byte line that forces the buffer
past its initial 120 bytes, and a final line without a newline), then checks
``getdelim`` with ';' as the delimiter. The serial transcript must contain
every marker and the shell's exit line, and the script copies it to
``docs/live-getline.txt``.

Usage::

    ./build.sh
    python3 tools/live-getline.py

``PAGH_RISCV`` selects the kernel checkout (default ``../pagh-riscv``); the
release ELF must already be built there. QEMU and mke2fs are required.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
KERNEL_RELEASE = "target/riscv64gc-unknown-none-elf/release/pagh"
KERNEL_DEBUG = "target/riscv64gc-unknown-none-elf/debug/pagh"
TIMEOUT = 180.0

EXPECTED = (
    "getline: wrote /mnt/getline.txt (339 bytes)",
    "getline: line 1: 6 bytes",
    "getline: line 2: 11 bytes",
    "getline: line 3: 1 bytes",
    "getline: empty line was stored as \"\\n\"",
    "getline: line 4: 301 bytes",
    "getline: 300-byte line grew the buffer to",
    "getline: line 5: 20 bytes",
    "getline: eof after 5 lines (feof set)",
    "getdelim: token 1: 4 bytes [one;]",
    "getdelim: token 2: 4 bytes [two;]",
    "getdelim: token 3: 5 bytes [three]",
    "getline: ok (5 getline lines, 3 getdelim tokens)",
    "run: /mnt/getline exited with code 0",
)


def die(message: str, code: int = 2) -> None:
    print(f"live-getline: {message}", file=sys.stderr)
    raise SystemExit(code)


def git_revision(repo: pathlib.Path) -> str:
    try:
        head = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "--short", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        dirty = subprocess.run(
            ["git", "-C", str(repo), "status", "--porcelain"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        return head + (" (dirty)" if dirty else " (clean)")
    except (OSError, subprocess.CalledProcessError) as error:
        return f"unknown ({error})"


def crate_version(repo: pathlib.Path) -> str:
    try:
        text = (repo / "Cargo.toml").read_text(encoding="utf-8")
    except OSError:
        return "unknown"
    for line in text.splitlines():
        if line.startswith("version"):
            return line.split("=", 1)[1].strip().strip('"')
    return "unknown"


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def qemu_version() -> str:
    try:
        return subprocess.run(
            ["qemu-system-riscv64", "--version"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.splitlines()[0]
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--transcript",
        default=str(ROOT / "docs" / "live-getline.txt"),
        help="where to write the committed transcript",
    )
    parser.add_argument(
        "--pagh-riscv",
        default=os.environ.get("PAGH_RISCV", str(ROOT.parent / "pagh-riscv")),
        help="pagh-riscv checkout (default ../pagh-riscv or $PAGH_RISCV)",
    )
    parser.add_argument("--keep-workdir", action="store_true")
    args = parser.parse_args()

    kernel_repo = pathlib.Path(args.pagh_riscv).resolve()
    kernel = kernel_repo / KERNEL_RELEASE
    if not kernel.is_file():
        kernel = kernel_repo / KERNEL_DEBUG
    if not kernel.is_file():
        die(f"no built kernel under {kernel_repo} (run `cargo build --release` there)")
    run_py = kernel_repo / "tools" / "run.py"
    if not run_py.is_file():
        die(f"{kernel_repo} is not a pagh-riscv checkout (tools/run.py missing)")
    if shutil.which("qemu-system-riscv64") is None:
        die("qemu-system-riscv64 is required")
    if shutil.which("mke2fs") is None:
        die("mke2fs (e2fsprogs) is required")

    work = pathlib.Path(tempfile.mkdtemp(prefix="pagh-live-getline-"))
    try:
        print("live-getline: building the examples", file=sys.stderr)
        build = subprocess.run(
            ["./build.sh"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if build.returncode != 0:
            print(build.stdout, file=sys.stderr)
            print(build.stderr, file=sys.stderr)
            die("build.sh failed")

        diskroot = work / "root"
        diskroot.mkdir()
        example = ROOT / "build" / "getline.elf"
        if not example.is_file():
            die(f"{example} was not built by build.sh")
        shutil.copy(example, diskroot / "getline")

        disk = work / "disk.img"
        make = subprocess.run(
            [
                "mke2fs",
                "-q",
                "-t",
                "ext2",
                "-b",
                "1024",
                "-F",
                "-d",
                str(diskroot),
                str(disk),
                "16M",
            ],
            capture_output=True,
            text=True,
        )
        if make.returncode != 0:
            die(f"mke2fs failed: {make.stderr.strip()}")

        serial = work / "serial.sock"
        monitor = work / "monitor.sock"
        shot = work / "shot.png"
        log = work / "serial.log"
        command = [
            sys.executable,
            str(run_py),
            "--kernel",
            str(kernel),
            "--disk",
            str(disk),
            "--keys",
            "run /mnt/getline\\n",
            "--settle",
            "8",
            "--serial",
            str(serial),
            "--monitor",
            str(monitor),
            "--serial-log",
            str(log),
            "--shot",
            str(shot),
            "--timeout",
            str(TIMEOUT),
        ]
        print("live-getline: booting the kernel", file=sys.stderr)
        run = subprocess.run(command, capture_output=True, text=True)
        if run.returncode != 0:
            print(run.stdout, file=sys.stderr)
            print(run.stderr, file=sys.stderr)

        transcript = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
        header = "\n".join(
            [
                "# Live getline/getdelim acceptance for pagh-libc",
                f"# kernel: {kernel}",
                f"# kernel checkout: {kernel_repo} (revision {git_revision(kernel_repo)}, crate {crate_version(kernel_repo)})",
                f"# kernel sha256: {sha256(kernel)}",
                f"# pagh-libc revision: {git_revision(ROOT)}",
                f"# host: {qemu_version()}",
                f"# image: mke2fs -t ext2 -b 1024 -d <root with build/getline.elf as getline> disk.img",
                "# guest command: run /mnt/getline",
                "",
            ]
        )
        output = header + transcript
        if not transcript.endswith("\n"):
            output += "\n"
        pathlib.Path(args.transcript).write_text(output, encoding="utf-8")

        missing = [marker for marker in EXPECTED if marker not in transcript]
        if run.returncode != 0 or missing:
            for marker in missing:
                print(f"live-getline: MISSING: {marker}", file=sys.stderr)
            print(f"live-getline: transcript is {args.transcript}", file=sys.stderr)
            return 1
        print(f"live-getline: PASS (transcript {args.transcript})", file=sys.stderr)
        return 0
    finally:
        if args.keep_workdir:
            print(f"live-getline: workdir kept at {work}", file=sys.stderr)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
