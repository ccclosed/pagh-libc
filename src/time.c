#include <pagh.h>
#include <time.h>
#include <errno.h>

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
