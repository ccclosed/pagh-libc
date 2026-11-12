/* Differential test for the pagh C library's time formatting (src/time.c).
 *
 * src/time.c is compiled with every public symbol renamed (see
 * tools/host-tests.sh) and its only syscall entry point, __pagh_syscall3(), is
 * stubbed in this file to forward clock_gettime/nanosleep to the host. That
 * leaves both implementations callable in one process:
 *
 *  - gmtime_r/localtime_r are compared field by field against the host libc
 *    over fixed edge cases and 2,000 random timestamps (pre-1970 included);
 *  - localtime_r is additionally compared for the fixed-offset TZ forms this
 *    libc supports (UTC, MSK-3, EST5, UTC+2, GMT-1), with the host's TZ set the
 *    same way;
 *  - strftime is compared byte for byte over the basic conversion set, odd
 *    formats (unknown conversion, trailing '%'), constructed `struct tm`
 *    values outside the usual range, and truncation caps;
 *  - time/clock_gettime/gettimeofday are checked for mutual consistency and
 *    against the host clock.
 *
 *   ./build/host_time_test
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* The library sources write the plain `int errno` defined by
 * tests/host_errno_shim.c; glibc's errno macro is thread-local and unrelated,
 * so this test reads the shim's variable instead. */
#undef errno
extern int errno;

/* The renamed pagh implementation. A host `struct tm` is bigger than the pagh
 * one (it has tm_gmtoff/tm_zone after the nine standard fields), so passing a
 * host struct to the pagh functions is safe: they only touch the first nine
 * ints, which is exactly the ABI-compatible prefix. */
extern long pagh_clock_gettime(int clock, void *ts);
extern long pagh_time(long *out);
extern long pagh_nanosleep(const void *req, void *rem);
extern unsigned pagh_sleep(unsigned seconds);
extern int pagh_usleep(unsigned usec);
extern int pagh_gettimeofday(void *tv, void *tz);
extern struct tm *pagh_gmtime_r(const long *timer, struct tm *result);
extern struct tm *pagh_localtime_r(const long *timer, struct tm *result);
extern struct tm *pagh_gmtime(const long *timer);
extern struct tm *pagh_localtime(const long *timer);
extern size_t pagh_strftime(char *s, size_t max, const char *format,
                            const struct tm *tm);

/* src/time.c's syscall escape hatch: forward to the host so the real code
 * paths run (returning -errno like the kernel would). */
#define SYS_CLOCK_GETTIME 113
#define SYS_NANOSLEEP 101
long __pagh_syscall3(long nr, long a0, long a1, long a2) {
    (void)a2;
    if (nr == SYS_CLOCK_GETTIME) {
        int r = clock_gettime((int)a0, (struct timespec *)(void *)a1);
        return r < 0 ? -errno : r;
    }
    if (nr == SYS_NANOSLEEP) {
        int r = nanosleep((const struct timespec *)(void *)a0,
                          (struct timespec *)(void *)a1);
        return r < 0 ? -errno : r;
    }
    return -38; /* -ENOSYS */
}

static int checks;
static int failures;

static void ok(int condition, const char *what, const char *detail) {
    checks++;
    if (condition) return;
    failures++;
    if (failures <= 40) printf("FAIL %s: %s\n", what, detail);
}

static unsigned long long rng_state = 0x9e3779b97f4a7c15ULL;

static unsigned long long rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void compare_tm(const char *what, const struct tm *pagh, const struct tm *host) {
    ok(pagh->tm_sec == host->tm_sec, what, "tm_sec");
    ok(pagh->tm_min == host->tm_min, what, "tm_min");
    ok(pagh->tm_hour == host->tm_hour, what, "tm_hour");
    ok(pagh->tm_mday == host->tm_mday, what, "tm_mday");
    ok(pagh->tm_mon == host->tm_mon, what, "tm_mon");
    ok(pagh->tm_year == host->tm_year, what, "tm_year");
    ok(pagh->tm_wday == host->tm_wday, what, "tm_wday");
    ok(pagh->tm_yday == host->tm_yday, what, "tm_yday");
    ok(pagh->tm_isdst == host->tm_isdst, what, "tm_isdst");
}

