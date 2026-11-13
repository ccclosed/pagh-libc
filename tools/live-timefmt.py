#!/usr/bin/env python3
"""Live acceptance for the pagh-libc time surface (clock_gettime/time/
gettimeofday, gmtime_r/localtime_r, strftime).

The harness boots the pagh-riscv kernel from a checkout's own ``tools/run.py``
(the kernel repository is never modified) with an ext2 image built by
``mke2fs -d`` that contains ``build/timefmt.elf`` as ``/timefmt`` (the guest
sees ``/mnt/timefmt``). The guest program checks the time helpers against each
other and against values it computes itself — including the fixed-offset TZ
forms — and the serial transcript must contain every marker plus the shell's
exit line. The transcript is copied to ``docs/live-timefmt.txt``.

Usage::

    ./build.sh
    python3 tools/live-timefmt.py

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
    "timefmt: clock_gettime realtime ",
    "timefmt: time() ",
    "timefmt: note: the ABI clock is kernel uptime, not the RTC wall clock",
    "timefmt: gmtime_r ",
    "timefmt: localtime_r == gmtime_r (TZ unset is UTC)",
    "timefmt: gmtime()/localtime() wrappers agree",
    "timefmt: strftime %Y-%m-%d %H:%M:%S = ",
    "timefmt: strftime C-locale names + %e = ",
    "timefmt: strftime %F %T %j %u %w %I %p = ",
    "timefmt: truncation returns 0 (4-byte buffer, %Y)",
    "timefmt: TZ=MSK-3 offset +3 h verified",
    "timefmt: TZ=EST5 offset -5 h verified",
    "timefmt: TZ unset restored UTC",
    "timefmt: ok (epoch ",
    "run: /mnt/timefmt exited with code 0",
)


def die(message: str, code: int = 2) -> None:
    print(f"live-timefmt: {message}", file=sys.stderr)
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
        default=str(ROOT / "docs" / "live-timefmt.txt"),
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

    work = pathlib.Path(tempfile.mkdtemp(prefix="pagh-live-timefmt-"))
    try:
        print("live-timefmt: building the examples", file=sys.stderr)
        build = subprocess.run(["./build.sh"], cwd=ROOT, capture_output=True, text=True)
        if build.returncode != 0:
            print(build.stdout, file=sys.stderr)
            print(build.stderr, file=sys.stderr)
            die("build.sh failed")

        diskroot = work / "root"
        diskroot.mkdir()
        example = ROOT / "build" / "timefmt.elf"
        if not example.is_file():
            die(f"{example} was not built by build.sh")
        shutil.copy(example, diskroot / "timefmt")

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
            "run /mnt/timefmt\\n",
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
        print("live-timefmt: booting the kernel", file=sys.stderr)
        run = subprocess.run(command, capture_output=True, text=True)
        if run.returncode != 0:
            print(run.stdout, file=sys.stderr)
            print(run.stderr, file=sys.stderr)

        transcript = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
        header = "\n".join(
            [
                "# Live time acceptance for pagh-libc (clock_gettime/time/gettimeofday,",
                "# gmtime_r/localtime_r, strftime)",
                f"# kernel: {kernel}",
                f"# kernel checkout: {kernel_repo} (revision {git_revision(kernel_repo)}, crate {crate_version(kernel_repo)})",
                f"# kernel sha256: {sha256(kernel)}",
                f"# pagh-libc revision: {git_revision(ROOT)}",
                f"# host: {qemu_version()}",
                "# image: mke2fs -t ext2 -b 1024 -d <root with build/timefmt.elf as timefmt> disk.img",
                "# guest command: run /mnt/timefmt",
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
                print(f"live-timefmt: MISSING: {marker}", file=sys.stderr)
            print(f"live-timefmt: transcript is {args.transcript}", file=sys.stderr)
            return 1
        print(f"live-timefmt: PASS (transcript {args.transcript})", file=sys.stderr)
        return 0
    finally:
        if args.keep_workdir:
            print(f"live-timefmt: workdir kept at {work}", file=sys.stderr)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())