/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>
#include <stdio.h>
#include "fat16.h"
#include "disk.h"
#include <vm_bulk.h>
extern void imem_store(unsigned, unsigned, unsigned *);
extern void dmem_store(unsigned, unsigned, unsigned);
extern void boot_halt(void);
extern void ros_enter(unsigned, unsigned, unsigned, unsigned);
static Fat16Fs fs;
static Fat16DirEnt image;
static uint16_t cluster;
static uint32_t within, remain;
static void ready(void) {
    if (within == fs.cluster_bytes) {
        cluster = fat16_fat_next(&fs, cluster);
        within = 0;
    }
    if (cluster < 2 || cluster >= 0xfff0) {
        puts("Riplix boot: invalid chain");
        boot_halt();
        for (;;)
            ;
    }
}
static unsigned next(void) {
    unsigned v;
    if (!remain) return 0;
    ready();
    v = disk_read8(fat16_cluster_off(&fs, cluster) + within);
    within++;
    remain--;
    return v;
}
static unsigned word(void) {
    unsigned v = next();
    return v | (next() << 8);
}
static unsigned long wide(void) {
    unsigned long v = word();
    return v | ((unsigned long)word() << 16);
}
int main(void) {
    unsigned bs, cb, gp, sb, cells[4], addr, bank;
    unsigned fast_code = RIPPLE_BULK_IO, fast_data = RIPPLE_BULK_IO;
    unsigned n, status;
    unsigned long bytes;
    unsigned long entry, count, data, i;
    puts("Ripple Riplix BIOS");
    if (fat16_mount(&fs) || fat16_lookup(&fs, 0, "FUZIX.SYS", &image)) {
        puts("FUZIX.SYS missing");
        return 1;
    }
    cluster = image.cluster;
    within = 0;
    remain = image.size;
    if (next() != 'S' || next() != 'Y' || next() != 'S' || next() != '1') {
        puts("bad SYS1");
        return 1;
    }
    bs = word();
    cb = word();
    gp = word();
    sb = word();
    entry = wide();
    count = wide();
    data = wide();
    if (bs != 64000 || cb != 1 || gp != 3 || sb != 32 || !count || count > 31UL * 64000UL ||
        entry >= count || data > 64000 || 24 + count * 8 + data != image.size) {
        puts("bad kernel layout");
        return 1;
    }
    puts("Riplix kernel loading");
    addr = 0;
    bank = cb;
    i = 0;
    while (i < count) {
        if (fast_code) {
            ready();
            n = (unsigned)((fs.cluster_bytes - within) / 8);
            if (n > VM_BULK_MAX_INSTRUCTIONS) n = VM_BULK_MAX_INSTRUCTIONS;
            if ((unsigned long)n > count - i) n = (unsigned)(count - i);
            status = vm_bulk_read(fat16_cluster_off(&fs, cluster) + within,
                bank, addr, n, VM_BULK_CODE_WORDS);
            if (!n || (status != VM_DEVICE_OK && status != VM_DEVICE_UNKNOWN_COMMAND && status != VM_DEVICE_UNSUPPORTED)) {
                puts("Riplix boot: code transfer failed"); return 1;
            }
            if (status == VM_DEVICE_OK) {
                bytes = (unsigned long)n * 8;
                within += bytes; remain -= bytes; i += n;
                if (n >= 64000U - addr) { bank++; addr = n - (64000U - addr); }
                else addr += n;
                continue;
            }
            fast_code = 0;
        }
        cells[0] = next();
        next();
        cells[1] = word();
        cells[2] = word();
        cells[3] = word();
        imem_store(bank, addr, cells);
        i++;
        if (++addr == 64000) {
            addr = 0;
            bank++;
        }
    }
    i = 0;
    while (i < 64000UL) {
        if (fast_data) {
            n = (unsigned)(64000UL - i);
            if (n > VM_BULK_MAX_CELLS) n = VM_BULK_MAX_CELLS;
            if (i < data) {
                ready();
                if ((unsigned long)n > data - i) n = (unsigned)(data - i);
                if ((unsigned long)n > fs.cluster_bytes - within) n = (unsigned)(fs.cluster_bytes - within);
                status = vm_bulk_read(fat16_cluster_off(&fs, cluster) + within,
                    gp, (unsigned)i, n, VM_BULK_OCTETS);
            } else status = vm_bulk_fill(gp, (unsigned)i, n, 0);
            if (status == VM_DEVICE_OK) {
                if (i < data) { within += n; remain -= n; }
                i += n;
                continue;
            }
            if (status != VM_DEVICE_UNKNOWN_COMMAND && status != VM_DEVICE_UNSUPPORTED) {
                puts("Riplix boot: data transfer failed"); return 1;
            }
            fast_data = 0;
        }
        dmem_store(gp, (unsigned)i, i < data ? next() : 0);
        i++;
    }
    puts("Riplix kernel starting");
    ros_enter(cb + (unsigned)(entry / 64000UL), (unsigned)(entry % 64000UL), gp, sb);
    return 1;
}
