#ifndef PAGH_TLS_H
#define PAGH_TLS_H
/* Kernel TLS 1.3 sessions and entropy for userland (kernel >= 0.21.0).
 *
 * pagh-riscv 0.21.0 exposes the kernel's own TLS client to userland: syscall
 * 1100 (`tls_connect`) runs the **same fail-closed handshake** the shell's
 * `https` command runs — the certificate chain must reach the committed CA
 * bundle, the SAN entries must authorize the name passed here, the clock gate
 * must pass and `CertificateVerify` must prove possession of the leaf key — on
 * a socket the program has already connected. Nothing in this header can weaken
 * any of it: the ABI defines `flags = 0` and answers -EINVAL for anything else,
 * so there is no "insecure" bit to set, and the kernel abandons the connection
 * (RFC 8446 §6.1) when verification refuses.
 *
 * The descriptor keeps the socket's number. After a successful
 * `pagh_tls_connect` the same fd carries the session:
 * `pagh_tls_read`/`pagh_tls_write` move **plaintext** through it, and
 * `pagh_tls_close` sends `close_notify` and releases both the session buffers
 * and the descriptor. A failed handshake leaves the fd as a tombstone
 * (`-ENOTSUP` for I/O): `close()` it and open a new socket to retry.
 *
 * `getrandom` is the entropy the handshake itself draws on. It is the same
 * kernel pool, so a program that needs a key, nonce or salt asks here rather
 * than inventing one from the clock; without a seed the kernel refuses
 * (`EAGAIN`) instead of returning predictable bytes.
 *
 * Errors are the library's usual shape: -1 with `errno`. The kernel's TLS
 * errnos reach the caller unchanged — `EKEYREJECTED` (129: the certificate was
 * rejected), `EPROTO` (the handshake broke), `EAGAIN` (no entropy seed, or
 * another thread is inside a call on this session), `ENOTSUP` (no wall clock),
 * `ECONNRESET` (the peer ended the stream without `close_notify`), `EINVAL`
 * (the fd is not a TLS session), `EBADF`.
 *
 * These calls are 0.21.0+; on an older kernel the unknown syscall is -ENOSYS.
 *
 * ```c
 * ssize_t n = https_get("example.com", "/", body, sizeof body);
 * ```
 * is the whole point of the header; see examples/https_get.c and
 * examples/tls_client.c for the session shape underneath it. */
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Largest response head `https_get` accepts, in bytes. A head that does not
 * end (RFC 9112 `CRLF CRLF`) within this is refused rather than buffered. */
#define PAGH_HTTPS_HEAD_MAX 8192

/* The port `https_get` connects to. There is no plaintext mode. */
#define PAGH_HTTPS_PORT 443

/* `getrandom(buf, len, flags)` — fill `buf` from the kernel entropy pool.
 *
 * `flags` must be 0: the ABI defines no flags and refuses Linux's `GRND_*`
 * with -EINVAL rather than ignoring them (an ignored `GRND_INSECURE` would hand
 * a caller bytes it believes are unpredictable).
 *
 * Returns the number of bytes written — at most the kernel's per-call cap, so a
 * larger request gets a short count and the caller loops — or -1 with errno:
 *   EAGAIN  the pool has no seed; the kernel refuses to invent randomness;
 *   EFAULT  the whole range was not readable and writable (nothing is written);
 *   EINVAL  `flags` was not 0;
 *   ENOSYS  the kernel is older than 0.21.0.
 * `len == 0` returns 0 without touching the pool. */
ssize_t getrandom(void *buf, size_t len, unsigned int flags);

/* Perform the TLS 1.3 handshake on the **connected** stream socket `fd`.
 *
 * `host` is the name the server must prove it is: it goes out as SNI and its
 * certificate's SAN entries are matched against it. It is *not* resolved and no
 * connection is opened here — the caller connects the socket first, so a name
 * can be tested against a pinned address.
 *
 * `host` must be 1..=255 bytes with no NUL or control byte (-EINVAL).
 * Returns 0, or -1 with errno (EKEYREJECTED for a rejected certificate, EPROTO
 * for a broken handshake, EAGAIN without an entropy seed, ENOTSUP without a
 * clock, ENOTCONN for an unconnected socket, EISCONN for a listener). */
int pagh_tls_connect(int fd, const char *host);

/* Encrypt `len` plaintext bytes from `buf` into the session on `fd`.
 *
 * Blocks (bounded by the session's idle budget) until the record layer accepted
 * them; returns the byte count, which is `len` unless the connection failed
 * part-way (a short count is the caller's signal to stop). -1 with errno:
 * EINVAL when `fd` is not a session, EFAULT for a bad buffer, EPIPE after
 * `pagh_tls_close` or a received `close_notify`, ECONNRESET, ETIMEDOUT,
 * EAGAIN for a concurrent call by another thread of this process. */
ssize_t pagh_tls_write(int fd, const void *buf, size_t len);

/* Decrypt up to `len` plaintext bytes into `buf`.
 *
 * **Blocks** until at least one byte is available, then returns up to one
 * record's worth; `0` is end of stream (`close_notify` or a clean TCP close).
 * A TCP close **without** `close_notify` is -1/ECONNRESET, not 0 — the
 * layering is what makes a truncated stream visible instead of looking like a
 * complete body. -1 with errno, as `pagh_tls_write`. */
ssize_t pagh_tls_read(int fd, void *buf, size_t len);

/* Send `close_notify` (best effort, bounded) and release the session, the TCP
 * connection and the descriptor.
 *
 * Returns 0 or -1 with errno (EINVAL when `fd` is not a session). `0` does not
 * promise the peer received the alert. `close(fd)` also releases a session —
 * without the alert, which is why a graceful goodbye calls this. */
int pagh_tls_close(int fd);

/* One HTTPS/1.1 `GET path` on `host`: resolve the name with the library's
 * resolver (<netdb.h>, `/etc/resolv.conf` or `/mnt/etc/resolv.conf`), open a
 * TCP connection to port 443, hand the socket to `pagh_tls_connect`, send the
 * request and copy the **response body** into `out`, which holds `cap` bytes.
 *
 * Returns the body length (>= 0), or -1 with errno:
 *   EINVAL   a NULL/empty argument, a `path` that does not start with '/',
 *            a host longer than the 255-byte ABI cap, or `cap == 0`;
 *   ENOENT   the name did not resolve; `h_errno` names the resolver's verdict;
 *   (kernel) the socket and TLS errnos exactly as the kernel returned them:
 *            EKEYREJECTED for a rejected certificate, EPROTO for a broken
 *            handshake, EAGAIN for an unseeded entropy pool, ECONNREFUSED /
 *            ENETUNREACH / ETIMEDOUT from `connect`;
 *   EIO      the status was not 200, the head exceeded PAGH_HTTPS_HEAD_MAX, the
 *            framing was malformed, or the body was **truncated** (a
 *            close-delimited body that does not end with `close_notify`);
 *   ENOSPC   the body did not fit in `cap`. It is refused, never truncated; on
 *            this error `out` may hold a partial body, so only the return value
 *            says what is there.
 *
 * The request asks for `Connection: close`. Three framings are understood, in
 * the order RFC 9112 defines them: `Transfer-Encoding: chunked` (chunk
 * extensions skipped, the terminal chunk required), `Content-Length` (a short
 * stream is an error, not a shorter body), and neither (close-delimited: the
 * body ends at `close_notify`, and a TCP reset there is a truncation error). */
ssize_t https_get(const char *host, const char *path, char *out, size_t cap);

#endif
