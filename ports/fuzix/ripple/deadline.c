/* SPDX-License-Identifier: GPL-2.0-only */
#include "deadline.h"

static int before(const struct ripple_deadline_time *a,
                  const struct ripple_deadline_time *b) {
    if (a->seconds_high != b->seconds_high) return a->seconds_high < b->seconds_high;
    if (a->seconds_low != b->seconds_low) return a->seconds_low < b->seconds_low;
    return a->nanoseconds < b->nanoseconds;
}
void ripple_deadline_cancel(struct ripple_deadline_queue *q, unsigned slot) {
    unsigned *link;
    if (slot >= RIPPLE_DEADLINE_SLOTS) return;
    if (q->slots[slot].state == RIPPLE_DEADLINE_ARMED) {
        link = &q->head;
        while (*link && *link != slot + 1) link = &q->slots[*link - 1].next;
        if (*link) *link = q->slots[slot].next;
    }
    q->slots[slot].state = RIPPLE_DEADLINE_NONE;
    q->slots[slot].next = 0;
}
int ripple_deadline_arm(struct ripple_deadline_queue *q, unsigned slot,
    const struct ripple_deadline_time *now, uint32_t seconds, uint32_t usec) {
    struct ripple_deadline_slot *timer;
    uint32_t previous;
    unsigned *link;
    if (slot >= RIPPLE_DEADLINE_SLOTS) return -1;
    ripple_deadline_cancel(q, slot);
    timer = &q->slots[slot];
    if (!now || now->nanoseconds >= 1000000000UL || usec >= 1000000UL) return -1;
    timer->at = *now;
    previous = timer->at.seconds_low;
    timer->at.seconds_low += seconds;
    if (timer->at.seconds_low < previous) {
        if (timer->at.seconds_high == 0xffffffffUL) return -1;
        ++timer->at.seconds_high;
    }
    timer->at.nanoseconds += usec * 1000UL;
    if (timer->at.nanoseconds >= 1000000000UL) {
        timer->at.nanoseconds -= 1000000000UL;
        if (!++timer->at.seconds_low) {
            if (timer->at.seconds_high == 0xffffffffUL) return -1;
            ++timer->at.seconds_high;
        }
    }
    link = &q->head;
    while (*link && !before(&timer->at, &q->slots[*link - 1].at))
        link = &q->slots[*link - 1].next;
    timer->next = *link;
    timer->state = RIPPLE_DEADLINE_ARMED;
    *link = slot + 1;
    return 0;
}
uint32_t ripple_deadline_poll(struct ripple_deadline_queue *q,
                             const struct ripple_deadline_time *now) {
    uint32_t ready = 0;
    unsigned slot;
    while (q->head) {
        slot = q->head - 1;
        if (now && before(now, &q->slots[slot].at)) break;
        q->head = q->slots[slot].next;
        q->slots[slot].next = 0;
        q->slots[slot].state = now ? RIPPLE_DEADLINE_EXPIRED : RIPPLE_DEADLINE_ERROR;
        ready |= 1UL << slot;
    }
    return ready;
}
