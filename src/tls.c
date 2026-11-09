/* The kernel TLS session syscalls (1100..1103), `getrandom` (278) and the
 * `https_get` convenience built on them.
 *
 * The wrappers are thin on purpose: the handshake, the certificate checks and
 * the record layer are the kernel's (see <pagh_tls.h>), and this file must not
 * add a policy of its own — no flag, no fallback, no retry that could turn a
 * refusal into a success. What it does add is the HTTP/1.1 request and the
 * response framing, which the pure half of lives in src/http.c so the host test
 * can exercise it.
 *
 * `EAGAIN` from a session call means "another thread of this process is inside
 * a call on this session" (a read blocks until data is there; it is not the
 * non-blocking-TCP `EAGAIN`), so the loops here wait and retry instead of
 * failing a fetch on a scheduling accident. */
#include <pagh.h>
#include <pagh_tls.h>
#include <pagh_internal.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long ret(long r) {
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

ssize_t getrandom(void *buf, size_t len, unsigned int flags) {
    return ret(__pagh_syscall3(SYS_GETRANDOM, (long)buf, (long)len, (long)flags));
}

int pagh_tls_connect(int fd, const char *host) {
    if (!host) {
        errno = EINVAL;
        return -1;
    }
    return (int)ret(__pagh_syscall6(SYS_TLS_CONNECT, fd, (long)host, (long)strlen(host), 0, 0, 0));
}

ssize_t pagh_tls_write(int fd, const void *buf, size_t len) {
    return ret(__pagh_syscall3(SYS_TLS_WRITE, fd, (long)buf, (long)len));
}

ssize_t pagh_tls_read(int fd, void *buf, size_t len) {
    return ret(__pagh_syscall3(SYS_TLS_READ, fd, (long)buf, (long)len));
}

int pagh_tls_close(int fd) {
    return (int)ret(__pagh_syscall3(SYS_TLS_CLOSE, fd, 0, 0));
}

/* ── https_get ───────────────────────────────────────────────────────────── */

/* One read from the session, retrying the "another thread is inside a call"
 * answer. A read blocks until at least one plaintext byte is available, so a
 * successful return is never a false "no data". */
static ssize_t tls_read_wait(int fd, void *buf, size_t cap) {
    for (;;) {
        ssize_t n = pagh_tls_read(fd, buf, cap);
        if (n >= 0 || errno != EAGAIN) return n;
        usleep(1000);
    }
}

/* Write the whole request: `tls_write` may accept a short prefix, so a loop is
 * the contract, not an optimisation. */
static int tls_write_all(int fd, const char *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = pagh_tls_write(fd, buf + done, len - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        if (errno != EAGAIN) return -1;
        usleep(1000);
    }
    return 0;
}

