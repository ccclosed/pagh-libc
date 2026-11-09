#ifndef PAGH_NETDB_H
#define PAGH_NETDB_H
/* A minimal name resolver: one A-record query over UDP, no search list, no
 * IPv6, no NSS. It is the client-side half of the kernel's own DNS stub, with
 * none of the kernel's privileges: everything here goes through the ordinary
 * UDP socket syscalls, so an unprivileged program resolves names exactly the
 * way it sends a datagram.
 *
 * Server selection, in order:
 *   1. a pin set with `pagh_resolv_set_server()` (pagh extension);
 *   2. `nameserver A.B.C.D[:port]` in `/etc/resolv.conf`;
 *   3. the same line in `/mnt/etc/resolv.conf` (pagh mounts its ext2 volume at
 *      /mnt, so this is where a disk image usually puts the file).
 * The ABI exposes no way to read the DHCP lease, so the file is how the
 * resolver learns the DHCP-provided server; the `:port` suffix is a pagh
 * extension for tests that must run a server above 1024 (the default is 53).
 *
 * `gethostbyname` returns a pointer to storage that the next call overwrites
 * and is not thread-safe, like every resolver of its era; `getaddrinfo`
 * allocates and `freeaddrinfo` releases. */
#include <sys/socket.h>
#include <sys/types.h>

struct hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype;
    int h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE 0x0001
#define AI_CANONNAME 0x0002
#define AI_NUMERICHOST 0x0004
#define AI_NUMERICSERV 0x0400

/* getaddrinfo error codes (the glibc values, so a caller can compare). */
#define EAI_BADFLAGS (-1)
#define EAI_NONAME (-2)
#define EAI_AGAIN (-3)
#define EAI_FAIL (-4)
#define EAI_FAMILY (-6)
#define EAI_SOCKTYPE (-7)
#define EAI_SERVICE (-8)
#define EAI_MEMORY (-10)
#define EAI_SYSTEM (-11)
#define EAI_OVERFLOW (-12)

/* gethostbyname error codes. */
#define HOST_NOT_FOUND 1
#define TRY_AGAIN 2
#define NO_RECOVERY 3
#define NO_DATA 4
#define NO_ADDRESS NO_DATA

extern int h_errno;

struct hostent *gethostbyname(const char *name);
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int err);
const char *hstrerror(int err);

/* pagh extension: pin the resolver to `spec` = "A.B.C.D" or "A.B.C.D:port".
 * Returns 0 on success; -1 with errno = EINVAL on a malformed address. */
int pagh_resolv_set_server(const char *spec);

/* pagh extension: what the next lookup would ask, as "A.B.C.D:port", and where
 * that setting came from ("pinned", "/etc/resolv.conf", "/mnt/etc/resolv.conf"
 * or "none"). `pagh_resolv_server` returns 0, or -1 with errno = ENOENT when
 * nothing is configured. */
int pagh_resolv_server(char *buf, socklen_t size);
const char *pagh_resolv_source(void);

#endif
