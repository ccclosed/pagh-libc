/* UDP echo round trips on the native pagh ABI: optional bind -> N x
 * (sendto -> recvfrom) with the payload and the source address checked.
 *
 *   run /mnt/udp_echo <host> <port> [count] [local-port]
 *
 * The host peer is a UDP echo server (tools/host_peer.py); QEMU's user network
 * maps the guest's 10.0.2.2<port> to the host's 127.0.0.1<port>, which is how
 * the datagrams reach it. A guest-bound port cannot be opened from the host
 * without a udp hostfwd, so the guest starts every round trip -- the reply is
 * what proves recvfrom.
 *
 * The kernel needs a destination on every sendto (no UDP connect) and its
 * recvfrom blocks for up to ~10 s before -EAGAIN; MSG_DONTWAIT would make it a
 * single poll, which this example does not need. */
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: udp_echo <host> <port> [count] [local-port]\n");
        return 2;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    int count = argc > 3 ? atoi(argv[3]) : 3;
    int local_port = argc > 4 ? atoi(argv[4]) : 0;
    if (count < 1 || count > 10) count = 3;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("udp_echo: socket"); return 1; }

    int type = 0;
    socklen_t tlen = sizeof type;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tlen) == 0)
        printf("udp_echo: SO_TYPE=%d (SOCK_DGRAM=%d)\n", type, SOCK_DGRAM);

    if (local_port > 0) {
        struct sockaddr_in me;
        memset(&me, 0, sizeof me);
        me.sin_family = AF_INET;
        me.sin_port = htons((unsigned short)local_port);
        me.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *)&me, sizeof me) < 0) {
            perror("udp_echo: bind");
            return 1;
        }
        printf("udp_echo: bound 0.0.0.0:%d\n", local_port);
    } else {
        printf("udp_echo: unbound (the first sendto picks a port)\n");
    }

    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)port);
    if (!inet_aton(host, &to.sin_addr)) {
        printf("udp_echo: %s: not an IPv4 address\n", host);
        return 2;
    }

    int failures = 0;
    for (int i = 1; i <= count; i++) {
        char payload[64];
        int n = snprintf(payload, sizeof payload, "pagh udp echo #%d", i);
        long sent = sendto(fd, payload, (size_t)n, 0, (struct sockaddr *)&to, sizeof to);
        if (sent != n) {
            perror("udp_echo: sendto");
            return 1;
        }
        printf("udp_echo: sent %ld bytes to %s:%d: %s\n", sent, host, port, payload);

        char reply[256];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof from;
        long got = recvfrom(fd, reply, sizeof reply - 1, 0,
                            (struct sockaddr *)&from, &fromlen);
        if (got < 0) {
            perror("udp_echo: recvfrom");
            failures++;
            continue;
        }
        reply[got] = 0;
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, text, (socklen_t)sizeof text);
        printf("udp_echo: recv %ld bytes from %s:%u: %s\n",
               got, text, (unsigned)ntohs(from.sin_port), reply);
        if (got != n || memcmp(reply, payload, (size_t)n) != 0 ||
            ntohs(from.sin_port) != (unsigned short)port) {
            printf("udp_echo: datagram %d mismatch\n", i);
            failures++;
        }
    }

    close(fd);
    if (failures) {
        printf("udp_echo: %d/%d datagrams failed\n", failures, count);
        return 1;
    }
    printf("udp_echo: ok (%d datagrams echoed)\n", count);
    return 0;
}
