#!/usr/bin/env python3
"""Live acceptance for the pagh-libc formatted-input surface (sscanf/
vsscanf/fscanf and the sprintf/vsprintf wrappers).

The harness boots the pagh-riscv kernel from a checkout's own ``tools/run.py``
(the kernel repository is never modified) with an ext2 image built by
``mke2fs -d`` that contains ``build/scan.elf`` as ``/scan`` (the guest sees
``/mnt/scan``). The guest program writes a small record file on the ext2
image and parses it with fgets+sscanf, again with fscanf, checks a sprintf
round-trip and the EOF/matching-failure/%n/suppression/scanset semantics; the
serial transcript must contain every marker plus the shell's exit line. The
transcript is copied to ``docs/live-scan.txt``.

Usage::

    ./build.sh
    python3 tools/live-scan.py

``PAGH_RISCV`` selects the kernel checkout (default ``../pagh-riscv``); its
release ELF must already be built. QEMU and mke2fs are required.
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
    "scan: wrote /mnt/scan-input.txt (4 lines, 3 records)",
    "scan: record 1: 42 -7 3.5 alpha",
    "scan: record 2: 100 200 1.25 beta",
    "scan: record 3: -5 8 0.5 gamma",
    "scan: line 4 is not a record (sscanf=0)",
    "scan: fgets+sscanf ok (3 records, sums 137 201 5.25)",
    "scan: sprintf ok: 137/201/5.25/gamma",
    "scan: fscanf record 1: 42 -7 3.5 alpha",
    "scan: fscanf stopped on the non-record line (return 0)",
    "scan: fscanf ok (3 records, FILE path)",
    "scan: edge semantics ok",
    "scan: ok (fgets+sscanf, sprintf, fscanf, edge semantics)",
    "run: /mnt/scan exited with code 0",
)


def die(message: str, code: int = 2) -> None:
    print(f"live-scan: {message}", file=sys.stderr)
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
        default=str(ROOT / "docs" / "live-scan.txt"),
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

    work = pathlib.Path(tempfile.mkdtemp(prefix="pagh-live-scan-"))
    try:
        print("live-scan: building the examples", file=sys.stderr)
        build = subprocess.run(["./build.sh"], cwd=ROOT, capture_output=True, text=True)
        if build.returncode != 0:
            print(build.stdout, file=sys.stderr)
            print(build.stderr, file=sys.stderr)
            die("build.sh failed")

        diskroot = work / "root"
        diskroot.mkdir()
        example = ROOT / "build" / "scan.elf"
        if not example.is_file():
            die(f"{example} was not built by build.sh")
        shutil.copy(example, diskroot / "scan")

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
            "run /mnt/scan\\n",
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
        print("live-scan: booting the kernel", file=sys.stderr)
        run = subprocess.run(command, capture_output=True, text=True)
        if run.returncode != 0:
            print(run.stdout, file=sys.stderr)
            print(run.stderr, file=sys.stderr)

        transcript = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
        header = "\n".join(
            [
                "# Live formatted-input acceptance for pagh-libc (sscanf/vsscanf/fscanf, sprintf)",
                "# and the C/POSIX scan conversion set)",
                f"# kernel: {kernel}",
                f"# kernel checkout: {kernel_repo} (revision {git_revision(kernel_repo)}, crate {crate_version(kernel_repo)})",
                f"# kernel sha256: {sha256(kernel)}",
                f"# pagh-libc revision: {git_revision(ROOT)}",
                f"# host: {qemu_version()}",
                "# image: mke2fs -t ext2 -b 1024 -d <root with build/scan.elf as scan> disk.img",
                "# guest command: run /mnt/scan",
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
                print(f"live-scan: MISSING: {marker}", file=sys.stderr)
            print(f"live-scan: transcript is {args.transcript}", file=sys.stderr)
            return 1
        print(f"live-scan: PASS (transcript {args.transcript})", file=sys.stderr)
        return 0
    finally:
        if args.keep_workdir:
            print(f"live-scan: workdir kept at {work}", file=sys.stderr)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())