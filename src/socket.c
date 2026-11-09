#include <pagh.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>

static long ret(long r) { if (r < 0) { errno = (int)-r; return -1; } return r; }

/* Byte order. These are plain shifts, so the result does not depend on the
 * host's endianness or on the compiler's builtins. */
uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (uint16_t)(v << 8)); }
uint16_t ntohs(uint16_t v) { return htons(v); }
uint32_t htonl(uint32_t v) {
    return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
           ((v & 0x00ff0000u) >> 8) | ((v & 0xff000000u) >> 24);
}
uint32_t ntohl(uint32_t v) { return htonl(v); }

/* Dotted quad only, one to four digits per part, no sign, no trailing junk. */
int inet_aton(const char *cp, struct in_addr *inp) {
    unsigned char bytes[4];
    const char *p = cp;
    if (!p) return 0;
    for (int i = 0; i < 4; i++) {
        int value = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p++ - '0');
            if (++digits > 3 || value > 255) return 0;
        }
        if (!digits) return 0;
        bytes[i] = (unsigned char)value;
        if (i < 3) {
            if (*p != '.') return 0;
            p++;
        }
    }
    if (*p) return 0;
    memcpy(&inp->s_addr, bytes, 4);
    return 1;
}

in_addr_t inet_addr(const char *cp) {
    struct in_addr in;
    if (!inet_aton(cp, &in)) return INADDR_BROADCAST; /* POSIX: (in_addr_t)-1 */
    return in.s_addr;
}

char *inet_ntoa(struct in_addr in) {
    static char text[16];
    if (!inet_ntop(AF_INET, &in.s_addr, text, (socklen_t)sizeof text)) return 0;
    return text;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (af != AF_INET) { errno = EAFNOSUPPORT; return 0; }
    if (size < 16) { errno = ENOSPC; return 0; }
    const unsigned char *b = src;
    /* Four calls into a caller-sized buffer: snprintf is bounded, and the
     * result is at most 15 characters plus the terminator. */
    char *out = dst;
    int left = (int)size;
    for (int i = 0; i < 4; i++) {
        int n = 0;
        unsigned int v = b[i];
        char digits[3];
        do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) { *out++ = digits[--n]; left--; }
        if (i < 3) { *out++ = '.'; left--; }
    }
    *out = 0;
    (void)left;
    return dst;
}

int inet_pton(int af, const char *src, void *dst) {
    if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    struct in_addr in;
    if (!inet_aton(src, &in)) return 0;
    memcpy(dst, &in.s_addr, 4);
    return 1;
}

int socket(int domain, int type, int protocol) {
    return (int)ret(__pagh_syscall3(SYS_SOCKET, domain, type, protocol));
}
int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    return (int)ret(__pagh_syscall3(SYS_BIND, fd, (long)addr, len));
}
int listen(int fd, int backlog) {
    return (int)ret(__pagh_syscall3(SYS_LISTEN, fd, backlog, 0));
}
int accept(int fd, struct sockaddr *addr, socklen_t *len) {
    return (int)ret(__pagh_syscall3(SYS_ACCEPT, fd, (long)addr, (long)len));
}
int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    return (int)ret(__pagh_syscall3(SYS_CONNECT, fd, (long)addr, len));
}
ssize_t send(int fd, const void *buf, size_t n, int flags) {
    return ret(__pagh_syscall6(SYS_SENDTO, fd, (long)buf, (long)n, flags, 0, 0));
}
ssize_t sendto(int fd, const void *buf, size_t n, int flags,
               const struct sockaddr *dest, socklen_t len) {
    return ret(__pagh_syscall6(SYS_SENDTO, fd, (long)buf, (long)n, flags,
                               (long)dest, len));
}
ssize_t recv(int fd, void *buf, size_t n, int flags) {
    return ret(__pagh_syscall6(SYS_RECVFROM, fd, (long)buf, (long)n, flags, 0, 0));
}
ssize_t recvfrom(int fd, void *buf, size_t n, int flags,
                 struct sockaddr *src, socklen_t *len) {
    return ret(__pagh_syscall6(SYS_RECVFROM, fd, (long)buf, (long)n, flags,
                               (long)src, (long)len));
}
int setsockopt(int fd, int level, int name, const void *value, socklen_t len) {
    return (int)ret(__pagh_syscall6(SYS_SETSOCKOPT, fd, level, name, (long)value, len, 0));
}
int getsockopt(int fd, int level, int name, void *value, socklen_t *len) {
    return (int)ret(__pagh_syscall6(SYS_GETSOCKOPT, fd, level, name, (long)value, (long)len, 0));
}
int shutdown(int fd, int how) {
    return (int)ret(__pagh_syscall3(SYS_SHUTDOWN, fd, how, 0));
}
