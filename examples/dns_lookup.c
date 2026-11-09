/* A DNS client on the native pagh ABI: gethostbyname (one A record over UDP)
 * plus the getaddrinfo-lite surface.
 *
 *   run /mnt/dns_lookup name [name...]
 *   run /mnt/dns_lookup @10.0.2.2:9053 name      (pin the server)
 *
 * With no `@server`, the server comes from `nameserver A.B.C.D[:port]` in
 * /etc/resolv.conf or /mnt/etc/resolv.conf, which is how the DHCP-provided
 * address reaches userland (`net` in the shell prints the lease). The example
 * prints the server it used, so a transcript says which server answered rather
 * than implying localhost magic.
 *
 * Exit status is the number of names that failed, capped at 1. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: dns_lookup [@server[:port]] name [name...]\n");
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; i++) {
        const char *name = argv[i];
        if (name[0] == '@') {
            if (pagh_resolv_set_server(name + 1) < 0) {
                printf("dns: %s: bad server (errno=%d %s)\n", name + 1, errno, strerror(errno));
                return 2;
            }
            printf("dns: pinned server %s\n", name + 1);
            continue;
        }
        char server[64];
        if (pagh_resolv_server(server, (socklen_t)sizeof server) == 0)
            printf("dns: server %s (from %s)\n", server, pagh_resolv_source());
        else
            printf("dns: no server configured (errno=%d %s)\n", errno, strerror(errno));

        struct hostent *he = gethostbyname(name);
        if (!he) {
            printf("dns: %s: FAILED: %s (h_errno=%d)\n", name, hstrerror(h_errno), h_errno);
            failures++;
            continue;
        }
        char addr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, he->h_addr_list[0], addr, (socklen_t)sizeof addr);
        printf("dns: gethostbyname %s -> %s (type=%d len=%d)\n",
               he->h_name, addr, he->h_addrtype, he->h_length);

        struct addrinfo hints;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = 0;
        int rc = getaddrinfo(name, "http", &hints, &res);
        if (rc != 0) {
            printf("dns: getaddrinfo %s: %s (EAI=%d)\n", name, gai_strerror(rc), rc);
            failures++;
            continue;
        }
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sin->sin_addr, text, (socklen_t)sizeof text);
        printf("dns: getaddrinfo %s http -> %s:%u family=%d socktype=%d addrlen=%u\n",
               name, text, (unsigned)ntohs(sin->sin_port), res->ai_family,
               res->ai_socktype, (unsigned)res->ai_addrlen);
        freeaddrinfo(res);
    }
    if (failures) {
        printf("dns: %d lookup(s) failed\n", failures);
        return 1;
    }
    printf("dns: ok\n");
    return 0;
}
