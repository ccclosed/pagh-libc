/* Live check for the time surface added in 0.14.0: clock_gettime/time/
 * gettimeofday, gmtime_r/localtime_r and strftime.
 *
 * The kernel's goldfish RTC supplies the epoch; this program does not trust
 * it blindly but checks the helpers against each other and against values it
 * computes itself:
 *
 *  - time(), gettimeofday() and clock_gettime(CLOCK_REALTIME) agree within a
 *    second;
 *  - gmtime_r breaks the epoch down to the civil date (printed), and
 *    localtime_r with TZ unset equals gmtime_r (the documented UTC fallback);
 *  - strftime's basic conversions match snprintf of the same `struct tm`
 *    fields, including the C-locale name tables and space-padded %e;
 *  - localtime_r applies the fixed POSIX offsets: TZ=MSK-3 is UTC+3 and
 *    TZ=EST5 is UTC-5, compared against gmtime_r of the shifted epoch;
 *  - truncation returns 0 instead of overflowing a small buffer.
 *
 * It prints one marker per step and ends with "timefmt: ok"; the exit code is
 * non-zero at the first failure.
 *
 *   run /mnt/timefmt
 *
 * Exit codes: 0 success, 1 clock, 2 gmtime/localtime, 3 strftime, 4 TZ offsets. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *const WDAY_ABBR[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *const MON_ABBR[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static int check_clock(long long *epoch_out) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        printf("timefmt: clock_gettime(CLOCK_REALTIME) failed\n");
        return 1;
    }
    if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) {
        printf("timefmt: tv_nsec out of range: %lld\n", (long long)ts.tv_nsec);
        return 1;
    }
    printf("timefmt: clock_gettime realtime %lld.%09lld\n",
           (long long)ts.tv_sec, (long long)ts.tv_nsec);

    time_t t = time(0);
    if (t != (time_t)ts.tv_sec && t != (time_t)ts.tv_sec + 1) {
        printf("timefmt: time() %lld disagrees with clock_gettime %lld\n",
               (long long)t, (long long)ts.tv_sec);
        return 1;
    }
    time_t t_out = 0;
    if (time(&t_out) != t_out || t_out < t) {
        printf("timefmt: time(&t) return/out mismatch\n");
        return 1;
    }

    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) {
        printf("timefmt: gettimeofday failed\n");
        return 1;
    }
    if (tv.tv_usec < 0 || tv.tv_usec >= 1000000L) {
        printf("timefmt: tv_usec out of range: %lld\n", (long long)tv.tv_usec);
        return 1;
    }
    if (tv.tv_sec < (long long)t - 1 || tv.tv_sec > (long long)t + 1) {
        printf("timefmt: gettimeofday %lld disagrees with time() %lld\n",
               (long long)tv.tv_sec, (long long)t);
        return 1;
    }
    printf("timefmt: time() %lld == gettimeofday %lld.%06lld (within 1 s)\n",
           (long long)t, (long long)tv.tv_sec, (long long)tv.tv_usec);
    printf("timefmt: note: the ABI clock is kernel uptime, not the RTC wall clock\n");
    *epoch_out = (long long)t;
    return 0;
}

static int check_broken_down(time_t t) {
    struct tm g, l;
    memset(&g, 0xAA, sizeof g);
    memset(&l, 0x55, sizeof l);
    if (!gmtime_r(&t, &g)) {
        printf("timefmt: gmtime_r failed\n");
        return 2;
    }
    printf("timefmt: gmtime_r %04d-%02d-%02d %02d:%02d:%02d "
           "(wday %d yday %d isdst %d)\n",
           g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
           g.tm_hour, g.tm_min, g.tm_sec, g.tm_wday, g.tm_yday, g.tm_isdst);
    if (g.tm_mon < 0 || g.tm_mon > 11 || g.tm_mday < 1 || g.tm_mday > 31 ||
        g.tm_hour < 0 || g.tm_hour > 23 || g.tm_wday < 0 || g.tm_wday > 6 ||
        g.tm_yday < 0 || g.tm_yday > 365) {
        printf("timefmt: gmtime_r fields out of range\n");
        return 2;
    }
    if (!localtime_r(&t, &l)) {
        printf("timefmt: localtime_r failed\n");
        return 2;
    }
    if (memcmp(&g, &l, sizeof g) != 0) {
        printf("timefmt: localtime_r differs from gmtime_r with TZ unset\n");
        return 2;
    }
    printf("timefmt: localtime_r == gmtime_r (TZ unset is UTC)\n");

    struct tm *ng = gmtime(&t);
    struct tm *nl = localtime(&t);
    if (!ng || !nl || memcmp(ng, &g, sizeof g) != 0 ||
        memcmp(nl, &l, sizeof l) != 0) {
        printf("timefmt: gmtime()/localtime() wrappers disagree\n");
        return 2;
    }
    printf("timefmt: gmtime()/localtime() wrappers agree\n");
    return 0;
}

static int check_strftime(const struct tm *g) {
    char buf[128];
    char want[128];

    if (strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", g) == 0) {
        printf("timefmt: strftime %%Y-%%m-%%d failed\n");
        return 3;
    }
    snprintf(want, sizeof want, "%04d-%02d-%02d %02d:%02d:%02d",
             g->tm_year + 1900, g->tm_mon + 1, g->tm_mday,
             g->tm_hour, g->tm_min, g->tm_sec);
    if (strcmp(buf, want) != 0) {
        printf("timefmt: strftime ISO [%s] != snprintf [%s]\n", buf, want);
        return 3;
    }
    printf("timefmt: strftime %%Y-%%m-%%d %%H:%%M:%%S = %s\n", buf);

    if (strftime(buf, sizeof buf, "%a %b %e %H:%M:%S %Y", g) == 0) {
        printf("timefmt: strftime %%c-like failed\n");
        return 3;
    }
    snprintf(want, sizeof want, "%s %s %2d %02d:%02d:%02d %d",
             WDAY_ABBR[g->tm_wday], MON_ABBR[g->tm_mon], g->tm_mday,
             g->tm_hour, g->tm_min, g->tm_sec, g->tm_year + 1900);
    if (strcmp(buf, want) != 0) {
        printf("timefmt: strftime names [%s] != snprintf [%s]\n", buf, want);
        return 3;
    }
    printf("timefmt: strftime C-locale names + %%e = %s\n", buf);

    if (strftime(buf, sizeof buf, "%F %T %j %u %w %I %p", g) == 0) {
        printf("timefmt: strftime basic set failed\n");
        return 3;
    }
    snprintf(want, sizeof want, "%04d-%02d-%02d %02d:%02d:%02d %03d %d %d %02d %s",
             g->tm_year + 1900, g->tm_mon + 1, g->tm_mday,
             g->tm_hour, g->tm_min, g->tm_sec, g->tm_yday + 1,
             g->tm_wday == 0 ? 7 : g->tm_wday, g->tm_wday,
             g->tm_hour % 12 == 0 ? 12 : g->tm_hour % 12,
             g->tm_hour < 12 ? "AM" : "PM");
    if (strcmp(buf, want) != 0) {
        printf("timefmt: strftime basic set [%s] != snprintf [%s]\n", buf, want);
        return 3;
    }
    printf("timefmt: strftime %%F %%T %%j %%u %%w %%I %%p = %s\n", buf);

    char small[4];
    if (strftime(small, sizeof small, "%Y", g) != 0) {
        printf("timefmt: strftime into a 4-byte buffer did not fail\n");
        return 3;
    }
    printf("timefmt: truncation returns 0 (4-byte buffer, %%Y)\n");
    return 0;
}

static int check_fixed_offsets(time_t t) {
    struct tm g;
    if (!gmtime_r(&t, &g)) return 4;

    setenv("TZ", "MSK-3", 1); /* POSIX sign: UTC+3 */
    struct tm l;
    if (!localtime_r(&t, &l)) {
        printf("timefmt: localtime_r failed with TZ=MSK-3\n");
        return 4;
    }
    time_t shifted = t + 3 * 3600;
    struct tm expect;
    if (!gmtime_r(&shifted, &expect) || memcmp(&l, &expect, sizeof l) != 0) {
        printf("timefmt: TZ=MSK-3 is not UTC+3 (%02d vs %02d)\n",
               l.tm_hour, expect.tm_hour);
        return 4;
    }
    printf("timefmt: TZ=MSK-3 offset +3 h verified\n");

    setenv("TZ", "EST5", 1); /* POSIX sign: UTC-5 */
    if (!localtime_r(&t, &l)) {
        printf("timefmt: localtime_r failed with TZ=EST5\n");
        return 4;
    }
    shifted = t - 5 * 3600;
    if (!gmtime_r(&shifted, &expect) || memcmp(&l, &expect, sizeof l) != 0) {
        printf("timefmt: TZ=EST5 is not UTC-5\n");
        return 4;
    }
    printf("timefmt: TZ=EST5 offset -5 h verified\n");

    unsetenv("TZ");
    if (!localtime_r(&t, &l) || memcmp(&l, &g, sizeof l) != 0) {
        printf("timefmt: localtime_r after unsetenv(TZ) is not UTC\n");
        return 4;
    }
    printf("timefmt: TZ unset restored UTC\n");
    return 0;
}

int main(void) {
    long long epoch = 0;
    int rc = check_clock(&epoch);
    if (rc) return rc;
    time_t t = (time_t)epoch;
    rc = check_broken_down(t);
    if (rc) return rc;
    struct tm g;
    if (!gmtime_r(&t, &g)) return 2;
    rc = check_strftime(&g);
    if (rc) return rc;
    rc = check_fixed_offsets(t);
    if (rc) return rc;
    printf("timefmt: ok (epoch %lld, gmtime %04d-%02d-%02d %02d:%02d:%02d)\n",
           epoch, g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
           g.tm_hour, g.tm_min, g.tm_sec);
    return 0;
}