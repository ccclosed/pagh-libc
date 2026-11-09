/* The DNS client: configuration, the UDP exchange and the getaddrinfo-lite
 * surface. The wire format lives in src/dns.c and is unit-tested on the host;
 * everything here needs sockets, so it is exercised by the live test
 * (tools/live-sockets.py) against a host peer and against the DHCP server.
 *
 * Design notes worth stating:
 *   * one datagram per attempt, three attempts, a 2 s wait each — a DNS server
 *     that answers at all answers in milliseconds, and the kernel's own UDP
 *     receive already blocks;
 *   * a reply is only accepted from the server address the query went to, with
 *     the query's transaction id and question name (so a stray or forged
 *     datagram cannot move the answer), and anything else is ignored while the
 *     wait continues;
 *   * there is no cache and no CNAME chasing: a CNAME answer is a "no A
 *     record" and the caller has to ask for the target itself. */
#include <netdb.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <pagh_internal.h>

#define DNS_PORT_DEFAULT 53
#define DNS_REPLY_MAX 512
#define DNS_ATTEMPTS 3
#define DNS_SPINS 200     /* 200 * 10 ms = 2 s per attempt */
#define DNS_SPIN_US 10000

int h_errno;

static int pinned;
static unsigned char pinned_ip[4];
static unsigned int pinned_port = DNS_PORT_DEFAULT;
static const char *resolv_source = "none";

/* ── configuration ───────────────────────────────────────────────────────── */

/* Dotted quad at the front of `text`; returns the first byte past it. */
static int parse_ipv4_prefix(const char *text, const char **end, unsigned char out[4]) {
    const char *p = text;
    for (int i = 0; i < 4; i++) {
        int value = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p++ - '0');
            if (++digits > 3 || value > 255) return 0;
        }
        if (!digits) return 0;
        out[i] = (unsigned char)value;
        if (i < 3) {
            if (*p != '.') return 0;
            p++;
        }
    }
    if (end) *end = p;
    return 1;
}

static int parse_server(const char *spec, unsigned char ip[4], unsigned int *port) {
    const char *end = spec;
    if (!parse_ipv4_prefix(spec, &end, ip)) return 0;
    *port = DNS_PORT_DEFAULT;
    if (*end == ':') {
        const char *digits = end + 1;
        unsigned long value = 0;
        int count = 0;
        while (*digits >= '0' && *digits <= '9' && count < 5) {
            value = value * 10 + (unsigned long)(*digits++ - '0');
            count++;
        }
        if (count == 0 || value < 1 || value > 65535) return 0;
        *port = (unsigned int)value;
        end = digits;
    }
    if (*end) return 0;
    return 1;
}

static int resolv_server(unsigned char ip[4], unsigned int *port);

int pagh_resolv_set_server(const char *spec) {
    unsigned char ip[4];
    unsigned int port;
    if (!spec || !parse_server(spec, ip, &port)) { errno = EINVAL; return -1; }
    memcpy(pinned_ip, ip, 4);
    pinned_port = port;
    pinned = 1;
    resolv_source = "pinned";
    return 0;
}

int pagh_resolv_server(char *buf, socklen_t size) {
    unsigned char ip[4];
    unsigned int port;
    if (!resolv_server(ip, &port)) { errno = ENOENT; return -1; }
    char text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, ip, text, (socklen_t)sizeof text);
    snprintf(buf, (size_t)size, "%s:%u", text, port);
    return 0;
}

const char *pagh_resolv_source(void) { return resolv_source; }

static int read_resolv_file(const char *path, unsigned char ip[4], unsigned int *port) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int found = 0;
    while (!found && fgets(line, (int)sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "nameserver", 10) != 0) continue;
        p += 10;
        while (*p == ' ' || *p == '\t') p++;
        const char *end = p;
        if (!parse_ipv4_prefix(p, &end, ip)) continue;
        *port = DNS_PORT_DEFAULT;
        if (*end == ':') {
            const char *digits = end + 1;
            unsigned long value = 0;
            int count = 0;
            while (*digits >= '0' && *digits <= '9' && count < 5) {
                value = value * 10 + (unsigned long)(*digits++ - '0');
                count++;
            }
            if (count == 0 || value < 1 || value > 65535) continue;
            *port = (unsigned int)value;
            end = digits;
        }
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
        if (*end && *end != '#') continue; /* trailing junk: not a nameserver line */
        found = 1;
    }
    fclose(f);
    return found;
}

