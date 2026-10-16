/* SPDX-License-Identifier: GPL-2.0-only */
#include "platform.h"
#include <kdata.h>
#include <printf.h>
#include <tty.h>
#include <mmio.h>
#include "fat16.h"
#include "disk.h"
static Fat16Fs outer;
static Fat16DirEnt rootfile;
static uint16_t clusters[4096];
static unsigned nclusters;
static uint8_t ttybuf[4096];
static uint16_t seen[4096];
struct s_queue ttyinq[NUM_DEV_TTY + 1];
tcflag_t termios_mask[NUM_DEV_TTY + 1] = {0, 65535U};
void kputchar(uint_fast8_t c) { tty_putchar(c & 255); }
void tty_putc(uint_fast8_t m, uint_fast8_t c) {
    used(m);
    kputchar(c);
}
void tty_setup(uint_fast8_t m, uint_fast8_t f) {
    used(m);
    used(f);
}
int tty_carrier(uint_fast8_t m) {
    used(m);
    return 1;
}
void tty_sleeping(uint_fast8_t m) { used(m); }
void tty_data_consumed(uint_fast8_t m) { used(m); }
ttyready_t tty_writeready(uint_fast8_t m) {
    used(m);
    return TTY_READY_NOW;
}
void ripple_poll(void) {
    unsigned c, n = 0;
    irqflags_t flags;
    if (!ttyinq[1].q_size)
        return;
    /* Idle and timer paths both poll. Keep pop+enqueue ordered as one
     * operation so an IRQ cannot enqueue later characters ahead of this one. */
    flags = di();
    while (n++ < 32 && (mmio_read(MMIO_TTY_IN_STATUS) & TTY_STATUS_HAS_BYTE)) {
        c = mmio_read(MMIO_TTY_IN_POP);
        tty_inproc(1, c & 255);
    }
    irqrestore(flags);
}

void ripple_disk_init(void) {
    uint16_t c;
    struct s_queue *q = &ttyinq[1];
    q->q_base = q->q_head = q->q_tail = ttybuf;
    q->q_size = 4096;
    q->q_count = 0;
    q->q_wakeup = 2048;
    if (fat16_mount(&outer) || fat16_lookup(&outer, 0, "ROOTFS.IMG", &rootfile))
        panic("ROOTFS.IMG missing");
    if (rootfile.size < 1024UL || rootfile.size % 512UL)
        panic("root image size");
    nclusters = (unsigned)((rootfile.size + outer.cluster_bytes - 1) / outer.cluster_bytes);
    if (nclusters > 4096)
        panic("root image too big");
    c = rootfile.cluster;
    {
        unsigned i;
        for (i = 0; i < nclusters; i++) {
            if (c < 2 || c >= 0xfff0)
                panic("root chain short");
            if (seen[c >> 4] & (1U << (c & 15)))
                panic("root chain cycle");
            seen[c >> 4] |= 1U << (c & 15);
            clusters[i] = c;
            c = fat16_fat_next(&outer, c);
        }
    }
    if (!fat16_is_eof(c))
        panic("root chain long");
}
static int root_open(uint_fast8_t m, uint16_t f) {
    used(f);
    return m ? -1 : 0;
}
static int root_io(uint_fast8_t m, uint_fast8_t raw, uint_fast8_t f, int write) {
    unsigned long off, diskoff;
    used(f);
    if (m || raw) {
        udata.u_error = ENXIO;
        return -1;
    }
    off = (unsigned long)udata.u_block * 512UL;
    if (off + 512UL > rootfile.size) {
        udata.u_error = EIO;
        return -1;
    }
    diskoff = fat16_cluster_off(&outer, clusters[(unsigned)(off / outer.cluster_bytes)]) +
              off % outer.cluster_bytes;
    if (write)
        storage_write_buffer((unsigned)(diskoff >> 16), (unsigned)(diskoff & 65535UL) >> 1,
                             udata.u_dptr, BLKSIZE);
    else
        storage_read_buffer((unsigned)(diskoff >> 16), (unsigned)(diskoff & 65535UL) >> 1,
                            udata.u_dptr, BLKSIZE);
    if (storage_get_status() & STORE_CTL_ERROR) {
        udata.u_error = EIO;
        return -1;
    }
    return BLKSIZE;
}
static int root_read(uint_fast8_t m, uint_fast8_t r, uint_fast8_t f) { return root_io(m, r, f, 0); }
static int root_write(uint_fast8_t m, uint_fast8_t r, uint_fast8_t f) {
    return root_io(m, r, f, 1);
}
static int root_ioctl(uint_fast8_t m, uarg_t req, char *p) {
    used(m);
    used(p);
    if (req == BLKFLSBUF) {
        storage_commit_all();
        if (storage_get_status() & STORE_CTL_ERROR) {
            udata.u_error = EIO;
            return -1;
        }
        return 0;
    }
    udata.u_error = ENOTTY;
    return -1;
}
struct devsw dev_tab[] = {{root_open, no_close, root_read, root_write, root_ioctl},
                          {nxio_open, no_close, no_rdwr, no_rdwr, no_ioctl},
                          {tty_open, tty_close, tty_read, tty_write, tty_ioctl}};
bool validdev(uint16_t d) { return major(d) < 3; }
