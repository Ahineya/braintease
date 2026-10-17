/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_CLOCK_ABI_H
#define RIPPLE_CLOCK_ABI_H
#include <stdint.h>
/* Syscall 27 selector 2. Selectors 0 (calendar) and 1 (legacy decisecond
 * counter) retain their existing __ktime_t payload and interpretation.
 * This selector copies exactly eight C16 cells, independently of time_t. */
#define RIPPLE_TIME_MONOTONIC 2
struct ripple_clock_sample {
    uint32_t seconds_low, seconds_high, nanoseconds, resolution_ns;
};
#endif
