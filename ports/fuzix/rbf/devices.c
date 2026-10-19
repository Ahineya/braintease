/* SPDX-License-Identifier: GPL-2.0-only */
#include <kernel.h>
#include <kdata.h>
#include <tty.h>
#include <mmio.h>
#include <vm_bulk.h>

static uint8_t input[512];
struct s_queue ttyinq[NUM_DEV_TTY + 1];
tcflag_t termios_mask[NUM_DEV_TTY + 1] = {0, 65535U};
void kputchar(uint_fast8_t c) { mmio_write(0, c & 255); }
void tty_putc(uint_fast8_t m, uint_fast8_t c) { if (m == 1) kputchar(c); }
void tty_setup(uint_fast8_t m, uint_fast8_t f) { used(m); used(f); }
int tty_carrier(uint_fast8_t m) { return m == 1; }
void tty_sleeping(uint_fast8_t m) { used(m); }
void tty_data_consumed(uint_fast8_t m) { used(m); }
ttyready_t tty_writeready(uint_fast8_t m) { used(m); return TTY_READY_NOW; }
int ripple_tty_write_run(uint_fast8_t m) { used(m); return 0; }
void device_init(void) {
    struct s_queue *q = &ttyinq[1];
    q->q_base = q->q_head = q->q_tail = input;
    q->q_size = sizeof(input);
    q->q_wakeup = sizeof(input) / 2;
}
#ifdef RBF_SHELL
/* Poll only while the process sleeps. Stop after one complete canonical line:
 * pasted commands remain in the host queue instead of overflowing the tty. */
void rbf_poll(void) {
    unsigned n = 0, c;
    while (n++ < 32 &&
           (mmio_read(MMIO_TTY_IN_STATUS) & TTY_STATUS_HAS_BYTE)) {
        c = mmio_read(MMIO_TTY_IN_POP) & 255;
        /* Modern terminals send DEL for Backspace; FUZIX defaults to BS. */
        if (c == 127 && (ttydata[1].termios.c_lflag & ICANON))
            c = ttydata[1].termios.c_cc[VERASE];
        tty_inproc(1, c);
        if (c == '\n' || c == '\r' || c == 4) break;
    }
}
#endif
static int disk_open(uint_fast8_t m, uint16_t flags) {
    used(flags);
    if (!m) return 0;
    udata.u_error = ENXIO;
    return -1;
}
static int disk_read(uint_fast8_t m, uint_fast8_t raw, uint_fast8_t flags) {
    unsigned long address = (unsigned long)udata.u_dptr;
    unsigned status;
    used(flags);
    if (m || raw) { udata.u_error = ENXIO; return -1; }
    if (udata.u_block >= RBF_DISK_BLOCKS) { udata.u_error = EIO; return -1; }
    status = vm_bulk_read((unsigned long)udata.u_block * (2UL * BLKSIZE),
        (unsigned)(address >> 16), (unsigned)address, BLKSIZE, VM_BULK_WORDS);
    if (status != VM_DEVICE_OK) { udata.u_error = EIO; return -1; }
    return BLKSIZE;
}
#ifdef RBF_SHELL
static int disk_write(uint_fast8_t m,uint_fast8_t raw,uint_fast8_t flags) {
    unsigned long address=(unsigned long)udata.u_dptr;
    used(flags);
    if (m || raw) { udata.u_error=ENXIO; return -1; }
    if (udata.u_block>=RBF_DISK_BLOCKS ||
        vm_bulk_write((unsigned long)udata.u_block*(2UL*BLKSIZE),
          address>>16,(unsigned)address,BLKSIZE,VM_BULK_WORDS)!=VM_DEVICE_OK) {
        udata.u_error=EIO; return -1;
    }
    return BLKSIZE;
}
#else
static int read_only(uint_fast8_t m, uint_fast8_t raw, uint_fast8_t flags) {
    used(m); used(raw); used(flags);
    udata.u_error = EROFS;
    return -1;
}

#define disk_write read_only
#endif
struct devsw dev_tab[] = {
    {disk_open, no_close, disk_read, disk_write, no_ioctl},
    {nxio_open, no_close, no_rdwr, no_rdwr, no_ioctl},
    {tty_open, tty_close, tty_read, tty_write, tty_ioctl}
};
bool validdev(uint16_t dev) {
    return dev == 0 || dev == TTYDEV;
}
