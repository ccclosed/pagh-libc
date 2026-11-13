#ifndef PAGH_TIME_H
#define PAGH_TIME_H
#include <stddef.h>
#include <stdint.h>
/* The ABI's `clock_gettime` and the broken-down time helpers. The kernel has
 * one clock family (`clock_gettime`) and no timezone database: `gmtime_r` is
 * UTC and `localtime_r` applies only a fixed offset read from `TZ` (see
 * below). There is no `mktime`, no leap-second table and no DST. */
struct timespec { int64_t tv_sec; int64_t tv_nsec; };
/* The classic nine-field `struct tm` (C89 order). `tm_isdst` is always 0:
 * nothing in this libc has DST rules. */
struct tm {
    int tm_sec;   /* 0..59 */
    int tm_min;   /* 0..59 */
    int tm_hour;  /* 0..23 */
    int tm_mday;  /* 1..31 */
    int tm_mon;   /* 0..11 */
    int tm_year;  /* years since 1900 */
    int tm_wday;  /* 0..6, Sunday = 0 */
    int tm_yday;  /* 0..365 */
    int tm_isdst; /* always 0 */
};
/* The ABI's only clock is the kernel uptime counter: `clock_gettime(0)` is
 * documented as "realtime/uptime" and the kernel returns the same millisecond
 * uptime for both clock ids. There is no wall-clock syscall (the RTC is used
 * inside the kernel for the TLS certificate clock gate, not exposed to
 * userland), so `time()` is **seconds since boot**, not an epoch timestamp.
 * `gmtime_r` still interprets the value as Unix seconds, so a fresh boot
 * formats as 1970-01-01T00:00:00 + uptime. Callers that need the real date
 * have no ABI call for it. */
typedef long time_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
int clock_gettime(int clock, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);
time_t time(time_t *out);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);

/* Broken-down time. `gmtime_r`/`localtime_r` fill a caller-owned struct and
 * return it, or NULL with errno = EOVERFLOW when the year does not fit an
 * `int` (the same limit glibc reports). `gmtime`/`localtime` use one static
 * struct and are therefore not thread-safe.
 *
 * `localtime_r` is **UTC plus a fixed offset**, not a timezone database:
 * `TZ` unset/empty/UTC/GMT/UT means UTC; a POSIX standard-time name with a
 * numeric offset ("MSK-3", "EST5", "UTC+2", optionally "hh:mm[:ss]") is
 * applied with the POSIX sign convention (the number is the time to add to
 * local time to get UTC, so "MSK-3" is UTC+3). Anything else — an
 * /etc/localtime-style path, an unknown zone name, DST rules after a comma —
 * falls back to UTC, because the ABI exposes no tzdata. */
struct tm *gmtime_r(const time_t *timer, struct tm *result);
struct tm *localtime_r(const time_t *timer, struct tm *result);
struct tm *gmtime(const time_t *timer);
struct tm *localtime(const time_t *timer);

/* Format `tm` according to `format` into `s` (at most `max` bytes including
 * the NUL). Returns the number of bytes written (excluding the NUL), or 0 when
 * the result does not fit. The supported conversions are the C/POSIX basic
 * set: a A b B c C d D e F H I j m M n p r R S t T u w x X y Y %. Unknown
 * conversions are copied literally ("%Q" stays "%Q"), as glibc does; the GNU
 * padding/locale flags (%-d, %_d, %0d, %Ec, %Od) are not implemented and are
 * copied literally. Names are the C locale's English ones. */
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);
#endif