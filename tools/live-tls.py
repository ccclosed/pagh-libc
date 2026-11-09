#!/usr/bin/env python3
"""Live HTTPS acceptance for the pagh C library's TLS and entropy support.

This is the evidence harness behind docs/live-tls.txt. It boots the RISC-V
kernel from a pagh-riscv checkout's own ``tools/run.py`` (the kernel tree is
never modified), with a ``--net`` user network and an ext2 image built by
``mke2fs -d`` from the examples this library builds. It drives the guest exactly
the way a human would, through the serial console, and checks:

1. **entropy** — ``tls_client`` draws 16 bytes from ``getrandom`` (278) twice and
   prints both; a pool that refused or repeated itself would stop the program
   before the socket is opened;
2. **the fetch** — ``tls_client`` and ``https_get`` complete a TLS 1.3 handshake
   against the real ``example.com``, verify the certificate chain to the
   committed CA bundle, check the SAN, and print the ``Example Domain`` body;
   the transcript carries the body byte count and the resolver's answer;
3. **fail-closed: a name that does not resolve** — ``https_get`` on an
   ``.invalid`` name reports ``errno=2`` (ENOENT) and returns no body, rather
   than connecting somewhere;
4. **fail-closed: a chain that does not anchor** — a local ``openssl s_server``
   with a self-signed certificate *for the requested name* answers on the host
   loopback (``10.0.2.2`` from the guest). Socket reachable, name right,
   certificate untrusted: the handshake still has to fail with ``errno=129``
   (EKEYREJECTED). This gate does not depend on the internet at all;
5. **fail-closed: a name the server will not serve for** — the guest is pinned
   to the real ``example.com`` address and asks for a name its certificate does
   not cover. The kernel refuses the handshake and the program never writes a
   request; which gate fires is the peer's choice (an abort for the unknown SNI
   is EPROTO, a certificate served anyway is caught by the SAN check with
   EKEYREJECTED), so the check requires a refusal of one of those two kinds and
   the transcript records which;
6. **fail-closed: no body for a non-200, no truncation for a small cap** — a 404
   is ``errno=5`` (EIO) with no body, and a 100-byte cap on the 577-byte body is
   ``errno=28`` (ENOSPC) rather than a partial body;
7. **the regressions** — sockets (``tcp_client`` against a host echo server,
   ``udp_echo``), DNS (``dns_lookup`` through the DHCP-leased server), ``%f``
   (``float_printf``), threads (``threads``) and ``dlopen`` (``ld-pagh`` +
   ``hello_dyn``) all still run from the same image.

Usage::

    ./build.sh
    python3 tools/live-tls.py                    # needs QEMU + host internet

The kernel checkout must already be built (``cargo build --release --locked``);
the default kernel is its release ELF. QEMU, mke2fs, debugfs and openssl are
required. The script writes the transcript to ``docs/live-tls.txt`` (override
with ``--transcript``); on failure it keeps the workdir and prints the failing
transcript.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
KERNEL_RELEASE = "target/riscv64gc-unknown-none-elf/release/pagh"
KERNEL_DEBUG = "target/riscv64gc-unknown-none-elf/debug/pagh"
PAGH_RISCV = pathlib.Path("/home/oleg/pagh-riscv")
TIMEOUT = 300.0
PROOF_HOST = "example.com"
PROOF_MARKER = "Example Domain"
# RFC 2606 reserves `.invalid`: the QEMU resolver (and any real one) answers
# NXDOMAIN, so the fetch has to fail at the resolve stage.
UNRESOLVABLE_HOST = "no-such-host.pagh.invalid"
# A name no certificate example.com serves can cover; with `--ip` pinning the
# real address this tests the SAN gate alone.
SAN_MISMATCH_HOST = "other.pagh.test"
HOST_FROM_GUEST = "10.0.2.2"  # QEMU's user network: the host's loopback

# The regression suite the same image proves: sockets, DNS, %f, threads, dlopen.
REGRESSION = [
    ("hello", "hello", "run /mnt/hello", ["hello from C on pagh-riscv"]),
    ("threads", "threads", "run /mnt/threads", ["counter=8 ret=50/30"]),
    ("float_printf", "float_printf", "run /mnt/float_printf",
     ["float: 355/113 = 3.141593", "float: ok ("]),
    ("dlopen", "ld-pagh", "run /mnt/ld-pagh /mnt/hello_dyn", ["dlopen: plug_value=42"]),
]


class Failure(RuntimeError):
    """A check failed; the caller prints the transcript and exits 1."""


def die(message: str, code: int = 2) -> None:
    print(f"live-tls: {message}", file=sys.stderr)
    raise SystemExit(code)


def check(condition: bool, what: str, transcript: str) -> None:
    if not condition:
        raise Failure(f"{what}\n--- serial transcript ---\n{transcript}")


def check_body_bytes(log: str, program: str, reported: int) -> None:
    """Require the bytes between a program's body markers to be `reported`.

    The transcript is the arbiter, not the program's summary line: the body
    region starts right after `<program>: ----- body ... -----\\n` and ends at
    the `\\n` that precedes `<program>: ----- end body -----`, which is exactly
    what the program wrote with `write(1, body, length)`.
    """
    start_marker = f"{program}: ----- body "
    start = log.find(start_marker)
    if start < 0:
        raise Failure(f"{program} printed no body marker\n--- serial transcript ---\n{log}")
    start = log.index("\n", start) + 1
    stop = log.find(f"\n{program}: ----- end body -----", start)
    if stop < 0:
        raise Failure(f"{program} printed no end marker\n--- serial transcript ---\n{log}")
    if stop - start != reported:
        raise Failure(
            f"{program} reported {reported} body bytes but the transcript holds "
            f"{stop - start}\n--- serial transcript ---\n{log}")


def provenance(pagh_riscv: pathlib.Path, kernel: pathlib.Path,
               ignore: pathlib.Path | None = None) -> dict[str, str]:
    """Identify the trees and tools a run should be reproducible from."""
    out = {"kernel_sha256": hashlib.sha256(kernel.read_bytes()).hexdigest()}

    def git(*args: str) -> str:
        result = subprocess.run(["git", "-C", str(pagh_riscv)] + list(args),
                                capture_output=True, text=True)
        return result.stdout.strip() if result.returncode == 0 else "unknown"

    out["kernel_rev"] = git("rev-parse", "--short", "HEAD")
    out["kernel_version"] = git("describe", "--tags", "--always")
    cargo = pagh_riscv / "Cargo.toml"
    if cargo.is_file():
        for line in cargo.read_text(encoding="utf-8").splitlines():
            if line.startswith("version ="):
                out["kernel_crate"] = line.split('"')[1]
                break
    result = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                            capture_output=True, text=True)
    out["libc_rev"] = result.stdout.strip() if result.returncode == 0 else "unknown"
    dirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain"],
                           capture_output=True, text=True)
    lines = dirty.stdout.splitlines()
    if ignore is not None:
        try:
            relative = str(ignore.resolve().relative_to(ROOT))
        except ValueError:
            relative = None
        if relative:
            lines = [line for line in lines if relative not in line]
    out["libc_dirty"] = "dirty" if lines else "clean"
    qemu = subprocess.run(["qemu-system-riscv64", "--version"], capture_output=True, text=True)
    out["qemu"] = qemu.stdout.splitlines()[0] if qemu.stdout else "unknown"
    openssl = subprocess.run(["openssl", "version"], capture_output=True, text=True)
    out["openssl"] = openssl.stdout.strip() if openssl.stdout else "unknown"
    return out


def free_host_port() -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]
    finally:
        sock.close()


# ── host peers ────────────────────────────────────────────────────────────────

class TcpEchoServer:
    """127.0.0.1 echo server the guest reaches as 10.0.2.2:<port>."""

    def __init__(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.received = b""
        self.error: str | None = None
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def _serve(self) -> None:
        try:
            self.sock.settimeout(120)
            conn, _peer = self.sock.accept()
            conn.settimeout(5)
            while True:
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    break
                if not chunk:
                    break
                self.received += chunk
                conn.sendall(chunk)
            conn.close()
        except OSError as error:
            self.error = str(error)
        finally:
            self.sock.close()

    def start(self) -> None:
        self.thread.start()

    def join(self, timeout: float = 30.0) -> None:
        self.thread.join(timeout)

    @property
    def alive(self) -> bool:
        return self.thread.is_alive()


class UdpEchoServer:
    """127.0.0.1 UDP echo server; records every datagram it saw."""

    def __init__(self, expected: int) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(120)
        self.port = self.sock.getsockname()[1]
        self.expected = expected
        self.received: list[tuple[bytes, tuple[str, int]]] = []
        self.error: str | None = None
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def _serve(self) -> None:
        try:
            for _ in range(self.expected):
                data, peer = self.sock.recvfrom(4096)
                self.received.append((data, peer))
                self.sock.sendto(data, peer)
        except OSError as error:
            self.error = str(error)
        finally:
            self.sock.close()

    def start(self) -> None:
        self.thread.start()

    def join(self, timeout: float = 30.0) -> None:
        self.thread.join(timeout)

    @property
    def alive(self) -> bool:
        return self.thread.is_alive()


class SelfSignedServer:
    """`openssl s_server` with a self-signed certificate *for the proof name*.

    This is negative control 5: the socket is reachable and the name matches the
    certificate, so only the chain check can refuse the handshake. The
    certificate is generated per run, so no committed key material is involved.
    """

    def __init__(self, work: pathlib.Path, name: str) -> None:
        self.work = work
        self.name = name
        self.port = free_host_port()
        self.cert = work / "selfsigned.pem"
        self.key = work / "selfsigned.key"
        self.process: subprocess.Popen | None = None

    def start(self) -> None:
        subprocess.run(
            [
                "openssl", "req", "-x509", "-newkey", "ec",
                "-pkeyopt", "ec_paramgen_curve:prime256v1",
                "-keyout", str(self.key), "-out", str(self.cert),
                "-days", "2", "-nodes",
                "-subj", f"/CN={self.name}",
                "-addext", f"subjectAltName=DNS:{self.name}",
            ],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        self.process = subprocess.Popen(
            [
                "openssl", "s_server",
                "-accept", f"127.0.0.1:{self.port}",
                "-cert", str(self.cert), "-key", str(self.key),
                "-tls1_3", "-www", "-quiet",
            ],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        # The listen socket is the readiness condition; a fixed sleep would be a
        # race on a loaded machine.
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.5):
                    return
            except OSError:
                time.sleep(0.1)
        die("openssl s_server did not start listening")

    def stop(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()


# ── guest plumbing ────────────────────────────────────────────────────────────

class Guest:
    """One booted QEMU instance driven through the kernel repo's tools/run.py."""

    def __init__(self, work: pathlib.Path, kernel: pathlib.Path, pagh_riscv: pathlib.Path):
        self.work = work
        self.kernel = kernel
        self.run_py = pagh_riscv / "tools" / "run.py"
        self.serial = work / "serial.sock"
        self.monitor = work / "monitor.sock"
        self.transcripts: list[str] = []

    def _invoke(self, args: list[str]) -> None:
        command = [sys.executable, str(self.run_py), "--kernel", str(self.kernel),
                   "--disk", str(self.work / "disk.img"),
                   "--serial", str(self.serial), "--monitor", str(self.monitor),
                   "--shot", str(self.work / "screen.png")] + args
        print("+", " ".join(command), flush=True)
        result = subprocess.run(command, cwd=str(self.work), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=TIMEOUT)
        if result.returncode != 0:
            raise Failure(f"tools/run.py failed ({result.returncode})\n{result.stdout}")

    def _read(self, log: pathlib.Path) -> str:
        # Read bytes and decode: text mode would translate the CRLFs a chunked
        # body contains into LFs, and the byte counts this harness checks are
        # taken from the transcript.
        text = log.read_bytes().decode("utf-8", errors="replace") if log.exists() else ""
        self.transcripts.append(text)
        return text

    def boot(self, *, net: bool, keep: bool, hostfwd: str = "", keys: str = "",
             settle: float = 3.0) -> str:
        log = self.work / f"boot-{len(self.transcripts)}.log"
        args = ["--serial-log", str(log), "--settle", str(settle), "--timeout", "180"]
        if net:
            args.append("--net")
        if hostfwd:
            args += ["--hostfwd", hostfwd]
        if keep:
            args.append("--keep")
        if keys:
            args += ["--keys", keys]
        self._invoke(args)
        return self._read(log)

    def attach(self, keys: str, settle: float = 4.0) -> str:
        log = self.work / f"attach-{len(self.transcripts)}.log"
        self._invoke(["--attach", "--serial-log", str(log), "--settle", str(settle),
                      "--timeout", "60", "--keys", keys])
        return self._read(log)

    def stop(self) -> None:
        found = subprocess.run(["pgrep", "-f", f"serial unix:{self.serial}"],
                               capture_output=True, text=True)
        for pid in found.stdout.split():
            subprocess.run(["kill", pid], capture_output=True)


