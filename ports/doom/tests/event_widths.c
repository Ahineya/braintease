/* Automap notifications share the keyboard event queue but need all 32 bits.
 * A 16-bit data1 silently suppresses the status bar refresh on automap entry. */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include "am_map.h"

static event_t entered = { ev_keyup, AM_MSGENTERED, 0, 0 };
static event_t exited = { ev_keyup, AM_MSGEXITED, 0, 0 };
static event_t queued[4];

static int notification(const event_t *event)
{
    /* The same filter used by ST_Responder. Ordinary key events must never
     * be mistaken for one of the packed notifications. */
    if (event->type == ev_keyup
        && ((event->data1 & UINT32_C(0xffff0000)) == AM_MSGHEADER)) {
        switch (event->data1) {
            case AM_MSGENTERED: return 1;
            case AM_MSGEXITED: return 2;
        }
    }
    return 0;
}

int main(void)
{
    event_t key = { ev_keyup, 43, 9, 0 };
    event_t copied;
    if (sizeof(entered.data1) * CHAR_BIT != 32) return 1;
    if (AM_MSGHEADER != UINT32_C(0x616d0000)
        || AM_MSGENTERED != UINT32_C(0x616d6500)
        || AM_MSGEXITED != UINT32_C(0x616d7800)) return 2;
    queued[0] = entered;
    queued[1] = exited;
    queued[2] = key;
    copied = queued[0];
    if (notification(&copied) != 1 || notification(&queued[1]) != 2
        || notification(&queued[2]) != 0) return 3;
    if (queued[0].data1 != UINT32_C(0x616d6500)
        || queued[1].data1 != UINT32_C(0x616d7800)
        || queued[2].data1 != 43 || queued[2].data2 != 9) return 4;
    copied.type = ev_keydown;
    if (notification(&copied)) return 5;
    copied.type = ev_keyup;
    copied.data1 = 0x6500;
    if (notification(&copied)) return 6;
    puts("EVENT WIDTHS OK packed automap messages, queue copy, keyboard discrimination");
    return 0;
}
