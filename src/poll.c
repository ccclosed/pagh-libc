#include <errno.h>
#include <pagh.h>
#include <poll.h>

/* poll(2): the kernel waits up to 64 descriptors and copies every entry's
 * revents back before returning. A negative timeout waits indefinitely; a 0
 * return is a timeout, which is not an error. */
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    long r = __pagh_syscall6(SYS_POLL, (long)fds, (long)nfds, timeout, 0, 0, 0);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return (int)r;
}

/* ppoll(2): the ABI's version takes a timespec that the kernel turns into a
 * millisecond timeout; its fourth argument (a sigmask) is declared but never
 * read, so it is ignored here as well. */
int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout,
          const sigset_t *sigmask) {
    (void)sigmask;
    long r = __pagh_syscall6(SYS_PPOLL, (long)fds, (long)nfds, (long)timeout, 0, 0, 0);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return (int)r;
}