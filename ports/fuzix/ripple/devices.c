/* SPDX-License-Identifier: GPL-2.0-only */
#include "disk.h"
#include "fat16.h"
#include "platform.h"
#include <kdata.h>
#include <mmio.h>
#include <vm_bulk.h>
#include <printf.h>
#include <tty.h>
#include "resources.h"
#include "pty.h"
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
    if (m == 1) kputchar(c);
    else ripple_pty_putc(m, c);
}
void tty_setup(uint_fast8_t m, uint_fast8_t f) {
    used(m);
    used(f);
}
int tty_carrier(uint_fast8_t m) {
    return m == 1 || ripple_pty_carrier(m);
}
void tty_sleeping(uint_fast8_t m) { used(m); }
void tty_data_consumed(uint_fast8_t m) { used(m); }
ttyready_t tty_writeready(uint_fast8_t m) {
    return m == 1 || ripple_pty_ready(m) ? TTY_READY_NOW : TTY_READY_LATER;
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
    ripple_pty_init();
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
        /* The descriptor mailbox is shared with every device and task. */
#if RIPPLE_BULK_IO
        {
            unsigned long ptr = (unsigned long)(udata.u_dptr + done);
            irqflags_t flags = di();
            unsigned status;
            if (write)
                status = vm_bulk_write(diskoff, (unsigned)(ptr >> 16), (unsigned)ptr, count, VM_BULK_WORDS);
            else
                status = vm_bulk_read(diskoff, (unsigned)(ptr >> 16), (unsigned)ptr, count, VM_BULK_WORDS);
            irqrestore(flags);
            if (status == VM_DEVICE_OK) goto transferred;
            if (status != VM_DEVICE_UNKNOWN_COMMAND && status != VM_DEVICE_UNSUPPORTED) {
                udata.u_error = EIO;
                return -1;
            }
        }
#endif
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
#if RIPPLE_BULK_IO
transferred:
#endif
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
        if (udata.u_sysio) {
            if (ripple_zero(udata.u_base, n)) return -1;
        } else if (uzero(udata.u_base, n))
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
        if (req == RIPPLE_PROCINFO) {
            struct ripple_procinfo info;
            unsigned slot;
            ptptr task;
            irqflags_t irq;
            if (uget(p, &slot, sizeof(slot)) ||
                !valaddr_w(p, sizeof(info), sizeof(info))) return -1;
            if (slot >= PTABSIZE) { udata.u_error = EINVAL; return -1; }
            memset(&info, 0, sizeof(info));
            info.slot = slot; info.version = RIPPLE_PROCINFO_VERSION;
            info.cpu_hz = TICKSPERSEC;
            irq = di();
            task = &ptab[slot];
            info.sample_ticks = ticks.full * (TICKSPERSEC / 10) + ticks_this_dsecond;
            if (task->p_status) {
                info.pid = task->p_pid; info.ppid = task->p_pptr->p_pid;
                info.pgrp = task->p_pgrp; info.session = task->p_session;
                info.uid = task->p_uid; info.tty = task->p_tty;
                info.state = task->p_status; info.priority = task->p_priority;
                info.nice = task->p_nice; info.start_ticks = task->p_time * (TICKSPERSEC / 10);
                info.user_ticks = task->p_utime; info.system_ticks = task->p_stime;
                memcpy(info.name, task->p_name, 8);
#if RIPPLE_MMU
                ripple_mmu_task_usage(slot, &info.private_kib, &info.shared_kib);
#else
                if (task->p_status != P_ZOMBIE) info.private_kib = 250;
#endif
                info.mapped_kib = info.private_kib + info.shared_kib;
            }
            irqrestore(irq);
            return uput(&info, p, sizeof(info));
        }
        if (req == RIPPLE_SYSINFO) {
            struct ripple_sysinfo info;
            unsigned i;
            irqflags_t irq;
            if (!valaddr_w((void *)p, sizeof(info), sizeof(info))) return -1;
            memset(&info, 0, sizeof(info));
            irq = di();
            info.version = RIPPLE_SYSINFO_VERSION;
            info.ram_kib = ramsize;
            info.pool_kib = procmem;
            info.used_kib = pagemap_mem_used();
            info.slots = PTABSIZE;
            info.heap_cells = (unsigned long)BANK_CELLS;
            info.stack_cells = BANK_CELLS;
            info.uptime_ticks = ticks.full;
            info.ticks_per_second = 10; /* ticks.full counts deciseconds. */
            for (i = 0; i < PTABSIZE; i++)
                if (ptab[i].p_status != P_EMPTY) ++info.processes;
#if RIPPLE_MMU
            info.flags = RIPPLE_SYSINFO_MMU;
            info.page_cells = MMU_PAGE_CELLS;
            info.heap_cells *= 16;
            ripple_mmu_usage(&info.shared_kib, &info.mapped_kib);
#else
            info.mapped_kib = info.used_kib;
#endif
            irqrestore(irq);
            return uput(&info, p, sizeof(info));
        }
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
    {ripple_slave_open, ripple_slave_close, tty_read, tty_write, tty_ioctl},
    {ripple_pty_open, ripple_pty_close, ripple_pty_read, ripple_pty_write, ripple_pty_ioctl}};
bool validdev(uint16_t d) { return major(d) < 4; }
