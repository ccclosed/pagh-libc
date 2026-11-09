/* A TCP server on the native pagh ABI: socket -> setsockopt/getsockopt ->
 * bind -> listen -> accept -> recv line -> send reply -> shutdown -> close.
 *
 *   run /mnt/tcp_server [port]        (default 8080)
 *
 * The kernel has one listener at a time and a four-entry accept queue, and
 * `recv` on a stream is non-blocking: -EAGAIN means "poll again", 0 means the
 * peer closed. That contract is why the read loop below looks the way it does;
 * there is no blocking read on this ABI.
 *
 * The reply to each line is "ack:<line>", so the peer can prove the bytes made
 * a round trip. A host client reaches the guest through QEMU's
 * `-netdev user,hostfwd=tcp:127.0.0.1:HOST-:PORT` (tools/live-sockets.py does
 * exactly that). */
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#define RECV_TIMEOUT_MS 20000

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + (long)(ts.tv_nsec / 1000000);
}

/* One newline-terminated line, or 0 at end of stream, or -1 on error/timeout.
 * `out` is always NUL-terminated. */
static long recv_line(int fd, char *out, size_t cap) {
    size_t n = 0;
    long deadline = now_ms() + RECV_TIMEOUT_MS;
    while (n + 1 < cap) {
        char c;
        long r = recv(fd, &c, 1, 0);
        if (r == 1) {
            out[n++] = c;
            if (c == '\n') break;
            continue;
        }
        if (r == 0) break; /* peer closed */
        if (errno != EAGAIN) return -1;
        if (now_ms() > deadline) { errno = ETIMEDOUT; return -1; }
        usleep(2000);
    }
    out[n] = 0;
    return (long)n;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { perror("tcp_server: socket"); return 1; }

    int one = 1;
    if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) {
        perror("tcp_server: setsockopt");
        return 1;
    }
    int type = 0;
    socklen_t tlen = sizeof type;
    if (getsockopt(server, SOL_SOCKET, SO_TYPE, &type, &tlen) == 0)
        printf("tcp_server: SO_TYPE=%d (SOCK_STREAM=%d)\n", type, SOCK_STREAM);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(server, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("tcp_server: bind");
        return 1;
    }
    if (listen(server, 2) < 0) { perror("tcp_server: listen"); return 1; }
    printf("tcp_server: listening on 0.0.0.0:%d\n", port);

    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    int conn = accept(server, (struct sockaddr *)&peer, &plen);
    if (conn < 0) { perror("tcp_server: accept"); return 1; }
    char text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer.sin_addr, text, (socklen_t)sizeof text);
    printf("tcp_server: accept from %s:%u\n", text, (unsigned)ntohs(peer.sin_port));
    close(server); /* the listener has done its job */

    int rc = 0;
    for (;;) {
        char line[256];
        long n = recv_line(conn, line, sizeof line);
        if (n < 0) { perror("tcp_server: recv"); rc = 1; break; }
        if (n == 0) { printf("tcp_server: peer closed\n"); break; }
        printf("tcp_server: recv %ld bytes: %s", n, line);
        if (line[n - 1] != '\n') printf("\n");

        char reply[300];
        int rn = snprintf(reply, sizeof reply, "ack:%s", line);
        long sent = 0;
        while (sent < rn) {
            long w = send(conn, reply + sent, (size_t)(rn - sent), 0);
            if (w <= 0) { perror("tcp_server: send"); rc = 1; break; }
            sent += w;
        }
        if (rc) break;
        printf("tcp_server: sent %ld bytes\n", sent);
    }

    shutdown(conn, SHUT_WR);
    close(conn);
    if (rc == 0) printf("tcp_server: ok\n");
    return rc;
}
