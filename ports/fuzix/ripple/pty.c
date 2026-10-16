/* SPDX-License-Identifier: GPL-2.0-only */
#include "platform.h"
#include <kdata.h>
#include <tty.h>
#include "pty.h"
#define QS 512
static uint8_t input[RIPPLE_PTY_COUNT][QS], output[RIPPLE_PTY_COUNT][QS];
static struct s_queue outq[RIPPLE_PTY_COUNT];
static uint8_t masters[RIPPLE_PTY_COUNT], opened[RIPPLE_PTY_COUNT];
static struct termios defaults;
static void queue_init(struct s_queue *q, uint8_t *buf) {
    q->q_base = q->q_head = q->q_tail = buf;
    q->q_size = QS; q->q_count = 0; q->q_wakeup = QS / 2;
}
void ripple_pty_init(void) {
    unsigned i;
    defaults = ttydata[1].termios;
    for (i = 0; i < RIPPLE_PTY_COUNT; i++) {
        queue_init(&ttyinq[i + 2], input[i]);
        queue_init(&outq[i], output[i]);
        termios_mask[i + 2] = 65535U;
    }
}
int ripple_pty_carrier(unsigned m) { return masters[m - 2]; }
int ripple_pty_ready(unsigned m) {
    return !masters[m - 2] || outq[m - 2].q_count < QS;
}
void ripple_pty_putc(unsigned m, unsigned c) {
    if (masters[m - 2]) {
        insq(&outq[m - 2], c & 255);
        wakeup(&outq[m - 2]);
    }
}
int ripple_slave_open(uint_fast8_t m, uint16_t f) {
    int r;
    if (!m || m > NUM_DEV_TTY || (m > 1 && !masters[m - 2])) {
        udata.u_error = ENXIO; return -1;
    }
    r = tty_open(m, f);
    if (!r && m > 1) { opened[m - 2] = 1; wakeup(&outq[m - 2]); }
    return r;
}
int ripple_slave_close(uint_fast8_t m) {
    int r = tty_close(m);
    if (m > 1) {
        wakeup(&outq[m - 2]);
        wakeup(&ttyinq[m].q_count);
    }
    return r;
}
int ripple_pty_open(uint_fast8_t m, uint16_t f) {
    struct tty *t;
    used(f);
    if (m >= RIPPLE_PTY_COUNT) { udata.u_error = ENXIO; return -1; }
    t = &ttydata[m + 2];
    if (masters[m] || t->users) { udata.u_error = EBUSY; return -1; }
    masters[m] = 1; opened[m] = 0;
    clrq(&ttyinq[m + 2]); clrq(&outq[m]);
    memset(t, 0, sizeof(*t));
    t->termios = defaults;
    t->winsize.ws_row = 24; t->winsize.ws_col = 80;
    return 0;
}
int ripple_pty_close(uint_fast8_t m) {
    masters[m] = 0;
    tty_hangup(m + 2);
    wakeup(&outq[m]); wakeup(&ttydata[m + 2]);
    wakeup(&ttyinq[m + 2]); wakeup(&ttyinq[m + 2].q_count);
    return 0;
}
int ripple_pty_read(uint_fast8_t m, uint_fast8_t raw, uint_fast8_t f) {
    unsigned n = 0, limit = udata.u_count > 32767UL ? 32767 : udata.u_count;
    uint_fast8_t c;
    struct s_queue *q = &outq[m];
    irqflags_t irq;
    used(raw);
    irq = di();
    while (n < limit) {
        if (remq(q, &c)) {
            if (udata.u_sysio) udata.u_base[n] = c;
            else if (uputc(c, udata.u_base + n)) break;
            ++n;
            wakeup(&ttydata[m + 2]);
        } else {
            if (n || (opened[m] && !ttydata[m + 2].users)) break;
            if (psleep_flags(q, f)) break;
        }
    }
    irqrestore(irq);
    if (n) { udata.u_error = 0; return n; }
    return udata.u_error ? -1 : 0;
}
int ripple_pty_write(uint_fast8_t m, uint_fast8_t raw, uint_fast8_t f) {
    unsigned n = 0, limit = udata.u_count > 32767UL ? 32767 : udata.u_count;
    unsigned c;
    irqflags_t irq;
    struct s_queue *q = &ttyinq[m + 2];
    used(raw);
    irq = di();
    while (n < limit) {
        if (opened[m] && !ttydata[m + 2].users) { udata.u_error = EIO; break; }
        if (q->q_count == q->q_size) {
            if (psleep_flags(&q->q_count, f)) break;
            continue;
        }
        c = udata.u_sysio ? udata.u_base[n] : _ugetc(udata.u_base + n);
        if (!tty_inproc(m + 2, c & 255)) break;
        ++n;
    }
    irqrestore(irq);
    if (n) { udata.u_error = 0; return n; }
    return udata.u_error ? -1 : 0;
}
int ripple_pty_ioctl(uint_fast8_t m, uarg_t r, char *p) {
    /* FIONREAD on a master describes slave output, not slave input. */
    if (r == TIOCINQ) return uputi(outq[m].q_count, p);
    if (r == TIOCSPGRP) { udata.u_error = ENOTTY; return -1; }
    return tty_ioctl(m + 2, r, p);
}
