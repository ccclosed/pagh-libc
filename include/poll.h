#ifndef PAGH_POLL_H
#define PAGH_POLL_H
#include <pagh.h>
#include <signal.h> /* sigset_t, for ppoll */
#include <time.h>   /* struct timespec, for ppoll */

/* One descriptor watched by poll(2): the kernel's 8-byte `PollFd`
 * (pagh-riscv docs/abi.md §2). A negative `fd` is skipped and leaves
 * `revents` 0. */
struct pollfd {
    int fd;
    short events;
    short revents;
};
typedef unsigned long nfds_t;

#define POLLIN 0x1
#define POLLOUT 0x4
#define POLLERR 0x8
#define POLLHUP 0x10
#define POLLNVAL 0x20

/* Wait for events on up to 64 descriptors. `timeout` is milliseconds: 0 scans
 * once, a negative value waits without a deadline, and a 0 return is a timeout
 * (not an error). Returns the number of ready descriptors, or -1 with errno. */
int poll(struct pollfd *fds, nfds_t nfds, int timeout);

/* As poll, with a kernel-ABI timespec (the kernel converts it to milliseconds)
 * instead of an int. The ABI documents `sigmask` but does not read it; this
 * wrapper accepts and ignores it, like the kernel. */
int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout,
          const sigset_t *sigmask);

#endif