/* The address of the server the next lookup will ask. */
static int resolv_server(unsigned char ip[4], unsigned int *port) {
    if (pinned) {
        memcpy(ip, pinned_ip, 4);
        *port = pinned_port;
        return 1;
    }
    /* The ABI has no way to read the DHCP lease, so the file is the only path
     * from "the kernel leased 10.0.2.3 as DNS" to userland. /mnt is where pagh
     * mounts its ext2 volume; /etc comes first so a future root filesystem
     * takes precedence. */
    if (read_resolv_file("/etc/resolv.conf", ip, port)) {
        resolv_source = "/etc/resolv.conf";
        return 1;
    }
    if (read_resolv_file("/mnt/etc/resolv.conf", ip, port)) {
        resolv_source = "/mnt/etc/resolv.conf";
        return 1;
    }
    resolv_source = "none";
    return 0;
}

/* ── the query ───────────────────────────────────────────────────────────── */

static uint32_t id_counter;

static uint16_t next_id(void) {
    struct timespec ts;
    uint32_t nanos = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) nanos = (uint32_t)ts.tv_nsec;
    id_counter += 0x9e37u;
    return (uint16_t)(nanos ^ id_counter ^ (uint32_t)getpid() * 2654435761u);
}

static int same_address(const struct sockaddr_in *a, const unsigned char ip[4], unsigned int port) {
    const unsigned char *bytes = (const unsigned char *)&a->sin_addr.s_addr;
    return bytes[0] == ip[0] && bytes[1] == ip[1] && bytes[2] == ip[2] && bytes[3] == ip[3] &&
           ntohs(a->sin_port) == (uint16_t)port;
}

/* 1 = an A record, 0 = a well-formed answer without one, -1 = no usable answer,
 * -2 = the server answered with an error code. */
static int dns_query(const char *name, const unsigned char server[4], unsigned int port,
                     unsigned char addr[4], int *rcode) {
    unsigned char query[512];
    unsigned char reply[DNS_REPLY_MAX];
    uint16_t id = next_id();
    int qlen = __pagh_dns_build_query(name, id, query, sizeof query);
    if (qlen <= 0) return -1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)port);
    memcpy(&to.sin_addr.s_addr, server, 4);

    int status = -1;
    for (int attempt = 0; attempt < DNS_ATTEMPTS; attempt++) {
        if (sendto(fd, query, (size_t)qlen, 0, (struct sockaddr *)&to, sizeof to) < 0) continue;
        for (int spin = 0; spin < DNS_SPINS; spin++) {
            struct sockaddr_in from;
            socklen_t fromlen = sizeof from;
            long n = recvfrom(fd, reply, sizeof reply, MSG_DONTWAIT,
                              (struct sockaddr *)&from, &fromlen);
            if (n > 0) {
                if (same_address(&from, server, port)) {
                    status = __pagh_dns_parse_a(reply, (int)n, id, name, addr, rcode);
                    /* A datagram that fails the id/question check may be a
                     * stray; keep waiting for the real answer. Anything the
                     * server itself said is final. */
                    if (status != PAGH_DNS_BAD) goto done;
                }
            } else if (n < 0 && errno != EAGAIN) {
                break;
            }
            usleep(DNS_SPIN_US);
        }
        if (status != -1 && status != PAGH_DNS_BAD) break;
    }
done:
    close(fd);
    return status;
}

/* ── gethostbyname ───────────────────────────────────────────────────────── */

static char gh_name[256];
static unsigned char gh_addr[4];
static char *gh_addr_list[2];
static char *gh_aliases[1];
static struct hostent gh;

struct hostent *gethostbyname(const char *name) {
    if (!name || !*name) { h_errno = NO_RECOVERY; return 0; }
    struct in_addr numeric;
    int status;
    if (inet_aton(name, &numeric)) {
        memcpy(gh_addr, &numeric.s_addr, 4);
    } else {
        unsigned char server[4];
        unsigned int port;
        if (!resolv_server(server, &port)) { h_errno = NO_RECOVERY; return 0; }
        int rcode = 0;
        status = dns_query(name, server, port, gh_addr, &rcode);
        if (status != PAGH_DNS_OK) {
            h_errno = status == PAGH_DNS_NAME_ERROR ? HOST_NOT_FOUND
                    : status == PAGH_DNS_NO_ANSWER ? NO_DATA
                                                   : TRY_AGAIN;
            return 0;
        }
    }
    size_t n = strlen(name);
    if (n >= sizeof gh_name) n = sizeof gh_name - 1;
    memcpy(gh_name, name, n);
    gh_name[n] = 0;
    gh_aliases[0] = 0;
    gh_addr_list[0] = (char *)gh_addr;
    gh_addr_list[1] = 0;
    gh.h_name = gh_name;
    gh.h_aliases = gh_aliases;
    gh.h_addrtype = AF_INET;
    gh.h_length = 4;
    gh.h_addr_list = gh_addr_list;
    return &gh;
}

