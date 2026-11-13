#ifndef PAGH_SYS_TIME_H
#define PAGH_SYS_TIME_H
#include <stdint.h>
/* `gettimeofday(2)` is the only member of POSIX's <sys/time.h> this ABI can
 * back: it is `clock_gettime(CLOCK_REALTIME)` split into seconds and
 * microseconds. There is no `settimeofday`, no `struct timezone` support (the
 * second argument is accepted and ignored, as on Linux) and no `utime`. */
struct timeval {
    int64_t tv_sec;  /* seconds since the epoch */
    int64_t tv_usec; /* microseconds, 0..999999 */
};
int gettimeofday(struct timeval *tv, void *tz);
#endif