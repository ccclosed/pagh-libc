#ifndef PAGH_ERRNO_H
#define PAGH_ERRNO_H
/* The errno values the native ABI can return (kernel `docs/abi.md` §3), plus
 * the socket errors that ABI adds. The numbers are Linux's, so a program that
 * checks for a specific code keeps working. */
extern int errno;
#define EPERM 1
#define ENOENT 2
#define EINTR 4
#define EIO 5
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENOSPC 28
#define EROFS 30
#define EPIPE 32
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define ELOOP 40
#define EOVERFLOW 75
#define EPROTO 71
#define ENOTSOCK 88
#define EDESTADDRREQ 89
#define EMSGSIZE 90
#define ENOPROTOOPT 92
#define ENOTSUP 95
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETUNREACH 101
#define ECONNRESET 104
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
/* The TLS session syscalls (1100..1103) return this when the server's
 * certificate was rejected — chain, SAN, clock window or `CertificateVerify`.
 * Linux's number, so a program that checks 129 keeps working. */
#define EKEYREJECTED 129
#endif
