/* Host-side checks for the POSIX wrapper surface added to the C library.
 *
 * These are compile-time: a struct whose size/offset or a syscall number that
 * drifts from the kernel contract in `pagh-riscv/docs/abi.md` §2 fails the host
 * test (and therefore CI) without a QEMU boot. The live behaviour is exercised
 * by examples/poll_fds.c through tools/live-sockets.py.
 *
 *   ./build/host_posix_abi_test
 */
#include <stddef.h>
#include <getopt.h>
#include <sys/time.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond) _Static_assert(cond, #cond)

/* struct pollfd — the kernel's 8-byte PollFd. */
CHECK(sizeof(struct pollfd) == 8);
CHECK(offsetof(struct pollfd, fd) == 0);
CHECK(offsetof(struct pollfd, events) == 4);
CHECK(offsetof(struct pollfd, revents) == 6);

/* stack_t — the kernel's 24-byte sigaltstack layout. */
CHECK(sizeof(stack_t) == 24);
CHECK(offsetof(stack_t, ss_sp) == 0);
CHECK(offsetof(stack_t, ss_flags) == 8);
CHECK(offsetof(stack_t, ss_size) == 16);

/* struct sigaction — 32 bytes (handler, flags, mask, restorer). */
CHECK(sizeof(struct sigaction) == 32);

/* Syscall numbers the wrappers issue (ABI §2). */
CHECK(SYS_POLL == 7);
CHECK(SYS_PPOLL == 72);
CHECK(SYS_DUP == 23);
CHECK(SYS_DUP2 == 24);
CHECK(SYS_PIPE2 == 59);
CHECK(SYS_SET_TID_ADDRESS == 96);
CHECK(SYS_SIGALTSTACK == 132);

/* Constants the headers promise. */
CHECK(POLLIN == 0x1);
CHECK(POLLOUT == 0x4);
CHECK(POLLERR == 0x8);
CHECK(POLLHUP == 0x10);
CHECK(POLLNVAL == 0x20);
CHECK(SS_ONSTACK == 1);
CHECK(SS_DISABLE == 2);

/* struct tm — the nine standard int fields, in C89 order, and no extra
 * members (this libc has no tm_gmtoff/tm_zone). struct timeval is the ABI's
 * two 64-bit fields. */
CHECK(sizeof(struct tm) == 9 * sizeof(int));
CHECK(offsetof(struct tm, tm_sec) == 0);
CHECK(offsetof(struct tm, tm_year) == 5 * sizeof(int));
CHECK(offsetof(struct tm, tm_isdst) == 8 * sizeof(int));
CHECK(sizeof(struct timeval) == 16);
CHECK(offsetof(struct timeval, tv_sec) == 0);
CHECK(offsetof(struct timeval, tv_usec) == 8);

/* struct option: four fields, in this order, and the has_arg constants. */
CHECK(sizeof(struct option) == 3 * sizeof(void *) + 2 * sizeof(int));
CHECK(offsetof(struct option, name) == 0);
CHECK(offsetof(struct option, has_arg) == sizeof(void *));
CHECK(no_argument == 0);
CHECK(required_argument == 1);
CHECK(optional_argument == 2);

/* And the signatures, so a header edit that drops or changes a prototype
 * cannot compile this test. */
static int (*const poll_fn)(struct pollfd *, nfds_t, int) = poll;
static int (*const ppoll_fn)(struct pollfd *, nfds_t, const struct timespec *,
                             const sigset_t *) = ppoll;
static int (*const dup_fn)(int) = dup;
static int (*const dup2_fn)(int, int) = dup2;
static int (*const pipe_fn)(int[2]) = pipe;
static int (*const pipe2_fn)(int[2], int) = pipe2;
static int (*const sigaltstack_fn)(const stack_t *, stack_t *) = sigaltstack;
static int (*const set_tid_address_fn)(int *) = set_tid_address;
static int (*const getopt_fn)(int, char *const[], const char *) = getopt;
static int (*const getopt_long_fn)(int, char *const[], const char *,
                                   const struct option *, int *) = getopt_long;
static void *(*const bsearch_fn)(const void *, const void *, size_t, size_t,
                                 int (*)(const void *, const void *)) = bsearch;
static char *(*const strtok_r_fn)(char *, const char *, char **) = strtok_r;
static char *(*const strsep_fn)(char **, const char *) = strsep;
static void *(*const memchr_fn)(const void *, int, size_t) = memchr;
static int (*const clock_gettime_fn)(int, struct timespec *) = clock_gettime;
static time_t (*const time_fn)(time_t *) = time;
static int (*const gettimeofday_fn)(struct timeval *, void *) = gettimeofday;
static struct tm *(*const gmtime_r_fn)(const time_t *, struct tm *) = gmtime_r;
static struct tm *(*const localtime_r_fn)(const time_t *, struct tm *) = localtime_r;
static struct tm *(*const gmtime_fn)(const time_t *) = gmtime;
static size_t (*const strftime_fn)(char *, size_t, const char *, const struct tm *) = strftime;

int main(void) {
    /* Keep the prototype checks "used" without calling anything. */
    const void *checked[] = {
        (const void *)&poll_fn, (const void *)&ppoll_fn, (const void *)&dup_fn,
        (const void *)&dup2_fn, (const void *)&pipe_fn, (const void *)&pipe2_fn,
        (const void *)&sigaltstack_fn, (const void *)&set_tid_address_fn,
        (const void *)&getopt_fn, (const void *)&getopt_long_fn,
        (const void *)&bsearch_fn, (const void *)&strtok_r_fn,
        (const void *)&strsep_fn, (const void *)&memchr_fn,
        (const void *)&clock_gettime_fn, (const void *)&time_fn,
        (const void *)&gettimeofday_fn, (const void *)&gmtime_r_fn,
        (const void *)&localtime_r_fn, (const void *)&gmtime_fn,
        (const void *)&strftime_fn,
    };
    (void)checked;
    printf("posix wrappers: ok (pollfd %d B, stack_t %d B, sigaction %d B, "
           "option %d B, tm %d B, timeval %d B, 21 prototypes)\n",
           (int)sizeof(struct pollfd), (int)sizeof(stack_t),
           (int)sizeof(struct sigaction), (int)sizeof(struct option),
           (int)sizeof(struct tm), (int)sizeof(struct timeval));
    return 0;
}