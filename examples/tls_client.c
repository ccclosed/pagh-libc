/* A TLS 1.3 session driven by hand on the native pagh ABI: resolve -> TCP
 * connect -> `pagh_tls_connect` (the kernel's fail-closed handshake) -> write a
 * request -> `pagh_tls_read` the response -> `pagh_tls_close`.
 *
 *   run /mnt/tls_client example.com
 *   run /mnt/tls_client example.com /index.html
 *   run /mnt/tls_client --ip 10.0.2.2 --port 9443 example.com
 *
 * `--ip A.B.C.D` pins the destination while `host` stays the name the
 * certificate must authorize (SNI + SAN). That is how a negative test asks a
 * server for a name its certificate does not cover — which is the whole reason
 * the SAN check exists — and how a locally pinned server is reached through
 * QEMU's user network (10.0.2.2 is the host's loopback).
 *
 * The first thing this program does is draw 16 bytes from `getrandom`: the
 * handshake's entropy is the same kernel pool, so a program that cannot get
 * random bytes has no business starting a TLS session, and saying so before the
 * socket is honest about which gate refused.
 *
 * The HTTP framing here is deliberately the simple one: ask for
 * `Connection: close`, stop at the `Content-Length` the head declared (or at
 * end of stream when it declared none). `https_get` in the same library is the
 * one that understands chunked bodies and refuses a truncated one; this example
 * exists to show the session calls, not to re-implement that.
 *
 * Exit codes: 0 success, 2 usage, 3 getrandom, 4 resolve, 5 connect, 6 TLS
 * handshake (including any rejected certificate), 7 request write, 8 response
 * read/framing, 9 the response did not fit in `--max`. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pagh_tls.h>

#define MAX_RESPONSE_DEFAULT 65536

static void usage(void) {
    printf("usage: tls_client [--ip A.B.C.D] [--port N] [--path P] [--max N] host [path]\n");
    printf("       tls_client example.com\n");
    printf("       tls_client --ip 10.0.2.2 --port 9443 example.com   (chain gate)\n");
}

/* 16 random bytes as hex, plus a second draw that must differ: the kernel's
 * pool ratchets after every block, so two equal draws would mean a broken
 * source, not bad luck. Returns 0 when the pool answered twice. */
static int entropy_check(void) {
    unsigned char first[16], second[16];
    ssize_t got = getrandom(first, sizeof first, 0);
    if (got != (ssize_t)sizeof first) {
        printf("tls_client: getrandom: errno=%d %s\n", errno, strerror(errno));
        return -1;
    }
    printf("tls_client: getrandom %ld bytes:", (long)got);
    for (size_t i = 0; i < sizeof first; i++) printf("%02x", first[i]);
    printf("\n");
    if (getrandom(second, sizeof second, 0) != (ssize_t)sizeof second) {
        printf("tls_client: getrandom (second draw): errno=%d %s\n", errno, strerror(errno));
        return -1;
    }
    if (memcmp(first, second, sizeof first) == 0) {
        printf("tls_client: getrandom returned the same 16 bytes twice\n");
        return -1;
    }
    return 0;
}

/* Does the head carry `name` (ASCII-insensitively)? Used only to say in the
 * output whether the body arrived chunk-framed, which this example prints
 * verbatim — decoding it is https_get's job, not this program's. */
static int head_has(const char *head, size_t len, const char *word) {
    size_t n = strlen(word);
    for (size_t i = 0; i + n <= len; i++) {
        size_t j = 0;
        while (j < n) {
            char c = head[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != word[j]) break;
            j++;
        }
        if (j == n) return 1;
    }
    return 0;
}

/* Where the body starts, or -1 when the head never ended. */
static long head_end(const char *data, size_t len) {
    for (size_t i = 0; i + 4 <= len; i++)
        if (memcmp(data + i, "\r\n\r\n", 4) == 0) return (long)(i + 4);
    return -1;
}

/* A `Content-Length: N` in the head, or -1 when the head does not declare one.
 * Case-insensitive, and only the first such header is read — the example does
 * not need the duplicate-header rules https_get implements. */
