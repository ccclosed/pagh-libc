#ifndef PAGH_TIME_H
#define PAGH_TIME_H
#include <stdint.h>
struct timespec { int64_t tv_sec; int64_t tv_nsec; };
typedef long time_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
int clock_gettime(int clock, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);
time_t time(time_t *out);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
#endif