/* ── getaddrinfo-lite ────────────────────────────────────────────────────── */

struct service_entry { const char *name; unsigned short port; };
static const struct service_entry services[] = {
    { "ftp", 21 }, { "ssh", 22 }, { "telnet", 23 }, { "smtp", 25 },
    { "domain", 53 }, { "http", 80 }, { "pop3", 110 }, { "ntp", 123 },
    { "imap", 143 }, { "https", 443 },
};

static int service_port(const char *service, int flags, unsigned short *port) {
    *port = 0;
    if (!service || !*service) return 0;
    const char *p = service;
    unsigned long value = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (unsigned long)(*p++ - '0');
        if (++digits > 5) return EAI_SERVICE;
    }
    if (digits && !*p) {
        if (value > 65535) return EAI_SERVICE;
        *port = (unsigned short)value;
        return 0;
    }
    if (flags & AI_NUMERICSERV) return EAI_SERVICE;
    for (unsigned i = 0; i < sizeof services / sizeof services[0]; i++) {
        if (strcmp(services[i].name, service) == 0) {
            *port = services[i].port;
            return 0;
        }
    }
    return EAI_SERVICE;
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    if (!res) return EAI_FAIL;
    *res = 0;
    int flags = hints ? hints->ai_flags : 0;
    if (flags & ~(AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV)) return EAI_BADFLAGS;
    int family = hints ? hints->ai_family : AF_UNSPEC;
    if (family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
    int socktype = hints ? hints->ai_socktype : 0;
    if (socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM) return EAI_SOCKTYPE;
    int protocol = hints ? hints->ai_protocol : 0;

    unsigned short port;
    int sr = service_port(service, flags, &port);
    if (sr) return sr;

    unsigned char ip[4] = { 0, 0, 0, 0 };
    if (!node) {
        if (!(flags & AI_PASSIVE)) { ip[0] = 127; ip[3] = 1; }
    } else {
        struct in_addr numeric;
        if (inet_aton(node, &numeric)) {
            memcpy(ip, &numeric.s_addr, 4);
        } else if (flags & AI_NUMERICHOST) {
            return EAI_NONAME;
        } else {
            struct hostent *he = gethostbyname(node);
            if (!he) return h_errno == TRY_AGAIN ? EAI_AGAIN : EAI_NONAME;
            memcpy(ip, he->h_addr_list[0], 4);
        }
    }

    struct addrinfo *ai = calloc(1, sizeof *ai);
    struct sockaddr_in *sin = calloc(1, sizeof *sin);
    if (!ai || !sin) { free(ai); free(sin); return EAI_MEMORY; }
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    memcpy(&sin->sin_addr.s_addr, ip, 4);
    ai->ai_flags = flags;
    ai->ai_family = AF_INET;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_addrlen = sizeof *sin;
    ai->ai_addr = (struct sockaddr *)sin;
    if (flags & AI_CANONNAME) {
        ai->ai_canonname = strdup(node ? node : "localhost");
        if (!ai->ai_canonname) { free(sin); free(ai); return EAI_MEMORY; }
    }
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_canonname);
        free(res->ai_addr);
        free(res);
        res = next;
    }
}

const char *gai_strerror(int err) {
    switch (err) {
    case 0: return "success";
    case EAI_BADFLAGS: return "bad flags";
    case EAI_NONAME: return "name or service not known";
    case EAI_AGAIN: return "temporary failure in name resolution";
    case EAI_FAIL: return "non-recoverable failure in name resolution";
    case EAI_FAMILY: return "address family not supported";
    case EAI_SOCKTYPE: return "socket type not supported";
    case EAI_SERVICE: return "service not supported";
    case EAI_MEMORY: return "out of memory";
    case EAI_SYSTEM: return "system error";
    case EAI_OVERFLOW: return "buffer overflow";
    default: return "unknown error";
    }
}

const char *hstrerror(int err) {
    switch (err) {
    case 0: return "success";
    case HOST_NOT_FOUND: return "host not found";
    case TRY_AGAIN: return "try again";
    case NO_RECOVERY: return "non-recoverable error";
    case NO_DATA: return "no address associated with name";
    default: return "unknown error";
    }
}
