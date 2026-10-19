/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_DEADLINE_H
#define RIPPLE_DEADLINE_H
#include <stdint.h>
#include "process-limits.h"

/* Internal monotonic wait queue, independent of FUZIX decisecond accounting.
 * All mutation is serialized by the kernel's IRQ/scheduler protection. Keeping
 * the core independent of ptab also permits exact C16 tests with a fake clock. */
#define RIPPLE_DEADLINE_SLOTS RIPPLE_PROCESS_SLOTS
#define RIPPLE_DEADLINE_WORDS ((RIPPLE_DEADLINE_SLOTS + 31) / 32)
#define RIPPLE_DEADLINE_NONE 0
#define RIPPLE_DEADLINE_ARMED 1
#define RIPPLE_DEADLINE_EXPIRED 2
#define RIPPLE_DEADLINE_ERROR 3
struct ripple_deadline_time {
    uint32_t seconds_low, seconds_high, nanoseconds;
};
struct ripple_deadline_slot {
    struct ripple_deadline_time at;
    unsigned state, next;
};
struct ripple_deadline_queue {
    /* Links are slot + 1; a zero-initialized queue is empty. */
    unsigned head;
    struct ripple_deadline_slot slots[RIPPLE_DEADLINE_SLOTS];
};
struct ripple_deadline_ready { uint32_t bits[RIPPLE_DEADLINE_WORDS]; };
void ripple_deadline_cancel(struct ripple_deadline_queue *, unsigned slot);
/* Replaces an existing timer. Returns -1 for an invalid duration, slot or
 * unrepresentable 64-bit deadline; no timer remains armed on failure. */
int ripple_deadline_arm(struct ripple_deadline_queue *, unsigned slot,
    const struct ripple_deadline_time *, uint32_t seconds, uint32_t microseconds);
/* Pop all due timers, returning a bit per slot. NULL means clock failure:
 * wake every armed waiter with ERROR rather than silently hang indefinitely. */
void ripple_deadline_poll(struct ripple_deadline_queue *,
    const struct ripple_deadline_time *, struct ripple_deadline_ready *);
#endif