def rewrite_resolv_conf(disk: pathlib.Path, server: str) -> None:
    """Replace /etc/resolv.conf in the image with the leased DNS server."""
    debugfs = shutil.which("debugfs")
    if debugfs is None:
        die("debugfs (e2fsprogs) is required to patch the image")
    tmp = disk.parent / "resolv.conf"
    tmp.write_text("# written by tools/live-tls.py from the guest's DHCP lease\n"
                   f"nameserver {server}\n", encoding="utf-8")
    for command in ("rm /etc/resolv.conf", f"write {tmp} /etc/resolv.conf",
                    "sif /etc/resolv.conf mode 0100644"):
        result = subprocess.run([debugfs, "-w", "-R", command, str(disk)],
                                capture_output=True, text=True)
        if result.returncode != 0:
            die(f"debugfs {command!r} failed: {result.stderr.strip()}")


def make_image(work: pathlib.Path) -> pathlib.Path:
    mke2fs = shutil.which("mke2fs")
    if mke2fs is None:
        die("mke2fs (e2fsprogs) is required to build the ext2 image")
    root = work / "root"
    (root / "etc").mkdir(parents=True)
    for elf in sorted((ROOT / "build").glob("*.elf")):
        target = root / elf.stem
        shutil.copyfile(elf, target)
        target.chmod(0o755)
    for so in sorted((ROOT / "build").glob("*.so")):
        shutil.copyfile(so, root / so.name)
    (root / "etc" / "resolv.conf").write_text("nameserver 10.0.2.3\n", encoding="utf-8")
    disk = work / "disk.img"
    with disk.open("wb") as handle:
        handle.truncate(64 * 1024 * 1024)
    command = [mke2fs, "-q", "-t", "ext2", "-b", "1024", "-F", "-d", str(root), str(disk)]
    print("+", " ".join(command), flush=True)
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        die(f"mke2fs failed: {result.stderr.strip()}")
    return disk


