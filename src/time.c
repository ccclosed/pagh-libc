#include <pagh.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int clock_gettime(int clock, struct timespec *ts) {
    long r = __pagh_syscall3(SYS_CLOCK_GETTIME, clock, (long)ts, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}
int nanosleep(const struct timespec *req, struct timespec *rem) {
    long r = __pagh_syscall3(SYS_NANOSLEEP, (long)req, (long)rem, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}
time_t time(time_t *out) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return -1;
    if (out) *out = (time_t)ts.tv_sec;
    return (time_t)ts.tv_sec;
}
int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz; /* the ABI has no timezone state; Linux ignores it too */
    struct timespec ts;
    if (!tv) return 0;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return -1;
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}
unsigned int sleep(unsigned int seconds) {
    struct timespec ts = { (long)seconds, 0 };
    struct timespec rem = { 0, 0 };
    if (nanosleep(&ts, &rem) == 0) return 0;
    return (unsigned int)rem.tv_sec;
}
int usleep(unsigned int usec) {
    struct timespec ts = { (long)(usec / 1000000), (long)((usec % 1000000) * 1000) };
    return nanosleep(&ts, 0);
}

/* ------------------------------------------------------------------------- */
/* Broken-down time                                                          */
/* ------------------------------------------------------------------------- */

/* Floor division: `-1 / 86400` must be -1, not 0, or every pre-1970 time is
 * off by a whole day. */
static long long floor_div(long long a, long long b) {
    long long q = a / b;
    if (a % b != 0 && ((a < 0) != (b < 0))) q--;
    return q;
}

static int is_leap(long long year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static const int MONTH_DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

/* Civil date from days since 1970-01-01 (Howard Hinnant's algorithm, valid for
 * every `long long` day count). */
struct tm *gmtime_r(const time_t *timer, struct tm *result) {
    if (!timer || !result) {
        errno = EINVAL;
        return 0;
    }
    long long sec = (long long)*timer;
    long long days = floor_div(sec, 86400);
    long long rem = sec - days * 86400;          /* 0..86399 */
    long long z = days + 719468;
    long long era = floor_div(z, 146097);
    unsigned doe = (unsigned)(z - era * 146097); /* [0, 146096] */
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long year = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned day = doy - (153 * mp + 2) / 5 + 1;
    unsigned month = mp < 10 ? mp + 3 : mp - 9;
    year += (month <= 2);

    long long tm_year = year - 1900;
    if (tm_year > 2147483647LL || tm_year < (-2147483647LL - 1)) {
        errno = EOVERFLOW;
        return 0;
    }

    int yday = (int)day - 1;
    for (unsigned m = 0; m < month - 1; m++) yday += MONTH_DAYS[m];
    if (month > 2 && is_leap(year)) yday++;

    result->tm_sec = (int)(rem % 60);
    result->tm_min = (int)((rem / 60) % 60);
    result->tm_hour = (int)(rem / 3600);
    result->tm_mday = (int)day;
    result->tm_mon = (int)month - 1;
    result->tm_year = (int)tm_year;
    result->tm_wday = (int)((days % 7 + 11) % 7); /* epoch was a Thursday (4) */
    result->tm_yday = yday;
    result->tm_isdst = 0;
    return result;
}

static struct tm gmtime_static;

struct tm *gmtime(const time_t *timer) {
    return gmtime_r(timer, &gmtime_static);
}

/* Parse the fixed-offset subset of `TZ` documented in <time.h>. Returns 0 with
 * *east set on success, -1 when the value is not that subset (the caller then
 * uses UTC). `east` is the number of seconds to add to UTC to get local time:
 * POSIX strings carry the opposite sign ("MSK-3" is UTC+3). */
static int tz_offset_east(long *east) {
    const char *tz = getenv("TZ");
    *east = 0;
    if (!tz || !*tz) return 0;
    if (strcmp(tz, "UTC") == 0 || strcmp(tz, "GMT") == 0 || strcmp(tz, "UT") == 0)
        return 0;
    const char *p = tz;
    if (*p == ':') {            /* POSIX allows a leading ':' */
        p++;
        if (*p == '/') return -1; /* a file path: no tzfile support */
    }
    size_t name_len = 0;
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
        p++;
        name_len++;
    }
    if (name_len < 3) return -1;
    if (!*p) return 0;          /* a bare name: standard time, zero offset */
    int sign = 1;
    if (*p == '+' || *p == '-') {
        if (*p == '-') sign = -1;
        p++;
    }
    if (*p < '0' || *p > '9') return -1;
    long hours = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        hours = hours * 10 + (*p - '0');
        p++;
        if (++digits > 2) return -1;
    }
    long minutes = 0, seconds = 0;
    if (*p == ':') {
        p++;
        if (*p < '0' || *p > '9') return -1;
        minutes = (*p++ - '0');
        if (*p >= '0' && *p <= '9') minutes = minutes * 10 + (*p++ - '0');
        if (*p == ':') {
            p++;
            if (*p < '0' || *p > '9') return -1;
            seconds = (*p++ - '0');
            if (*p >= '0' && *p <= '9') seconds = seconds * 10 + (*p++ - '0');
        }
    }
    if (*p != '\0' && *p != ',') return -1; /* DST rules: not supported */
    if (hours > 23 || minutes > 59 || seconds > 59) return -1;
    long west = sign * (hours * 3600 + minutes * 60 + seconds);
    *east = -west;
    return 0;
}