static void test_clock(void) {
    struct timespec pagh_ts, host_ts;
    ok(pagh_clock_gettime(0, &pagh_ts) == 0, "clock_gettime realtime", "failed");
    ok(clock_gettime(CLOCK_REALTIME, &host_ts) == 0, "host clock", "failed");
    ok(pagh_ts.tv_nsec >= 0 && pagh_ts.tv_nsec < 1000000000L, "clock_gettime realtime",
       "tv_nsec out of range");
    long long diff = (long long)pagh_ts.tv_sec - (long long)host_ts.tv_sec;
    ok(diff >= -2 && diff <= 2, "clock_gettime realtime", "seconds differ from host");
    long long realtime_sec = pagh_ts.tv_sec;

    ok(pagh_clock_gettime(1, &pagh_ts) == 0, "clock_gettime monotonic", "failed");
    ok(clock_gettime(CLOCK_MONOTONIC, &host_ts) == 0, "host monotonic", "failed");
    diff = (long long)pagh_ts.tv_sec - (long long)host_ts.tv_sec;
    ok(diff >= -2 && diff <= 2, "clock_gettime monotonic", "seconds differ from host");

    long t1 = 0;
    long ret = pagh_time(&t1);
    ok(ret == t1, "time", "return value != *out");
    ok(ret == realtime_sec || ret == realtime_sec + 1, "time", "not the realtime clock");
    ok(pagh_time(NULL) >= t1, "time NULL", "clock went backwards");

    struct timeval tv;
    ok(pagh_gettimeofday(&tv, NULL) == 0, "gettimeofday", "failed");
    ok(tv.tv_usec >= 0 && tv.tv_usec < 1000000L, "gettimeofday", "tv_usec out of range");
    diff = (long long)tv.tv_sec - (long long)t1;
    ok(diff >= -2 && diff <= 2, "gettimeofday", "seconds differ from time()");
    struct timeval host_tv;
    ok(gettimeofday(&host_tv, NULL) == 0, "host gettimeofday", "failed");
    diff = (long long)tv.tv_sec - (long long)host_tv.tv_sec;
    ok(diff >= -2 && diff <= 2, "gettimeofday", "seconds differ from host");
    ok(pagh_gettimeofday(NULL, NULL) == 0, "gettimeofday NULL", "must succeed");
}

static void gmtime_case(long long stamp) {
    time_t t = (time_t)stamp;
    struct tm pagh_tm, host_tm;
    memset(&pagh_tm, 0xAA, sizeof pagh_tm);
    memset(&host_tm, 0x55, sizeof host_tm);
    struct tm *pr = pagh_gmtime_r(&t, &pagh_tm);
    struct tm *hr = gmtime_r(&t, &host_tm);
    ok((pr == NULL) == (hr == NULL), "gmtime_r NULL", "return mismatch");
    if (!pr || !hr) return;
    compare_tm("gmtime_r", &pagh_tm, &host_tm);
}

static void test_gmtime(void) {
    static const long long fixed[] = {
        0, 1, -1, 86399, 86400, -86400, 951782400, /* 2000-02-29 */
        951868800,                                  /* 2000-03-01 */
        4102444800,                                 /* 2100-01-01 */
        253402300799LL,                             /* 9999-12-31 */
        -2208988800LL,                              /* 1900-01-01 */
        -62135596800LL,                             /* year 1 */
    };
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) gmtime_case(fixed[i]);
    for (int i = 0; i < 2000; i++) {
        long long stamp = (long long)(rng() % 7000000000ULL) - 2300000000LL;
        gmtime_case(stamp);
    }
    /* Overflow behaviour: the year must fit an int, as in glibc. */
    time_t big = (time_t)LLONG_MAX, small = (time_t)LLONG_MIN;
    struct tm out;
    errno = 0;
    ok(pagh_gmtime_r(&big, &out) == NULL && errno == EOVERFLOW, "gmtime overflow max",
       "must be NULL/EOVERFLOW");
    errno = 0;
    ok(pagh_gmtime_r(&small, &out) == NULL && errno == EOVERFLOW, "gmtime overflow min",
       "must be NULL/EOVERFLOW");

    /* The non-reentrant wrappers return the same breakdown. */
    time_t t = 1762980313;
    struct tm *p = pagh_gmtime(&t);
    struct tm expect;
    ok(p != NULL && pagh_gmtime_r(&t, &expect) != NULL, "gmtime", "failed");
    if (p) compare_tm("gmtime", p, &expect);
}

