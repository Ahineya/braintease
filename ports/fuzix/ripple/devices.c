/* SPDX-License-Identifier: GPL-2.0-only */
#include "disk.h"
#include "fat16.h"
#include "platform.h"
#include <kdata.h>
#include <mmio.h>
#include <printf.h>
#include <tty.h>
static Fat16Fs outer;
static Fat16DirEnt rootfile;
static uint16_t clusters[16384];
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
    if (rootfile.size < 4UL * BLKSIZE || rootfile.size % (2UL * BLKSIZE))
        panic("root image size");
    nclusters = (unsigned)((rootfile.size + outer.cluster_bytes - 1) /
                           outer.cluster_bytes);
    if (nclusters > 16384)
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
static int root_io(uint_fast8_t m, uint_fast8_t raw, uint_fast8_t f,
                   int write) {
    unsigned long off, diskoff;
    unsigned done = 0, count;
    used(f);
    if (m || raw) {
        udata.u_error = ENXIO;
        return -1;
    }
    off = (unsigned long)udata.u_block * (2UL * BLKSIZE);
    if (off + 2UL * BLKSIZE > rootfile.size) {
        udata.u_error = EIO;
        return -1;
    }
    /* FAT clusters need not align with the host's 64-KiB storage windows.
       Split larger filesystem blocks at both kinds of boundary. */
    while (done < BLKSIZE) {
        diskoff = fat16_cluster_off(
                      &outer, clusters[(unsigned)(off / outer.cluster_bytes)]) +
                  off % outer.cluster_bytes;
        count = BLKSIZE - done;
        if ((unsigned long)count * 2 > 65536UL - (diskoff & 65535UL))
            count = (unsigned)((65536UL - (diskoff & 65535UL)) >> 1);
        if ((unsigned long)count * 2 > outer.cluster_bytes - off % outer.cluster_bytes)
            count = (unsigned)((outer.cluster_bytes - off % outer.cluster_bytes) >> 1);
        if (write)
            storage_write_buffer((unsigned)(diskoff >> 16),
                                 (unsigned)(diskoff & 65535UL) >> 1,
                                 udata.u_dptr + done, count);
        else
            storage_read_buffer((unsigned)(diskoff >> 16),
                                (unsigned)(diskoff & 65535UL) >> 1,
                                udata.u_dptr + done, count);
        if (storage_get_status() & STORE_CTL_ERROR) {
            udata.u_error = EIO;
            return -1;
        }
        done += count;
        off += (unsigned long)count * 2;
    }
    return BLKSIZE;
}
static int root_read(uint_fast8_t m, uint_fast8_t r, uint_fast8_t f) {
    return root_io(m, r, f, 0);
}
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
/* Standard FUZIX system-device minors, restricted to null, zero and proc.
 * Kernel/physical memory access is deliberately not exposed by this port. */
static int system_open(uint_fast8_t m, uint16_t f) {
    used(f);
    if (m == 0 || m == 2 || m == 3)
        return 0;
    udata.u_error = ENXIO;
    return -1;
}
static int system_read(uint_fast8_t m, uint_fast8_t raw, uint_fast8_t f) {
    usize_t n = udata.u_count > 32767UL ? 32767UL : udata.u_count;
    irqflags_t irq;
    used(raw);
    used(f);
    if (m == 0)
        return 0;
    if (m == 2) {
        if (udata.u_sysio)
            memset(udata.u_base, 0, n);
        else if (uzero(udata.u_base, n))
            return -1;
        return n;
    }
    if (m == 3) {
        if (udata.u_offset < 0 || udata.u_offset >= sizeof(ptab))
            return 0;
        if (n > sizeof(struct p_tab))
            n = sizeof(struct p_tab);
        if (n > sizeof(ptab) - udata.u_offset)
            n = sizeof(ptab) - udata.u_offset;
        irq = di();
        if (uput((char *)ptab + udata.u_offset, udata.u_base, n)) {
            irqrestore(irq);
            return -1;
        }
        irqrestore(irq);
        return umove(n);
    }
    udata.u_error = ENXIO;
    return -1;
}
static int system_write(uint_fast8_t m, uint_fast8_t raw, uint_fast8_t f) {
    used(raw);
    used(f);
    if (m == 0 || m == 2)
        return udata.u_count > 32767UL ? 32767 : (int)udata.u_count;
    udata.u_error = EINVAL;
    return -1;
}
static int system_ioctl(uint_fast8_t m, uarg_t req, char *p) {
    if (m == 3) {
        if (req == 1)
            return uputi(PTABSIZE, p);
        if (req == 2)
            return uputi(sizeof(struct p_tab), p);
    }
    udata.u_error = ENOTTY;
    return -1;
}

struct devsw dev_tab[] = {
    {root_open, no_close, root_read, root_write, root_ioctl},
    {system_open, no_close, system_read, system_write, system_ioctl},
    {tty_open, tty_close, tty_read, tty_write, tty_ioctl}};
bool validdev(uint16_t d) { return major(d) < 3; }
