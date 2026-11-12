#!/usr/bin/env python3
"""Live acceptance for the pagh C library's sockets, DNS and %f.

This is the evidence harness behind docs/live-sockets.txt. It boots the kernel
from a pagh-riscv checkout's own ``tools/run.py`` (the kernel repo is never
modified), with an ext2 image built by ``mke2fs -d`` and four host peers:

1. ``net`` in the guest prints the DHCP lease; the script reads the leased DNS
   server out of the transcript and writes it into the image's
   ``/etc/resolv.conf``, which is how a DHCP-provided resolver reaches
   userland in this ABI (nothing in the syscall list exposes the lease);
2. a guest TCP server (``tcp_server``) is reached by a host client through
   QEMU's ``hostfwd``, and a guest TCP client (``tcp_client``) reaches a host
   echo server at 10.0.2.2;
3. a host UDP echo server (``udp_echo``) and a hand-rolled authoritative DNS
   server (``dns_lookup``) answer the guest; the DNS server first sends a decoy
   answer with the wrong transaction id, so a client that ignored the id would
   answer 198.51.100.9 instead of 203.0.113.7;
4. the same DNS client then resolves a real name through the *DHCP-leased*
   server (the kernel's own ``dns``/``https`` path uses the same lease);
5. ``float_printf`` checks %f/%e/%g against glibc-verified strings and prints
   FPU arithmetic, and the dlopen/thread/signal/file examples are re-run as a
   regression.

Usage::

    ./build.sh
    PAGH_RISCV=../pagh-riscv python3 tools/live-sockets.py

The kernel checkout must already be built (``cargo build --release``); the
default kernel is its release ELF. QEMU, mke2fs and debugfs are required. The
script writes the transcript to ``docs/live-sockets.txt`` (override with
``--transcript``); on failure it keeps the workdir and prints the failing
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
import struct
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
GUEST_TCP_PORT = 45679           # the guest's tcp_server listens here
UDP_LOCAL_PORT = 45678           # the guest's udp_echo binds this
DNS_TEST_NAME = "echo.pagh.test"
DNS_TEST_ADDR = bytes([203, 0, 113, 7])
DNS_DECOY_ADDR = bytes([198, 51, 100, 9])
DNS_NX_NAME = "nx.pagh.test"
TCP_MESSAGE = "hello from host"
TCP_CLIENT_MESSAGE = "pagh-tcp-ping"


class Failure(RuntimeError):
    """A check failed; the caller prints the transcript and exits 1."""


def die(message: str, code: int = 2) -> None:
    print(f"live-sockets: {message}", file=sys.stderr)
    raise SystemExit(code)


def check(condition: bool, what: str, transcript: str) -> None:
    if not condition:
        raise Failure(f"{what}\n--- serial transcript ---\n{transcript}")


def provenance(pagh_riscv: pathlib.Path, kernel: pathlib.Path,
               ignore: pathlib.Path | None = None) -> dict[str, str]:
    """Identify the two trees a run should be reproducible from.

    `ignore` is the transcript this run is about to rewrite: it is generated
    output, so its own modification must not make the recorded revision look
    "dirty"."""
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


class TcpClient:
    """Host client for the guest's tcp_server, through QEMU's hostfwd."""

    def __init__(self, port: int, payload: bytes) -> None:
        self.port = port
        self.payload = payload
        self.received = b""
        self.error: str | None = None
        self.connects = 0
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        deadline = time.time() + 120
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            while time.time() < deadline:
                self.connects += 1
                try:
                    client.connect(("127.0.0.1", self.port))
                    break
                except OSError:
                    time.sleep(0.2)
            else:
                self.error = "could not connect to the forwarded guest port"
                return
            client.settimeout(30)
            client.sendall(self.payload)
            chunks: list[bytes] = []
            while True:
                chunk = client.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
                if b"ack:" in b"".join(chunks):
                    break
            self.received = b"".join(chunks)
            client.shutdown(socket.SHUT_WR)
            while True:
                chunk = client.recv(4096)
                if not chunk:
                    break
                self.received += chunk
        except OSError as error:
            self.error = str(error)
        finally:
            client.close()

    def start(self) -> None:
        self.thread.start()

    def join(self, timeout: float = 40.0) -> None:
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


class DnsServer:
    """A one-name authoritative UDP DNS server.

    It answers ``echo.pagh.test`` with one A record (203.0.113.7) and NXDOMAIN
    for everything else. The first response it sends for a matching query is a
    *decoy* with a deliberately wrong transaction id and a different address:
    a resolver that did not check the id would print 198.51.100.9.
    """

    def __init__(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(120)
        self.port = self.sock.getsockname()[1]
        self.queries: list[tuple[str, int, int]] = []  # (name, qtype, id)
        self.decoys = 0
        self.answers = 0
        self.error: str | None = None
        self.thread = threading.Thread(target=self._serve, daemon=True)

    @staticmethod
    def _parse_name(msg: bytes, offset: int) -> tuple[str, int]:
        labels = []
        while offset < len(msg):
            length = msg[offset]
            if length == 0:
                offset += 1
                break
            if length & 0xC0:
                offset += 2
                break
            labels.append(msg[offset + 1:offset + 1 + length].decode("ascii", "replace"))
            offset += 1 + length
        return ".".join(labels), offset

    def _response(self, query: bytes, tid: int, rcode: int, answer: bytes) -> bytes:
        _name, offset = self._parse_name(query, 12)
        question_end = offset + 4
        flags = 0x8180 | rcode
        ancount = 1 if answer else 0
        header = struct.pack("!HHHHHH", tid, flags, 1, ancount, 0, 0)
        body = query[12:question_end]
        if answer:
            body += b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 60, 4) + answer
        return header + body

    def _serve(self) -> None:
        try:
            deadline = time.time() + 120
            while time.time() < deadline:
                try:
                    query, peer = self.sock.recvfrom(2048)
                except socket.timeout:
                    break
                if len(query) < 12:
                    continue
                tid = struct.unpack("!H", query[:2])[0]
                name, offset = self._parse_name(query, 12)
                qtype = struct.unpack("!H", query[offset:offset + 2])[0] if offset + 2 <= len(query) else 0
                self.queries.append((name, qtype, tid))
                if name.lower() == DNS_TEST_NAME:
                    self.sock.sendto(self._response(query, tid ^ 0x5555, 0, DNS_DECOY_ADDR), peer)
                    self.decoys += 1
                    self.sock.sendto(self._response(query, tid, 0, DNS_TEST_ADDR), peer)
                else:
                    self.sock.sendto(self._response(query, tid, 3, b""), peer)
                self.answers += 1
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
        text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
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
    tmp.write_text("# written by tools/live-sockets.py from the guest's DHCP lease\n"
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
    (root / "hello.txt").write_text("hello libc\n", encoding="utf-8")
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


REGRESSION = [
    # (label, executable, command, required markers)
    ("hello", "hello", "run /mnt/hello", ["hello from C on pagh-riscv"]),
    ("threads", "threads", "run /mnt/threads", ["counter=8 ret=50/30"]),
    ("signals", "signals", "run /mnt/signals", ["hits=1"]),
    ("fileio", "fileio", "run /mnt/fileio", ["line one", "line two"]),
    ("mutex", "mutex", "run /mnt/mutex", ["counter=1000"]),
    ("cond", "cond", "run /mnt/cond", ["ready=1 seen=1"]),
    ("cwd", "cwd", "run /mnt/cwd", ["cwd=/mnt", "relative read=hello libc"]),
    ("proc", "proc", "run /mnt/proc", ["code=0"]),
    ("env", "env", "run /mnt/env", ["PWD=/"]),
    ("ls", "ls", "run /mnt/ls /mnt", ["hello.txt"]),
    ("cat", "cat", "run /mnt/cat /mnt/hello.txt", ["hello libc"]),
    ("dlopen", "ld-pagh", "run /mnt/ld-pagh /mnt/hello_dyn", ["dlopen: plug_value=42"]),
    # Process exit: exit() must run the atexit handlers and end the whole run
    # even though a worker thread is still alive (SYS_EXIT_GROUP, not SYS_EXIT).
    ("exit_semantics", "exit_semantics", "run /mnt/exit_semantics",
     ["exit_semantics: atexit handler ran"]),
]


# ── the run ───────────────────────────────────────────────────────────────────

def run(args: argparse.Namespace) -> int:
    work = pathlib.Path(tempfile.mkdtemp(prefix="pagh-libc-live-"))
    kernel = args.kernel
    if not kernel.is_absolute():
        kernel = args.pagh_riscv / kernel
    if not kernel.is_file():
        die(f"kernel {kernel} not found — build the kernel checkout first")
    if not (ROOT / "build" / "tcp_server.elf").is_file():
        die("build/tcp_server.elf missing — run ./build.sh first")

    disk = make_image(work)
    guest = Guest(work, kernel, args.pagh_riscv)
    ids = provenance(args.pagh_riscv, kernel, ignore=args.transcript)
    print("live-sockets: kernel " + ids["kernel_sha256"][:16] + " rev " + ids["kernel_rev"] +
          " crate " + ids.get("kernel_crate", "unknown"), flush=True)
    record: list[str] = []
    steps: list[str] = []

    def banner(title: str) -> None:
        print(f"live-sockets: == {title} ==", flush=True)

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
        print(f"live-sockets: DHCP-leased DNS server {dns_server} -> /etc/resolv.conf", flush=True)

        # 2. Boot once more with the peers, then drive it with --attach.
        host_port = free_host_port()
        banner(f"boot 2: --net --hostfwd {host_port}->{GUEST_TCP_PORT}")
        boot_log = guest.boot(net=True, keep=True,
                              hostfwd=f"{host_port}:{GUEST_TCP_PORT}", settle=2.0)
        record.append(f"== boot 2: hostfwd 127.0.0.1:{host_port} -> guest "
                      f"10.0.2.15:{GUEST_TCP_PORT} ==\n" + boot_log)

        # ── TCP server: guest listens, host connects through hostfwd ──────────
        banner("tcp_server")
        client = TcpClient(host_port, TCP_MESSAGE.encode() + b"\n")
        client.start()
        log = guest.attach(f"run /mnt/tcp_server {GUEST_TCP_PORT}\\n", settle=8.0)
        client.join(timeout=40)
        steps.append("== tcp_server: guest listener, host client via hostfwd ==\n" + log)
        check(f"tcp_server: listening on 0.0.0.0:{GUEST_TCP_PORT}" in log,
              "tcp_server did not listen", log)
        check("tcp_server: SO_TYPE=1 (SOCK_STREAM=1)" in log, "getsockopt SO_TYPE", log)
        check("tcp_server: accept from 10.0.2.2:" in log, "tcp_server did not accept", log)
        check(f"tcp_server: recv {len(TCP_MESSAGE) + 1} bytes: {TCP_MESSAGE}" in log,
              "tcp_server did not receive the host line", log)
        check(f"tcp_server: sent {len(TCP_MESSAGE) + 5} bytes" in log,
              "tcp_server did not send its reply", log)
        check("tcp_server: peer closed" in log, "tcp_server did not see the peer close", log)
        check("tcp_server: ok" in log, "tcp_server did not report success", log)
        check("run: /mnt/tcp_server exited with code 0" in log, "tcp_server exit status", log)
        check(not client.alive, "host TCP client never finished", log)
        check(client.error is None, f"host TCP client error: {client.error}", log)
        check(client.received == b"ack:" + TCP_MESSAGE.encode() + b"\n",
              f"host TCP client received {client.received!r}", log)
        steps.append(f"== host TCP client ==\nconnected after {client.connects} attempt(s); "
                     f"sent {TCP_MESSAGE!r}; received {client.received!r}\n")

        # ── TCP client: guest connects to a host echo server ──────────────────
        banner("tcp_client")
        echo = TcpEchoServer()
        echo.start()
        log = guest.attach(f"run /mnt/tcp_client 10.0.2.2 {echo.port} {TCP_CLIENT_MESSAGE}\\n",
                           settle=8.0)
        echo.join(timeout=30)
        steps.append("== tcp_client: guest connect/send/recv to host peer ==\n" + log)
        check(f"tcp_client: connecting to 10.0.2.2:{echo.port}" in log, "tcp_client connect", log)
        check("tcp_client: connected" in log, "tcp_client did not connect", log)
        check("tcp_client: SO_ERROR=0" in log, "tcp_client SO_ERROR", log)
        check(f"tcp_client: echo {len(TCP_CLIENT_MESSAGE)} bytes: {TCP_CLIENT_MESSAGE}" in log,
              "tcp_client did not receive its echo", log)
        check("tcp_client: ok" in log, "tcp_client did not report success", log)
        check("run: /mnt/tcp_client exited with code 0" in log, "tcp_client exit status", log)
        check(not echo.alive, "host TCP echo server never finished", log)
        check(echo.error is None, f"host TCP echo server error: {echo.error}", log)
        check(echo.received == TCP_CLIENT_MESSAGE.encode(),
              f"host echo server received {echo.received!r}", log)
        steps.append(f"== host TCP echo server ==\nport {echo.port}; received {echo.received!r}\n")

        # ── UDP echo round trips ──────────────────────────────────────────────
        banner("udp_echo")
        udp = UdpEchoServer(expected=3)
        udp.start()
        log = guest.attach(f"run /mnt/udp_echo 10.0.2.2 {udp.port} 3 {UDP_LOCAL_PORT}\\n",
                           settle=10.0)
        udp.join(timeout=30)
        steps.append("== udp_echo: sendto/recvfrom round trips ==\n" + log)
        check(f"udp_echo: bound 0.0.0.0:{UDP_LOCAL_PORT}" in log, "udp_echo bind", log)
        check("udp_echo: SO_TYPE=2 (SOCK_DGRAM=2)" in log, "getsockopt SO_TYPE on UDP", log)
        check("udp_echo: ok (3 datagrams echoed)" in log, "udp_echo success marker", log)
        check("run: /mnt/udp_echo exited with code 0" in log, "udp_echo exit status", log)
        check(not udp.alive, "host UDP echo server never finished", log)
        check(udp.error is None, f"host UDP echo server error: {udp.error}", log)
        check(len(udp.received) == 3, f"host UDP server saw {len(udp.received)} datagrams", log)
        check(all(data == f"pagh udp echo #{i}".encode() for i, (data, _) in
                  enumerate(udp.received, start=1)), f"host UDP payloads {udp.received!r}", log)
        steps.append("== host UDP echo server ==\n" +
                     "\n".join(f"{data!r} from {peer[0]}:{peer[1]}"
                               for data, peer in udp.received) + "\n")

        # ── DNS: deterministic host server (decoy + NXDOMAIN) ─────────────────
        banner("dns_lookup (pinned host server)")
        dns = DnsServer()
        dns.start()
        log = guest.attach(f"run /mnt/dns_lookup @10.0.2.2:{dns.port} {DNS_TEST_NAME}\\n",
                           settle=10.0)
        steps.append("== dns_lookup: pinned host server, decoy packet first ==\n" + log)
        check(f"dns: pinned server 10.0.2.2:{dns.port}" in log, "dns pin", log)
        check(f"dns: gethostbyname {DNS_TEST_NAME} -> 203.0.113.7" in log,
              "gethostbyname did not return the host server's A record", log)
        check("dns: getaddrinfo echo.pagh.test http -> 203.0.113.7:80" in log,
              "getaddrinfo did not build the sockaddr", log)
        check("dns: ok" in log, "dns_lookup did not report success", log)
        check("run: /mnt/dns_lookup exited with code 0" in log, "dns_lookup exit status", log)
        check("198.51.100.9" not in log, "the decoy answer was accepted instead of ignored", log)

        log = guest.attach(f"run /mnt/dns_lookup @10.0.2.2:{dns.port} {DNS_NX_NAME}\\n",
                           settle=10.0)
        steps.append("== dns_lookup: NXDOMAIN from the same server ==\n" + log)
        check(f"dns: {DNS_NX_NAME}: FAILED: host not found (h_errno=1)" in log,
              "NXDOMAIN was not reported as host not found", log)
        check("run: /mnt/dns_lookup exited with code 1" in log, "NXDOMAIN exit status", log)
        dns.join(timeout=5)
        check(dns.error is None, f"host DNS server error: {dns.error}", log)
        # The example asks twice for echo.pagh.test (gethostbyname then
        # getaddrinfo -- the resolver has no cache, and that is documented), so
        # two queries and two decoys precede the NXDOMAIN query.
        check(dns.decoys == 2, f"host DNS server sent {dns.decoys} decoys", log)
        check(len(dns.queries) == 3, f"host DNS server saw {dns.queries!r}", log)
        check(all(name == DNS_TEST_NAME and qtype == 1 for name, qtype, _ in dns.queries[:2]),
              f"the first queries were {dns.queries[:2]!r}", log)
        check(dns.queries[2][0] == DNS_NX_NAME, f"second query was {dns.queries[2]!r}", log)
        steps.append("== host DNS server ==\n" +
                     "\n".join(f"query {name} type={qtype} id={tid}"
                               for name, qtype, tid in dns.queries) +
                     f"\ndecoy (wrong id, 198.51.100.9) sent before the real answer for "
                     f"{DNS_TEST_NAME}; the guest printed 203.0.113.7\n")

        # ── DNS: the DHCP-leased server, through /etc/resolv.conf ─────────────
        banner("dns_lookup (DHCP server)")
        log = guest.attach("run /mnt/dns_lookup example.com\\n", settle=12.0)
        steps.append("== dns_lookup: DHCP-leased server via /mnt/etc/resolv.conf ==\n" + log)
        check(f"dns: server {dns_server}:53 (from /mnt/etc/resolv.conf)" in log,
              f"the resolver did not use the leased server {dns_server}", log)
        check("dns: gethostbyname example.com -> " in log,
              "the DHCP server did not resolve example.com", log)
        check("dns: ok" in log, "dns_lookup did not report success", log)
        check("run: /mnt/dns_lookup exited with code 0" in log, "dns_lookup exit status", log)

        # ── %f / %e / %g and FPU arithmetic ───────────────────────────────────
        banner("float_printf")
        log = guest.attach("run /mnt/float_printf\\n", settle=5.0)
        steps.append("== float_printf: %f/%e/%g against glibc-verified strings ==\n" + log)
        check("float: 1.5*2.25 = 3.375" in log, "FPU multiply", log)
        check("float: sum(1..100) = 5050.0" in log, "FPU accumulation", log)
        check("float: 355/113 = 3.141593" in log, "%f of a computed value", log)
        check("float: ok (" in log, "float_printf success marker", log)
        check("run: /mnt/float_printf exited with code 0" in log, "float_printf exit status", log)

        # ── regression: dlopen, threads, signals, files, everything else ──────
        for label, exe, command, markers in REGRESSION:
            banner(f"regression: {label}")
            log = guest.attach(command + "\\n", settle=6.0)
            steps.append(f"== regression: {command} ==\n" + log)
            check(f"run: /mnt/{exe} exited with code 0" in log, f"{label} exit status", log)
            for marker in markers:
                check(marker in log, f"{label}: missing {marker!r}", log)
    except Failure:
        guest.stop()
        print(f"live-sockets: workdir kept for inspection: {work}", file=sys.stderr)
        raise
    finally:
        guest.stop()

    transcript = (
        "# Live socket/DNS/%f acceptance for pagh-libc\n"
        f"# kernel: {kernel}\n"
        f"# kernel checkout: {args.pagh_riscv}\n"
        f"# kernel revision: {ids['kernel_rev']} (crate {ids.get('kernel_crate', 'unknown')}, "
        f"tag {ids['kernel_version']})\n"
        f"# kernel sha256: {ids['kernel_sha256']}\n"
        f"# pagh-libc revision: {ids['libc_rev']} ({ids['libc_dirty']})\n"
        f"# host: {ids['qemu']}\n"
        f"# image: mke2fs -t ext2 -b 1024 -d <build/*.elf + etc/resolv.conf> disk.img\n"
        f"# DHCP-leased DNS server: {dns_server}\n"
        f"# guest TCP port: {GUEST_TCP_PORT} (QEMU hostfwd 127.0.0.1:{host_port})\n"
        f"# guest UDP local port: {UDP_LOCAL_PORT}\n"
        "\n" + "\n".join(record) + "\n" + "\n".join(steps) +
        "\nRESULT: PASS — sockets, DNS, %f and the dlopen/thread/signal/file "
        "regressions all green\n"
    )
    text = "\n".join(line.rstrip() for line in transcript.splitlines()) + "\n"
    path = args.transcript
    if not path.is_absolute():
        path = ROOT / path
    path.write_text(text, encoding="utf-8")
    print(f"live-sockets: PASS — {path} ({len(text)} bytes)")
    if args.keep:
        print(f"live-sockets: workdir {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Live socket/DNS/%f acceptance for pagh-libc")
    parser.add_argument("--pagh-riscv", type=pathlib.Path,
                        default=pathlib.Path(os.environ.get("PAGH_RISCV", str(PAGH_RISCV))),
                        help=f"pagh-riscv checkout (default {PAGH_RISCV})")
    parser.add_argument("--kernel", type=pathlib.Path, default=pathlib.Path(KERNEL_RELEASE),
                        help=f"kernel ELF, relative to --pagh-riscv (default {KERNEL_RELEASE})")
    parser.add_argument("--debug", action="store_true", help=f"use {KERNEL_DEBUG}")
    parser.add_argument("--transcript", type=pathlib.Path,
                        default=ROOT / "docs/live-sockets.txt",
                        help="where to write the transcript (default docs/live-sockets.txt)")
    parser.add_argument("--keep", action="store_true", help="keep the temporary workdir")
    args = parser.parse_args()
    if args.debug:
        args.kernel = pathlib.Path(KERNEL_DEBUG)
    try:
        return run(args)
    except Failure as failure:
        print(f"live-sockets: FAIL — {failure}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