ssize_t https_get(const char *host, const char *path, char *out, size_t cap) {
    if (!host || !*host || !path || !*path || !out || cap == 0) {
        errno = EINVAL;
        return -1;
    }
    if (path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    size_t host_len = strlen(host);
    if (host_len > 255) { /* the ABI's tls_connect host cap */
        errno = EINVAL;
        return -1;
    }

    /* The name is resolved here; the TLS handshake authenticates it later, so a
     * DNS answer that points at the wrong server is caught there. */
    struct hostent *entry = gethostbyname(host);
    if (!entry) {
        errno = ENOENT; /* h_errno names the resolver's verdict */
        return -1;
    }
    unsigned char address[4];
    memcpy(address, entry->h_addr_list[0], 4);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(PAGH_HTTPS_PORT);
    memcpy(&to.sin_addr.s_addr, address, 4);
    if (connect(fd, (struct sockaddr *)&to, sizeof to) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }

    /* Every refusal from here on gets the fd closed: after a failed handshake
     * the kernel leaves a tombstone, and that is what releases it. The errno of
     * the failing call is the one the caller sees. */
    if (pagh_tls_connect(fd, host) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }

    char request[1024];
    int request_len = snprintf(request, sizeof request,
                               "GET %s HTTP/1.1\r\nHost: %s\r\n"
                               "User-Agent: pagh-libc/https_get\r\nAccept: */*\r\n"
                               "Connection: close\r\n\r\n",
                               path, host);
    int failed = 0;
    ssize_t body_len = -1;
    size_t used = 0;
    if (request_len <= 0 || (size_t)request_len >= sizeof request) {
        errno = EINVAL;
        failed = 1;
        goto out;
    }
    if (tls_write_all(fd, request, (size_t)request_len) < 0) {
        failed = 1;
        goto out;
    }

    /* Read until the head is complete. Whatever follows it in the same read is
     * body, and is handed to the framing below rather than thrown away. */
    char head[PAGH_HTTPS_HEAD_MAX];
    size_t head_len = 0;
    struct pagh_http_head parsed;
    for (;;) {
        if (head_len == sizeof head) {
            errno = EIO;
            failed = 1;
            goto out;
        }
        ssize_t n = tls_read_wait(fd, head + head_len, sizeof head - head_len);
        if (n < 0) {
            failed = 1;
            goto out;
        }
        if (n == 0) { /* the peer closed before the head ended */
            errno = EIO;
            failed = 1;
            goto out;
        }
        head_len += (size_t)n;
        int complete = __pagh_http_parse_head(head, head_len, &parsed);
        if (complete < 0) {
            errno = EIO;
            failed = 1;
            goto out;
        }
        if (complete > 0) break;
    }
    if (parsed.status != 200) {
        errno = EIO; /* documented: only a 200 has a body this call can return */
        failed = 1;
        goto out;
    }

    size_t buffered = head_len - parsed.body_off; /* body bytes already in `head` */
    char window[4096];

    if (parsed.framing == PAGH_HTTP_LENGTH) {
        size_t want = (size_t)parsed.length;
        if (want > cap) {
            errno = ENOSPC;
            failed = 1;
            goto out;
        }
        size_t got = buffered < want ? buffered : want;
        memcpy(out, head + parsed.body_off, got);
        while (got < want) {
            ssize_t n = tls_read_wait(fd, out + got, want - got);
            if (n < 0) {
                failed = 1;
                goto out;
            }
            if (n == 0) { /* the length promised more than the stream held */
                errno = EIO;
                failed = 1;
                goto out;
            }
            got += (size_t)n;
        }
        used = want;
    } else if (parsed.framing == PAGH_HTTP_CHUNKED) {
        struct pagh_http_chunk state;
        __pagh_http_chunk_init(&state);
        const char *chunk = head + parsed.body_off;
        size_t chunk_len = buffered;
        for (;;) {
            int rc = __pagh_http_chunk_feed(&state, chunk, chunk_len, out, cap, &used);
            if (rc < 0) {
                errno = rc == -2 ? ENOSPC : EIO;
                failed = 1;
                goto out;
            }
            if (rc == 1) break; /* the terminal chunk arrived */
            chunk = window;
            ssize_t n = tls_read_wait(fd, window, sizeof window);
            if (n < 0) {
                failed = 1;
                goto out;
            }
            if (n == 0) { /* closed mid-body: the terminal chunk never came */
                errno = EIO;
                failed = 1;
                goto out;
            }
            chunk_len = (size_t)n;
        }
    } else { /* PAGH_HTTP_UNTIL_CLOSE: the body ends when the peer closes */
        if (buffered > cap) {
            errno = ENOSPC;
            failed = 1;
            goto out;
        }
        memcpy(out, head + parsed.body_off, buffered);
        used = buffered;
        for (;;) {
            if (used == cap) {
                /* Exactly full with no end in sight: one more byte would prove
                 * the body is larger than `cap`, a clean close proves it fit. */
                char probe;
                ssize_t n = tls_read_wait(fd, &probe, 1);
                if (n < 0) {
                    failed = 1;
                    goto out;
                }
                if (n > 0) {
                    errno = ENOSPC;
                    failed = 1;
                    goto out;
                }
                break;
            }
            ssize_t n = tls_read_wait(fd, out + used, cap - used);
            if (n < 0) { /* ECONNRESET: the stream ended without close_notify */
                failed = 1;
                goto out;
            }
            if (n == 0) break; /* close_notify: the body is complete */
            used += (size_t)n;
        }
    }
    body_len = (ssize_t)used;

out: {
    int saved = errno;
    if (pagh_tls_close(fd) < 0 && !failed) {
        failed = 1;
        saved = errno;
    }
    if (failed) {
        errno = saved;
        return -1;
    }
    return body_len;
}
}
