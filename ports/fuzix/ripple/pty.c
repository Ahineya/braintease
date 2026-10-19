/* SPDX-License-Identifier: GPL-2.0-only */
#include "platform.h"
#include <kdata.h>
#include <tty.h>
#include "pty.h"
#include "graphics.h"
#include <gx.h>
#define QS 512
#ifdef CONFIG_FUSE_X
#include <native_process.h>
/* Queues are shared kernel storage, not private/COW userspace heap. Keep the
 * larger pool out of the hosted kernel's single static-global bank. */
static uint8_t (*input)[QS], (*output)[QS];
#else
static uint8_t input[RIPPLE_PTY_COUNT][QS], output[RIPPLE_PTY_COUNT][QS];
#endif
static struct s_queue outq[RIPPLE_PTY_COUNT];
static uint8_t masters[RIPPLE_PTY_COUNT], opened[RIPPLE_PTY_COUNT];
static struct termios defaults;
unsigned ripple_pty_status(unsigned m) {
    unsigned status=0;
    if(outq[m].q_count || (opened[m]&&!ttydata[m+2].users))status|=GX_READY_READ;
    if(ttyinq[m+2].q_count<ttyinq[m+2].q_size || (opened[m]&&!ttydata[m+2].users))status|=GX_READY_WRITE;
    return status;
}
unsigned ripple_pty_select(unsigned m) {
    unsigned s;
    if(m>=RIPPLE_PTY_COUNT)return SELECT_IN|SELECT_OUT;
    s=ripple_pty_status(m);
    return (s&GX_READY_READ?SELECT_IN:0)|(s&GX_READY_WRITE?SELECT_OUT:0);
}
static void queue_init(struct s_queue *q, uint8_t *buf) {
    q->q_base = q->q_head = q->q_tail = buf;
    q->q_size = QS; q->q_count = 0; q->q_wakeup = QS / 2;
}
void ripple_pty_init(void) {
    unsigned i;
#ifdef CONFIG_FUSE_X
    input = rcc_process_kernel_alloc(2UL * RIPPLE_PTY_COUNT * QS);
    if (!input) panic("PTY queues");
    output = input + RIPPLE_PTY_COUNT;
#endif
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
        if(outq[m-2].q_count==1) {
            wakeup(&outq[m-2]); ripple_gfx_wakeup();
            selwake_dev(3,m-2,SELECT_IN);
        }
    }
}
/* Called only after foreground/STOP/DEAD checks in tty_write. */
int ripple_pty_write_run(unsigned minor, const uint8_t *data, unsigned n, unsigned sysio)
{
    unsigned m=minor-2, empty;
    int result;
    irqflags_t irq=di();
    if (!masters[m]) { irqrestore(irq); return 0; }
    empty=!outq[m].q_count;
    result=ripple_queue_write(&outq[m],data,n,sysio);
    if (result>0 && empty) {
        wakeup(&outq[m]); ripple_gfx_wakeup(); selwake_dev(3,m,SELECT_IN);
    }
    irqrestore(irq); return result;
}
int ripple_slave_open(uint_fast8_t m, uint16_t f) {
    int r;
    if (!m || m > NUM_DEV_TTY || (m > 1 && !masters[m - 2])) {
        udata.u_error = ENXIO; return -1;
    }
    r = tty_open(m, f);
    if (!r && m > 1) { opened[m - 2] = 1; wakeup(&outq[m - 2]); selwake_dev(3,m-2,SELECT_IN|SELECT_OUT); }
    return r;
}
int ripple_slave_close(uint_fast8_t m) {
    int r = tty_close(m);
    if (m > 1) {
        wakeup(&outq[m - 2]);
        wakeup(&ttyinq[m].q_count);
        ripple_gfx_wakeup();
        selwake_dev(3,m-2,SELECT_IN|SELECT_OUT);
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
    struct s_queue *q = &outq[m];
    irqflags_t irq;
    used(raw);
    irq = di();
    while (n < limit) {
        if (q->q_count) {
            unsigned was_full=q->q_count==QS;
            int got=ripple_queue_read(q,udata.u_base+n,limit-n,udata.u_sysio);
            if (got<0) break;
            n+=got;
            wakeup(&ttydata[m + 2]);
            if(was_full)selwake_dev(2,m+2,SELECT_OUT);
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
        if (!(ttydata[m+2].termios.c_lflag & (ICANON|ISIG|ECHO)) &&
            !(ttydata[m+2].termios.c_iflag & (ISTRIP|IGNCR|ICRNL|INLCR|IXON))) {
            int got=ripple_queue_write(q,udata.u_base+n,limit-n,udata.u_sysio);
            if (got<0) break;
            n+=got;
            wakeup(q);selwake_dev(2,m+2,SELECT_IN);
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