# ── the run ───────────────────────────────────────────────────────────────────

def run(args: argparse.Namespace) -> int:
    work = pathlib.Path(tempfile.mkdtemp(prefix="pagh-libc-tls-"))
    kernel = args.kernel
    if not kernel.is_absolute():
        kernel = args.pagh_riscv / kernel
    if not kernel.is_file():
        die(f"kernel {kernel} not found — build the kernel checkout first")
    for needed in ("tls_client.elf", "https_get.elf"):
        if not (ROOT / "build" / needed).is_file():
            die(f"build/{needed} missing — run ./build.sh first")
    if shutil.which("openssl") is None:
        die("openssl is required for the self-signed chain-gate negative")

    disk = make_image(work)
    guest = Guest(work, kernel, args.pagh_riscv)
    ids = provenance(args.pagh_riscv, kernel, ignore=args.transcript)
    print("live-tls: kernel " + ids["kernel_sha256"][:16] + " rev " + ids["kernel_rev"] +
          " crate " + ids.get("kernel_crate", "unknown"), flush=True)
    record: list[str] = []
    steps: list[str] = []
    counters: dict[str, str] = {}

    def banner(title: str) -> None:
        print(f"live-tls: == {title} ==", flush=True)

    try:
        # 1. Learn the DHCP lease (including the DNS server) from the guest.
        banner("boot 1: DHCP lease")
        lease_log = guest.boot(net=True, keep=False, keys="net\\n", settle=3.0)
        record.append("== boot 1: DHCP lease (net) ==\n" + lease_log)
        match = re.search(r"dns (\d+\.\d+\.\d+\.\d+)", lease_log)
        check(match is not None, "the guest printed no DHCP DNS server", lease_log)
        dns_server = match.group(1)
        check("net         DHCP ip 10.0.2.15/" in lease_log, "no DHCP lease", lease_log)
        guest.stop()
        rewrite_resolv_conf(disk, dns_server)
        print(f"live-tls: DHCP-leased DNS server {dns_server} -> /etc/resolv.conf", flush=True)

        # 2. Boot once more with the network, then drive it with --attach.
        banner("boot 2: --net")
        boot_log = guest.boot(net=True, keep=True, settle=2.0)
        record.append("== boot 2: --net ==\n" + boot_log)

        # ── entropy + the fetch, driven by hand through the session calls ─────
        banner("tls_client example.com")
        log = guest.attach(f"run /mnt/tls_client {PROOF_HOST}\\n", settle=30.0)
        steps.append("== tls_client: getrandom + handshake + GET example.com ==\n" + log)
        check("tls_client: getrandom 16 bytes:" in log, "getrandom did not answer", log)
        check("tls_client: resolve example.com -> " in log, "the guest did not resolve example.com", log)
        check("tls_client: tcp connected to " in log, "tls_client did not connect", log)
        check('tls_client: handshake ok: TLS 1.3 session on fd ' in log,
              "the kernel did not complete the handshake", log)
        check("tls_client: response \"HTTP/1.1 200" in log, "the response was not 200", log)
        check("tls_client: ----- body (" in log and PROOF_MARKER in log,
              "the body does not contain the proof marker", log)
        check("tls_client: ok" in log, "tls_client did not report success", log)
        check("run: /mnt/tls_client exited with code 0" in log, "tls_client exit status", log)
        # Count the printed body from the transcript itself and require it to
        # match the number the program reported.
        body = re.search(r"tls_client: ----- body \((\d+) bytes", log)
        check(body is not None, "tls_client printed no body line", log)
        check_body_bytes(log, "tls_client", int(body.group(1)))
        resolved = re.search(r"tls_client: resolve example\.com -> (\d+\.\d+\.\d+\.\d+)", log)
        if resolved:
            counters["example.com address"] = resolved.group(1)

        # ── the one-call convenience function ────────────────────────────────
        banner("https_get example.com")
        log = guest.attach(f"run /mnt/https_get {PROOF_HOST}\\n", settle=30.0)
        steps.append("== https_get: the convenience call, same name and path ==\n" + log)
        length = re.search(r"https_get: body (\d+) bytes", log)
        check(length is not None, "https_get returned no body length", log)
        check(PROOF_MARKER in log, "the body does not contain the proof marker", log)
        check("https_get: ok" in log, "https_get did not report success", log)
        check("run: /mnt/https_get exited with code 0" in log, "https_get exit status", log)
        check(int(length.group(1)) > 0, "https_get returned an empty body", log)
        # The number the call returned is the number of bytes the program then
        # printed: counted from the serial transcript, not trusted from the
        # program's own summary line.
        check_body_bytes(log, "https_get", int(length.group(1)))
        counters["https_get body"] = length.group(1) + " bytes"

        # ── negative 1: a name that does not resolve ─────────────────────────
        banner(f"negative: https_get {UNRESOLVABLE_HOST}")
        log = guest.attach(f"run /mnt/https_get {UNRESOLVABLE_HOST}\\n", settle=15.0)
        steps.append("== negative: a name that does not resolve ==\n" + log)
        check("https_get: FAILED errno=2 " in log,
              "an unresolvable name was not ENOENT", log)
        check("https_get: no body was returned" in log, "a body was claimed for a failed fetch", log)
        check(PROOF_MARKER not in log, "a body leaked from a failed fetch", log)
        check("run: /mnt/https_get exited with code 1" in log, "negative exit status", log)

        # ── negative 2: a reachable server with an untrusted chain ───────────
        banner("negative: self-signed server, correct name, untrusted chain")
        selfsigned = SelfSignedServer(work, PROOF_HOST)
        selfsigned.start()
        steps.append(f"== host: openssl s_server {PROOF_HOST} self-signed on "
                     f"127.0.0.1:{selfsigned.port} (guest reaches it as "
                     f"{HOST_FROM_GUEST}:{selfsigned.port}) ==\n")
        log = guest.attach(
            f"run /mnt/tls_client --ip {HOST_FROM_GUEST} --port {selfsigned.port} "
            f"{PROOF_HOST}\\n", settle=20.0)
        steps.append("== negative: reachable socket, matching name, self-signed chain ==\n" + log)
        check("tls_client: tcp connected to " in log, "the guest did not reach the local server", log)
        check("tls_client: FAILED stage=handshake errno=129 " in log,
              "a self-signed chain was not refused with EKEYREJECTED", log)
        check("tls_client: the kernel refused the server; no request was written" in log,
              "a request was written to a refused server", log)
        check("tls_client: handshake ok" not in log, "the untrusted chain was accepted", log)
        check("run: /mnt/tls_client exited with code 6" in log, "chain-gate exit status", log)
        selfsigned.stop()

        # ── negative 3: the real server, a name it will not serve for ────────
        if "example.com address" not in counters:
            raise Failure("no example.com address in the transcript: cannot run the "
                          "wrong-name negative")
        banner(f"negative: {SAN_MISMATCH_HOST} at the real example.com address")
        log = guest.attach(
            f"run /mnt/tls_client --ip {counters['example.com address']} "
            f"{SAN_MISMATCH_HOST}\\n", settle=25.0)
        steps.append("== negative: real server, a name it does not serve for ==\n" + log)
        check("tls_client: tcp connected to " in log, "the guest did not reach the real server", log)
        check("tls_client: FAILED stage=handshake " in log,
              "the mismatched name was not refused", log)
        check("tls_client: the kernel refused the server; no request was written" in log,
              "a request was written to a server that refused the name", log)
        check("tls_client: handshake ok" not in log, "a mismatched name was accepted", log)
        check("run: /mnt/tls_client exited with code 6" in log, "wrong-name exit status", log)
        # The refusal is the kernel's, and which gate fires depends on the peer:
        # a server that aborts the handshake for an unrecognized SNI (observed
        # live: example.com answers `HandshakeAborted(HandshakeFailure)`) is
        # EPROTO ; one that serves its certificate anyway is caught by the SAN
        # check with EKEYREJECTED. Both are refusals; neither lets a byte out.
        wrong_name = re.search(r"tls_client: FAILED stage=handshake errno=(\d+) ", log)
        check(wrong_name is not None and wrong_name.group(1) in ("71", "129"),
              "the wrong-name refusal was not EPROTO or EKEYREJECTED", log)
        counters["wrong-name refusal"] = f"errno={wrong_name.group(1)}"
        steps.append("== the peer's exact refusal ==\n" +
                     "\n".join(line for line in log.splitlines() if line.startswith("tls:")) + "\n")

        # ── negative 4: a 404 is not a body ──────────────────────────────────
        banner("negative: a non-200 response")
        log = guest.attach(f"run /mnt/https_get {PROOF_HOST} /no-such-page-pagh\\n", settle=25.0)
        steps.append("== negative: the server answered 404, so there is no body ==\n" + log)
        check("https_get: FAILED errno=5 " in log, "a 404 was not EIO", log)
        check("https_get: no body was returned" in log, "a body was claimed for a 404", log)
        check("run: /mnt/https_get exited with code 1" in log, "404 exit status", log)

        # ── negative 5: a body larger than the caller's buffer is refused ────
        banner("negative: a cap smaller than the body")
        log = guest.attach(f"run /mnt/https_get {PROOF_HOST} / 100\\n", settle=25.0)
        steps.append("== negative: a 100-byte cap for a 577-byte body ==\n" + log)
        check("https_get: FAILED errno=28 " in log, "a too-small cap was not ENOSPC", log)
        check("https_get: no body was returned" in log, "a truncated body was returned", log)
        check(PROOF_MARKER not in log, "a partial body leaked out of a refused fetch", log)
        check("run: /mnt/https_get exited with code 1" in log, "cap exit status", log)

        # ── regressions: sockets, DNS, %f, threads, dlopen, all in one image ──
        banner("regression: tcp_client (host echo)")
        echo = TcpEchoServer()
        echo.start()
        message = "pagh-tls-regression"
        log = guest.attach(f"run /mnt/tcp_client {HOST_FROM_GUEST} {echo.port} {message}\\n",
                           settle=8.0)
        echo.join(timeout=30)
        steps.append("== regression: tcp_client against a host echo server ==\n" + log)
        check("tcp_client: connected" in log, "tcp_client did not connect", log)
        check(f"tcp_client: echo {len(message)} bytes: {message}" in log,
              "tcp_client did not receive its echo", log)
        check("tcp_client: ok" in log, "tcp_client did not report success", log)
        check("run: /mnt/tcp_client exited with code 0" in log, "tcp_client exit status", log)
        check(not echo.alive and echo.error is None,
              f"host echo server error: {echo.error}", log)
        check(echo.received == message.encode(), f"host echo server received {echo.received!r}", log)

        banner("regression: udp_echo (host echo)")
        udp = UdpEchoServer(expected=3)
        udp.start()
        log = guest.attach(f"run /mnt/udp_echo {HOST_FROM_GUEST} {udp.port} 3 45678\\n", settle=10.0)
        udp.join(timeout=30)
        steps.append("== regression: udp_echo against a host echo server ==\n" + log)
        check("udp_echo: ok (3 datagrams echoed)" in log, "udp_echo success marker", log)
        check("run: /mnt/udp_echo exited with code 0" in log, "udp_echo exit status", log)
        check(not udp.alive and udp.error is None, f"host UDP server error: {udp.error}", log)
        check(len(udp.received) == 3, f"host UDP server saw {len(udp.received)} datagrams", log)

        banner("regression: dns_lookup example.com (DHCP server)")
        log = guest.attach(f"run /mnt/dns_lookup {PROOF_HOST}\\n", settle=12.0)
        steps.append("== regression: dns_lookup through the DHCP-leased server ==\n" + log)
        check(f"dns: server {dns_server}:53 (from /mnt/etc/resolv.conf)" in log,
              f"the resolver did not use the leased server {dns_server}", log)
        check("dns: gethostbyname example.com -> " in log, "the DHCP server did not resolve example.com", log)
        check("dns: ok" in log, "dns_lookup did not report success", log)
        check("run: /mnt/dns_lookup exited with code 0" in log, "dns_lookup exit status", log)

        for label, exe, command, markers in REGRESSION:
            banner(f"regression: {label}")
            log = guest.attach(command + "\\n", settle=6.0)
            steps.append(f"== regression: {command} ==\n" + log)
            check(f"run: /mnt/{exe} exited with code 0" in log, f"{label} exit status", log)
            for marker in markers:
                check(marker in log, f"{label}: missing {marker!r}", log)
    except Failure:
        guest.stop()
        print(f"live-tls: workdir kept for inspection: {work}", file=sys.stderr)
        raise
    finally:
        guest.stop()

    transcript = (
        "# Live TLS/entropy acceptance for pagh-libc\n"
        f"# kernel: {kernel}\n"
        f"# kernel checkout: {args.pagh_riscv}\n"
        f"# kernel revision: {ids['kernel_rev']} (crate {ids.get('kernel_crate', 'unknown')}, "
        f"tag {ids['kernel_version']})\n"
        f"# kernel sha256: {ids['kernel_sha256']}\n"
        f"# pagh-libc revision: {ids['libc_rev']} ({ids['libc_dirty']})\n"
        f"# host: {ids['qemu']}\n"
        f"# host: {ids['openssl']}\n"
        f"# image: mke2fs -t ext2 -b 1024 -d <build/*.elf + etc/resolv.conf> disk.img\n"
        f"# DHCP-leased DNS server: {dns_server}\n"
        f"# pinned address for the wrong-name negative: "
        f"{counters.get('example.com address', 'n/a')} "
        f"({counters.get('wrong-name refusal', 'not run')})\n"
        f"# self-signed chain-gate server: 127.0.0.1:{selfsigned.port} "
        f"(guest: {HOST_FROM_GUEST})\n"
        "\n" + "\n".join(record) + "\n" + "\n".join(steps) +
        "\nRESULT: PASS — getrandom, the live example.com fetch from both C entry "
        "points, five fail-closed negatives (resolve, chain, wrong name, non-200, "
        "cap) and the socket/DNS/%f/threads/dlopen regressions all green\n"
    )
    text = "\n".join(line.rstrip() for line in transcript.splitlines()) + "\n"
    path = args.transcript
    if not path.is_absolute():
        path = ROOT / path
    path.write_text(text, encoding="utf-8")
    print(f"live-tls: PASS — {path} ({len(text)} bytes)")
    if args.keep:
        print(f"live-tls: workdir {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Live TLS/entropy acceptance for pagh-libc")
    parser.add_argument("--pagh-riscv", type=pathlib.Path,
                        default=pathlib.Path(os.environ.get("PAGH_RISCV", str(PAGH_RISCV))),
                        help=f"pagh-riscv checkout (default {PAGH_RISCV})")
    parser.add_argument("--kernel", type=pathlib.Path, default=pathlib.Path(KERNEL_RELEASE),
                        help=f"kernel ELF, relative to --pagh-riscv (default {KERNEL_RELEASE})")
    parser.add_argument("--debug", action="store_true", help=f"use {KERNEL_DEBUG}")
    parser.add_argument("--transcript", type=pathlib.Path,
                        default=ROOT / "docs/live-tls.txt",
                        help="where to write the transcript (default docs/live-tls.txt)")
    parser.add_argument("--keep", action="store_true", help="keep the temporary workdir")
    args = parser.parse_args()
    if args.debug:
        args.kernel = pathlib.Path(KERNEL_DEBUG)
    try:
        return run(args)
    except Failure as failure:
        print(f"live-tls: FAIL — {failure}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
