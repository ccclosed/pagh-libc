#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

const char *strerror(int err) {
    switch (err) {
    case 0: return "success";
    case EPERM: return "operation not permitted";
    case ENOENT: return "no such file or directory";
    case EINTR: return "interrupted system call";
    case EIO: return "input/output error";
    case EBADF: return "bad file descriptor";
    case EAGAIN: return "resource temporarily unavailable";
    case ENOMEM: return "cannot allocate memory";
    case EACCES: return "permission denied";
    case EFAULT: return "bad address";
    case EEXIST: return "file exists";
    case ENOTDIR: return "not a directory";
    case EISDIR: return "is a directory";
    case EINVAL: return "invalid argument";
    case ENOSPC: return "no space left on device";
    case EROFS: return "read-only file system";
    case EPIPE: return "broken pipe";
    case ERANGE: return "numerical result out of range";
    case ENAMETOOLONG: return "file name too long";
    case ENOSYS: return "function not implemented";
    case ENOTEMPTY: return "directory not empty";
    case ELOOP: return "too many levels of symbolic links";
    case EOVERFLOW: return "value too large for defined data type";
    case ENOTSOCK: return "socket operation on non-socket";
    case EDESTADDRREQ: return "destination address required";
    case EMSGSIZE: return "message too long";
    case ENOPROTOOPT: return "protocol not available";
    case ENOTSUP: return "operation not supported";
    case EAFNOSUPPORT: return "address family not supported by protocol";
    case EADDRINUSE: return "address already in use";
    case EADDRNOTAVAIL: return "cannot assign requested address";
    case ENETUNREACH: return "network is unreachable";
    case ECONNRESET: return "connection reset by peer";
    case EISCONN: return "transport endpoint is already connected";
    case ENOTCONN: return "transport endpoint is not connected";
    case ETIMEDOUT: return "connection timed out";
    case ECONNREFUSED: return "connection refused";
    case EHOSTUNREACH: return "no route to host";
    default: return "unknown error";
    }
}

void perror(const char *s) {
    if (s && *s) {
        write(2, s, strlen(s));
        write(2, ": ", 2);
    }
    const char *text = strerror(errno);
    write(2, text, strlen(text));
    write(2, "\n", 1);
}
