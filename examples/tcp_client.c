/* A TCP client on the native pagh ABI: socket -> connect -> getsockopt(SO_ERROR)
 * -> send -> recv -> shutdown -> close.
 *
 *   run /mnt/tcp_client <host> <port> [message]
 *
 * The peer is expected to echo the message back, which is what proves both
 * directions instead of just "send returned a number". QEMU's user network
 * maps the guest's 10.0.2.2 to the host's loopback, so
 *   python3 tools/host_peer.py ...
 * on the host is what the guest connects to in tools/live-sockets.py. */
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

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: tcp_client <host> <port> [message]\n");
        return 2;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *message = argc > 3 ? argv[3] : "pagh tcp client";
    size_t want = strlen(message);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("tcp_client: socket"); return 1; }
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)port);
    if (!inet_aton(host, &to.sin_addr)) {
        printf("tcp_client: %s: not an IPv4 address\n", host);
        return 2;
    }
    printf("tcp_client: connecting to %s:%d\n", host, port);
    if (connect(fd, (struct sockaddr *)&to, sizeof to) < 0) {
        perror("tcp_client: connect");
        return 1;
    }
    printf("tcp_client: connected\n");

    int error = -1;
    socklen_t elen = sizeof error;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &elen) == 0)
        printf("tcp_client: SO_ERROR=%d\n", error);

    long sent = 0;
    while (sent < (long)want) {
        long w = send(fd, message + sent, want - (size_t)sent, 0);
        if (w <= 0) { perror("tcp_client: send"); return 1; }
        sent += w;
    }
    printf("tcp_client: sent %ld bytes\n", sent);

    char echo[512];
    size_t got = 0;
    long deadline = now_ms() + RECV_TIMEOUT_MS;
    while (got < sizeof echo - 1) {
        long r = recv(fd, echo + got, sizeof echo - 1 - got, 0);
        if (r > 0) { got += (size_t)r; if (got >= want) break; continue; }
        if (r == 0) break;
        if (errno != EAGAIN) { perror("tcp_client: recv"); return 1; }
        if (now_ms() > deadline) { printf("tcp_client: recv timeout\n"); return 1; }
        usleep(2000);
    }
    echo[got] = 0;
    printf("tcp_client: echo %lu bytes: %s\n", (unsigned long)got, echo);

    shutdown(fd, SHUT_WR);
    close(fd);
    int ok = got == want && memcmp(echo, message, want) == 0;
    printf(ok ? "tcp_client: ok\n" : "tcp_client: echo mismatch\n");
    return ok ? 0 : 1;
}
