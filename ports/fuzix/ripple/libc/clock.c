/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include "clock-abi.h"
extern long call(unsigned, long, long, long, long);

static int sample(struct ripple_clock_sample *t) {
    return call(27, (long)t, RIPPLE_TIME_MONOTONIC, 0, 0);
}
int clock_gettime(clockid_t clock, struct timespec *out) {
    struct timespec result;
    struct ripple_clock_sample fine;
    __ktime_t calendar;
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC) { errno=EINVAL;return -1; }
    if (!out) { errno=EFAULT;return -1; }
    if (clock == CLOCK_REALTIME) {
        if (_time(&calendar, 0) < 0) return -1;
        result.tv_sec=calendar.low;result.tv_nsec=0;
    } else {
        if (sample(&fine) < 0) return -1;
        if (fine.seconds_high) { errno=EOVERFLOW;return -1; }
        result.tv_sec=fine.seconds_low;result.tv_nsec=fine.nanoseconds;
    }
    *out=result;return 0;
}
int clock_getres(clockid_t clock, struct timespec *out) {
    struct timespec result;
    struct ripple_clock_sample fine;
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC) { errno=EINVAL;return -1; }
    if (!out) return 0;
    if (clock == CLOCK_REALTIME) { result.tv_sec=1;result.tv_nsec=0; }
    else {
        if (sample(&fine) < 0) return -1;
        result.tv_sec=fine.resolution_ns/1000000000UL;
        result.tv_nsec=fine.resolution_ns%1000000000UL;
    }
    *out=result;return 0;
}

/* This obsolete timezone argument is deliberately UTC; local civil-time rules
 * belong to tzset/localtime, not the epoch clock or gettimeofday. */
int gettimeofday(struct timeval *out, struct timezone *zone) {
    struct timespec sample;
    if (!out) { errno = EFAULT; return -1; }
    if (clock_gettime(CLOCK_REALTIME, &sample)) return -1;
    out->tv_sec = sample.tv_sec;
    out->tv_usec = sample.tv_nsec / 1000L;
    if (zone) { zone->tz_minuteswest = 0; zone->tz_dsttime = 0; }
    return 0;
}