struct tm *localtime_r(const time_t *timer, struct tm *result) {
    if (!timer || !result) {
        errno = EINVAL;
        return 0;
    }
    long east = 0;
    if (tz_offset_east(&east) != 0) east = 0; /* unsupported TZ: UTC */
    long long local;
    if (__builtin_add_overflow((long long)*timer, (long long)east, &local)) {
        errno = EOVERFLOW;
        return 0;
    }
    time_t adjusted = (time_t)local;
    return gmtime_r(&adjusted, result);
}

static struct tm localtime_static;

struct tm *localtime(const time_t *timer) {
    return localtime_r(timer, &localtime_static);
}

/* ------------------------------------------------------------------------- */
/* strftime                                                                  */
/* ------------------------------------------------------------------------- */

static const char *const WDAY_ABBR[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *const WDAY_FULL[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char *const MON_ABBR[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char *const MON_FULL[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

struct out {
    char *s;
    size_t max;   /* including the NUL */
    size_t len;
    int overflow;
};

static void out_put(struct out *o, const char *text, size_t n) {
    if (o->overflow) return;
    if (n > o->max - 1 - o->len) {
        o->overflow = 1;
        return;
    }
    memcpy(o->s + o->len, text, n);
    o->len += n;
}
static void out_char(struct out *o, char c) {
    if (o->overflow) return;
    if (o->len + 1 > o->max - 1) {
        o->overflow = 1;
        return;
    }
    o->s[o->len++] = c;
}
/* Signed decimal with a minimum width. `pad` fills to the left of the digits
 * ("%e" uses ' ', the numeric conversions '0'). */
static void out_number(struct out *o, long long value, int width, char pad) {
    char digits[24];
    int n = 0;
    int negative = value < 0;
    unsigned long long magnitude =
        negative ? (unsigned long long)(-(value + 1)) + 1 : (unsigned long long)value;
    do {
        digits[n++] = (char)('0' + (int)(magnitude % 10));
        magnitude /= 10;
    } while (magnitude);
    if (o->overflow) return;
    int total = n + (negative ? 1 : 0);
    if (negative) out_char(o, '-');
    for (int i = total; i < width; i++) out_char(o, pad);
    while (n) out_char(o, digits[--n]);
}

static int normalize_index(int value, int modulo) {
    value %= modulo;
    if (value < 0) value += modulo;
    return value;
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    if (!s || !format || !tm) {
        errno = EINVAL;
        return 0;
    }
    if (max == 0) return 0;
    struct out o = { s, max, 0, 0 };
    int wday = normalize_index(tm->tm_wday, 7);
    int mon = normalize_index(tm->tm_mon, 12);
    long long year = (long long)tm->tm_year + 1900;

    while (*format && !o.overflow) {
        if (*format != '%') {
            out_char(&o, *format++);
            continue;
        }
        format++;
        char conversion = *format;
        if (!conversion) { /* a trailing lone '%' is emitted literally */
            out_char(&o, '%');
            break;
        }
        format++;
        switch (conversion) {
        case 'a': out_put(&o, WDAY_ABBR[wday], 3); break;
        case 'A': out_put(&o, WDAY_FULL[wday], strlen(WDAY_FULL[wday])); break;
        case 'b': case 'h': out_put(&o, MON_ABBR[mon], 3); break;
        case 'B': out_put(&o, MON_FULL[mon], strlen(MON_FULL[mon])); break;
        case 'c':
            out_put(&o, WDAY_ABBR[wday], 3);
            out_char(&o, ' ');
            out_put(&o, MON_ABBR[mon], 3);
            out_char(&o, ' ');
            out_number(&o, tm->tm_mday, 2, ' ');
            out_char(&o, ' ');
            out_number(&o, tm->tm_hour, 2, '0');
            out_char(&o, ':');
            out_number(&o, tm->tm_min, 2, '0');
            out_char(&o, ':');
            out_number(&o, tm->tm_sec, 2, '0');
            out_char(&o, ' ');
            out_number(&o, year, 1, '0');
            break;
        case 'C': out_number(&o, year / 100, 1, '0'); break;
        case 'd': out_number(&o, tm->tm_mday, 2, '0'); break;
        case 'D': case 'x':
            out_number(&o, tm->tm_mon + 1, 2, '0');
            out_char(&o, '/');
            out_number(&o, tm->tm_mday, 2, '0');
            out_char(&o, '/');
            out_number(&o, year % 100, 2, '0');
            break;
        case 'e': out_number(&o, tm->tm_mday, 2, ' '); break;
        case 'F':
            out_number(&o, year, 1, '0');
            out_char(&o, '-');
            out_number(&o, tm->tm_mon + 1, 2, '0');
            out_char(&o, '-');
            out_number(&o, tm->tm_mday, 2, '0');
            break;
        case 'H': out_number(&o, tm->tm_hour, 2, '0'); break;
        case 'I': {
            int hour = tm->tm_hour % 12;
            if (hour <= 0) hour += 12;
            out_number(&o, hour, 2, '0');
            break;
        }
        case 'j': out_number(&o, tm->tm_yday + 1, 3, '0'); break;
        case 'm': out_number(&o, tm->tm_mon + 1, 2, '0'); break;
        case 'M': out_number(&o, tm->tm_min, 2, '0'); break;
        case 'n': out_char(&o, '\n'); break;
        case 'p': out_put(&o, tm->tm_hour < 12 ? "AM" : "PM", 2); break;
        case 'r':
            {
                int hour = tm->tm_hour % 12;
                if (hour <= 0) hour += 12;
                out_number(&o, hour, 2, '0');
            }
            out_char(&o, ':');
            out_number(&o, tm->tm_min, 2, '0');
            out_char(&o, ':');
            out_number(&o, tm->tm_sec, 2, '0');
            out_char(&o, ' ');
            out_put(&o, tm->tm_hour < 12 ? "AM" : "PM", 2);
            break;
        case 'R':
            out_number(&o, tm->tm_hour, 2, '0');
            out_char(&o, ':');
            out_number(&o, tm->tm_min, 2, '0');
            break;
        case 'S': out_number(&o, tm->tm_sec, 2, '0'); break;
        case 't': out_char(&o, '\t'); break;
        case 'T': case 'X':
            out_number(&o, tm->tm_hour, 2, '0');
            out_char(&o, ':');
            out_number(&o, tm->tm_min, 2, '0');
            out_char(&o, ':');
            out_number(&o, tm->tm_sec, 2, '0');
            break;
        case 'u': out_number(&o, wday == 0 ? 7 : wday, 1, '0'); break;
        case 'w': out_number(&o, wday, 1, '0'); break;
        case 'y': out_number(&o, year % 100, 2, '0'); break;
        case 'Y': out_number(&o, year, 1, '0'); break;
        case '%': out_char(&o, '%'); break;
        default:  /* unknown conversion: copy it literally, like glibc */
            out_char(&o, '%');
            out_char(&o, conversion);
            break;
        }
    }
    if (o.overflow) return 0;
    s[o.len] = 0;
    return o.len;
}