static long content_length(const char *head, size_t head_len) {
    static const char name[] = "content-length:";
    for (size_t i = 0; i + sizeof name - 1 <= head_len; i++) {
        size_t j = 0;
        while (j < sizeof name - 1) {
            char c = head[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != name[j]) break;
            j++;
        }
        if (j != sizeof name - 1) continue;
        const char *p = head + i + sizeof name - 1;
        while (*p == ' ' || *p == '\t') p++;
        long value = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') { value = value * 10 + (*p++ - '0'); digits++; }
        if (digits) return value;
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *host = 0;
    const char *path = "/";
    const char *pinned = 0;
    int port = PAGH_HTTPS_PORT;
    size_t max = MAX_RESPONSE_DEFAULT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            pinned = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
            max = (size_t)atol(argv[++i]);
        } else if (argv[i][0] == '-') {
            printf("tls_client: unknown option %s\n", argv[i]);
            usage();
            return 2;
        } else if (!host) {
            host = argv[i];
        } else {
            path = argv[i];
        }
    }
    if (!host || port <= 0 || port > 65535 || max == 0) {
        usage();
        return 2;
    }

    printf("tls_client: GET %s%s (port %d)\n", host, path, port);
    if (entropy_check() < 0) return 3;

    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)port);
    if (pinned) {
        if (!inet_aton(pinned, &to.sin_addr)) {
            printf("tls_client: --ip %s: not an IPv4 address\n", pinned);
            return 2;
        }
        printf("tls_client: destination pinned to %s:%d\n", pinned, port);
    } else {
        struct hostent *entry = gethostbyname(host);
        if (!entry) {
            printf("tls_client: FAILED stage=resolve %s: %s (h_errno=%d)\n",
                   host, hstrerror(h_errno), h_errno);
            return 4;
        }
        memcpy(&to.sin_addr.s_addr, entry->h_addr_list[0], 4);
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &to.sin_addr, text, (socklen_t)sizeof text);
        printf("tls_client: resolve %s -> %s\n", host, text);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("tls_client: FAILED stage=socket errno=%d %s\n", errno, strerror(errno));
        return 5;
    }
    if (connect(fd, (struct sockaddr *)&to, sizeof to) < 0) {
        printf("tls_client: FAILED stage=connect errno=%d %s\n", errno, strerror(errno));
        close(fd);
        return 5;
    }
    char peer[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &to.sin_addr, peer, (socklen_t)sizeof peer);
    printf("tls_client: tcp connected to %s:%d\n", peer, port);

    /* The handshake and every certificate decision are the kernel's. A refusal
     * here is final: the connection is gone and no application byte was sent. */
    if (pagh_tls_connect(fd, host) < 0) {
        int error = errno;
        printf("tls_client: FAILED stage=handshake errno=%d %s (server name %s)\n",
               error, strerror(error), host);
        printf("tls_client: the kernel refused the server; no request was written\n");
        close(fd);
        return 6;
    }
    printf("tls_client: handshake ok: TLS 1.3 session on fd %d, \"%s\" authenticated by the kernel\n",
           fd, host);

    char request[1024];
    int request_len = snprintf(request, sizeof request,
                               "GET %s HTTP/1.1\r\nHost: %s\r\n"
                               "User-Agent: pagh-libc/tls_client\r\nAccept: */*\r\n"
                               "Connection: close\r\n\r\n",
                               path, host);
    if (request_len <= 0 || (size_t)request_len >= sizeof request) {
        printf("tls_client: request does not fit (%d bytes)\n", request_len);
        pagh_tls_close(fd);
        return 7;
    }
    long sent = 0;
    while (sent < request_len) {
        ssize_t n = pagh_tls_write(fd, request + sent, (size_t)(request_len - sent));
        if (n > 0) { sent += n; continue; }
        printf("tls_client: FAILED stage=write errno=%d %s\n", errno, strerror(errno));
        pagh_tls_close(fd);
        return 7;
    }
    printf("tls_client: sent %ld bytes\n", sent);

    /* Read until the length the head declared is satisfied, or until the peer
     * ends the stream. A TCP close without `close_notify` is ECONNRESET here,
     * not end-of-file: the session layer refuses to let a truncation look like
     * a complete body, and this program prints which of the two happened. */
    char *response = malloc(max + 1);
    if (!response) {
        printf("tls_client: out of memory (%lu bytes)\n", (unsigned long)max);
        pagh_tls_close(fd);
        return 8;
    }
    size_t total = 0;
    long body_at = -1;
    long want = -1;
    int ended = 0;
    const char *how = "close_notify";
    while (!ended) {
        if (total == max) {
            printf("tls_client: response over --max %lu bytes\n", (unsigned long)max);
            free(response);
            pagh_tls_close(fd);
            return 9;
        }
        ssize_t n = pagh_tls_read(fd, response + total, max - total);
        if (n > 0) {
            total += (size_t)n;
            if (body_at < 0) {
                body_at = head_end(response, total);
                if (body_at > 0) {
                    want = content_length(response, (size_t)body_at);
                    if (want == 0) ended = 1;
                }
            }
            if (want > 0 && body_at > 0 && total >= (size_t)body_at + (size_t)want) ended = 1;
            continue;
        }
        if (n == 0) {
            how = "close_notify";
            break;
        }
        if (errno == ECONNRESET) {
            how = "tcp close without close_notify (ECONNRESET)";
            break;
        }
        printf("tls_client: FAILED stage=read errno=%d %s\n", errno, strerror(errno));
        free(response);
        pagh_tls_close(fd);
        return 8;
    }
    /* The graceful goodbye: close_notify, then release the session. */
    if (pagh_tls_close(fd) < 0)
        printf("tls_client: pagh_tls_close: errno=%d %s\n", errno, strerror(errno));

    if (body_at <= 0) {
        printf("tls_client: FAILED stage=response: no complete HTTP head in %lu bytes\n",
               (unsigned long)total);
        free(response);
        return 8;
    }
    size_t head_len = (size_t)body_at;
    const char *body = response + head_len;
    size_t body_len = total - head_len;
    if (want >= 0 && body_len > (size_t)want) body_len = (size_t)want;

    char status[64];
    size_t status_len = 0;
    while (status_len + 1 < sizeof status && status_len < head_len &&
           response[status_len] != '\r' && response[status_len] != '\n') {
        status[status_len] = response[status_len];
        status_len++;
    }
    status[status_len] = 0;
    int chunked = head_has(response, head_len, "transfer-encoding: chunked");
    printf("tls_client: response \"%s\", %lu head bytes, stream ended by %s\n",
           status, (unsigned long)head_len, how);
    printf("tls_client: ----- body (%lu bytes%s) -----\n", (unsigned long)body_len,
           chunked ? ", chunk framing included" : "");
    if (body_len && write(1, body, body_len) != (long)body_len) {
        printf("\ntls_client: body write failed\n");
        free(response);
        return 8;
    }
    printf("\ntls_client: ----- end body -----\n");
    printf("tls_client: ok\n");
    free(response);
    return 0;
}
