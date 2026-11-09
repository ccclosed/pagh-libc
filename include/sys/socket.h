#ifndef PAGH_SYS_SOCKET_H
#define PAGH_SYS_SOCKET_H
/* The native pagh socket ABI (kernel >= 0.17.0 for UDP, >= 0.18.0 for TCP
 * listen/accept), BSD-shaped.
 *
 * This header is the single source for the address types as well as the calls:
 * <netinet/in.h> and <arpa/inet.h> are one-line wrappers around it, so any
 * include order works. The C library is one small file set, not glibc.
 *
 * Kernel contract (see the kernel's `docs/abi.md`): `struct sockaddr_in` is
 * exactly 16 bytes, family little-endian, port **big-endian**, address four
 * raw bytes. `bind`/`connect` require exactly 16; `accept`/`recvfrom` fill the
 * peer address and set the length to 16. */
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

#define AF_UNSPEC 0
#define AF_INET 2
#define PF_INET AF_INET

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
/* The kernel ORs this into the type; there is no fcntl(F_SETFL) in this ABI. */
#define SOCK_NONBLOCK 0x800
/* Declared for source compatibility only: the kernel does not know this bit,
 * so a socket() carrying it is refused with EAFNOSUPPORT rather than silently
 * accepted without close-on-exec. */
#define SOCK_CLOEXEC 0x80000

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_KEEPALIVE 9
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21

#define IPPROTO_IP 0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define TCP_NODELAY 1

/* Only MSG_DONTWAIT is understood, and only on a datagram socket. Any other
 * flag is EINVAL, and a stream send/recv must pass 0. */
#define MSG_DONTWAIT 0x40

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

/* Same value and layout as glibc's INADDR_ANY (network byte order, 0). */
#define INADDR_ANY ((in_addr_t)0x00000000u)
#define INADDR_BROADCAST ((in_addr_t)0xffffffffu)

struct in_addr {
    in_addr_t s_addr; /* network byte order */
};

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

/* Exactly the kernel's 16-byte layout: family, big-endian port, 4 address
 * bytes, 8 bytes of padding the kernel never reads. */
struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;      /* big-endian */
    struct in_addr sin_addr; /* 4 bytes, network order */
    unsigned char sin_zero[8];
};

/* Byte order. RISC-V is little-endian; these are written portably anyway. */
uint16_t htons(uint16_t hostshort);
uint16_t ntohs(uint16_t netshort);
uint32_t htonl(uint32_t hostlong);
uint32_t ntohl(uint32_t netlong);

/* Text <-> binary IPv4. `inet_aton`/`inet_pton` accept the dotted-quad form
 * only. `inet_ntoa` returns a pointer into a static buffer (not thread-safe);
 * `inet_ntop` writes into the caller's buffer, which must hold at least
 * INET_ADDRSTRLEN bytes. */
#define INET_ADDRSTRLEN 16
int inet_aton(const char *cp, struct in_addr *inp);
in_addr_t inet_addr(const char *cp);
char *inet_ntoa(struct in_addr in);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int inet_pton(int af, const char *src, void *dst);

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
ssize_t send(int fd, const void *buf, size_t n, int flags);
ssize_t sendto(int fd, const void *buf, size_t n, int flags,
               const struct sockaddr *dest, socklen_t len);
ssize_t recv(int fd, void *buf, size_t n, int flags);
ssize_t recvfrom(int fd, void *buf, size_t n, int flags,
                 struct sockaddr *src, socklen_t *len);
int setsockopt(int fd, int level, int name, const void *value, socklen_t len);
int getsockopt(int fd, int level, int name, void *value, socklen_t *len);
int shutdown(int fd, int how);

#endif
