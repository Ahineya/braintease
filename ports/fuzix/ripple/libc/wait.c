/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <sys/select.h>
#include <unistd.h>

/* Preserve signal interruption and process alarms; readiness uses monotonic
 * kernel deadlines independently of the legacy decisecond timer queue. */
int usleep(useconds_t microseconds) {
    struct timeval delay;
    delay.tv_sec = microseconds / 1000000UL;
    delay.tv_usec = microseconds % 1000000UL;
    return select(0, NULL, NULL, NULL, &delay);
}