static void localtime_case(const char *tz, long long stamp) {
    if (tz) {
        setenv("TZ", tz, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    time_t t = (time_t)stamp;
    struct tm pagh_tm, host_tm;
    memset(&pagh_tm, 0xAA, sizeof pagh_tm);
    memset(&host_tm, 0x55, sizeof host_tm);
    struct tm *pr = pagh_localtime_r(&t, &pagh_tm);
    if (!tz) {
        /* Our documented fallback for an unset TZ is UTC; glibc would read the
         * host's /etc/localtime, so compare against UTC instead. */
        struct tm utc;
        struct tm *ur = gmtime_r(&t, &utc);
        ok(pr != NULL && ur != NULL, "localtime_r unset", "failed");
        if (pr && ur) compare_tm("localtime_r unset", &pagh_tm, &utc);
        return;
    }
    struct tm *hr = localtime_r(&t, &host_tm);
    ok((pr == NULL) == (hr == NULL), "localtime_r NULL", "return mismatch");
    if (!pr || !hr) return;
    compare_tm("localtime_r", &pagh_tm, &host_tm);
}

static void test_localtime(void) {
    static const char *const zones[] = {
        "UTC", "UTC0", "MSK-3", "EST5", "UTC+2", "GMT-1",
    };
    static const long long stamps[] = {
        0, -1, 86399, 1762980313, 951782400, -2208988800LL,
    };
    for (size_t z = 0; z < sizeof zones / sizeof zones[0]; z++) {
        for (size_t s = 0; s < sizeof stamps / sizeof stamps[0]; s++)
            localtime_case(zones[z], stamps[s]);
    }
    for (int i = 0; i < 100; i++) {
        const char *zone = zones[rng() % (sizeof zones / sizeof zones[0])];
        localtime_case(zone, (long long)(rng() % 7000000000ULL) - 2300000000LL);
    }
    /* Unset TZ: documented UTC fallback. */
    localtime_case(NULL, 1762980313);
    unsetenv("TZ");
    tzset();

    /* The non-reentrant wrapper agrees with the _r form. */
    setenv("TZ", "MSK-3", 1);
    tzset();
    time_t t = 1762980313;
    struct tm *p = pagh_localtime(&t);
    struct tm expect;
    ok(p != NULL && pagh_localtime_r(&t, &expect) != NULL, "localtime", "failed");
    if (p) compare_tm("localtime", p, &expect);
    unsetenv("TZ");
    tzset();
}

static void strftime_case(const char *what, const char *format, const struct tm *tm) {
    char pagh_buf[512];
    char host_buf[512];
    memset(pagh_buf, 0xAA, sizeof pagh_buf);
    memset(host_buf, 0xBB, sizeof host_buf);
    size_t pn = pagh_strftime(pagh_buf, sizeof pagh_buf, format, tm);
    size_t hn = strftime(host_buf, sizeof host_buf, format, tm);
    if (pn != hn && failures <= 40)
        printf("DEBUG %s fmt=[%s] y=%d m=%d d=%d wd=%d yd=%d h=%d mi=%d s=%d: "
               "pagh n=%zu [%s] host n=%zu [%s]\n",
               what, format, tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_wday,
               tm->tm_yday, tm->tm_hour, tm->tm_min, tm->tm_sec,
               pn, pn ? pagh_buf : "", hn, hn ? host_buf : "");
    ok(pn == hn, what, "return length mismatch");
    if (pn > 0 && pn == hn) {
        ok(strcmp(pagh_buf, host_buf) == 0, what, "formatted text mismatch");
        ok(pn == strlen(pagh_buf), what, "return length != strlen");
    }
}

static void strftime_truncation(const char *format, const struct tm *tm) {
    char host_buf[512];
    size_t full = strftime(host_buf, sizeof host_buf, format, tm);
    if (full == 0) return; /* empty result: nothing to truncate */
    for (size_t cap = 1; cap <= full + 1; cap++) {
        char pagh_buf[512];
        char hbuf[512];
        size_t pn = pagh_strftime(pagh_buf, cap, format, tm);
        size_t hn = strftime(hbuf, cap, format, tm);
        ok(pn == hn, "strftime truncation", "return mismatch");
        if (cap > full) ok(pn == full, "strftime truncation", "must fit");
        else ok(pn == 0, "strftime truncation", "must return 0");
    }
}

static void test_strftime(void) {
    static const char *const formats[] = {
        "%Y-%m-%d %H:%M:%S", "%F %T", "%a %A %b %B", "%c", "%C", "%d", "%D",
        "%e", "%H", "%I", "%j", "%m", "%M", "%n%t%%", "%p", "%r", "%R", "%S",
        "%u", "%w", "%x", "%X", "%y", "%Y", "plain text", "100%% done",
        "%Q", "%", "%j%%%Y", "%%%Y", "%Y%m%d%H%M%S",
    };
    static const char *const zones[] = { "UTC", "MSK-3" };
    static const long long stamps[] = { 0, -1, 951782400, 1762980313,
                                        -2208988800LL, 4102444800LL };
    setenv("TZ", "UTC", 1);
    tzset();
    for (size_t z = 0; z < sizeof zones / sizeof zones[0]; z++) {
        setenv("TZ", zones[z], 1);
        tzset();
        for (size_t s = 0; s < sizeof stamps / sizeof stamps[0]; s++) {
            struct tm tm;
            time_t t = (time_t)stamps[s];
            if (!localtime_r(&t, &tm)) continue;
            for (size_t f = 0; f < sizeof formats / sizeof formats[0]; f++)
                strftime_case("strftime", formats[f], &tm);
        }
    }
    unsetenv("TZ");
    tzset();

    /* Truncation: cap = required size + 1 is the first one that fits. */
    struct tm tm;
    time_t t = 1762980313;
    gmtime_r(&t, &tm);
    strftime_truncation("%Y-%m-%d", &tm);
    strftime_truncation("%c", &tm);
    strftime_truncation("%%", &tm);

    /* Constructed struct tm values: years outside the usual 4-digit range,
     * every month length, the ends of the week and year. glibc formats these
     * fields directly, and so must we. */
    static const int years[] = { 1900 - 1900, 0 - 1900, 5 - 1900, 999 - 1900,
                                 1970 - 1900, 2038 - 1900, 9999 - 1900,
                                 10000 - 1900, -100 - 1900 };
    static const int months[] = { 0, 1, 2, 6, 11 };
    static const int days[] = { 1, 5, 15, 28, 30, 31 };
    static const int weekdays[] = { 0, 1, 6 };
    static const char *const cformats[] = {
        "%Y %C %y", "%a %A", "%b %B", "%j", "%e", "%Y-%m-%d %H:%M:%S",
    };
    for (size_t y = 0; y < sizeof years / sizeof years[0]; y++) {
        for (size_t m = 0; m < sizeof months / sizeof months[0]; m++) {
            for (size_t d = 0; d < sizeof days / sizeof days[0]; d++) {
                struct tm built;
                memset(&built, 0, sizeof built);
                built.tm_year = years[y];
                built.tm_mon = months[m];
                built.tm_mday = days[d];
                built.tm_wday = weekdays[(y + m + d) % (sizeof weekdays / sizeof weekdays[0])];
                built.tm_yday = (int)((y * 37 + m * 31 + d) % 366);
                built.tm_hour = (int)((y + m * 3 + d * 7) % 24);
                built.tm_min = (int)((y * 5 + m + d) % 60);
                built.tm_sec = (int)((y + m * 11 + d * 13) % 60);
                for (size_t f = 0; f < sizeof cformats / sizeof cformats[0]; f++)
                    strftime_case("strftime built", cformats[f], &built);
            }
        }
    }
}

int main(void) {
    test_clock();
    test_gmtime();
    test_localtime();
    test_strftime();
    if (failures) {
        printf("time: %d checks, %d FAILURES\n", checks, failures);
        return 1;
    }
    printf("time: %d checks, 0 failures\n", checks);
    return 0;
}