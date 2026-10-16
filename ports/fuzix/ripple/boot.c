/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>
#include <stdio.h>
#include "fat16.h"
#include "disk.h"
extern void imem_store(unsigned, unsigned, unsigned *);
extern void dmem_store(unsigned, unsigned, unsigned);
extern void boot_halt(void);
extern void ros_enter(unsigned, unsigned, unsigned, unsigned);
static Fat16Fs fs;
static Fat16DirEnt image;
static uint16_t cluster;
static uint32_t within, remain;
static unsigned next(void) {
    unsigned v;
    if (!remain)
        return 0;
    if (within == fs.cluster_bytes) {
        cluster = fat16_fat_next(&fs, cluster);
        within = 0;
    }
    if (cluster < 2 || cluster >= 0xfff0) {
        puts("FUZIX boot: invalid chain");
        boot_halt();
        for (;;)
            ;
    }
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
    unsigned long entry, count, data, i;
    puts("Ripple FUZIX BIOS");
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
    puts("FUZIX.SYS loading");
    addr = 0;
    bank = cb;
    for (i = 0; i < count; i++) {
        cells[0] = next();
        next();
        cells[1] = word();
        cells[2] = word();
        cells[3] = word();
        imem_store(bank, addr, cells);
        if (++addr == 64000) {
            addr = 0;
            bank++;
        }
    }
    for (i = 0; i < 64000UL; i++)
        dmem_store(gp, (unsigned)i, i < data ? next() : 0);
    puts("FUZIX.SYS starting");
    ros_enter(cb + (unsigned)(entry / 64000UL), (unsigned)(entry % 64000UL), gp, sb);
    return 1;
}
