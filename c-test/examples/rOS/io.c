#include <stdio.h>
#include <mmio.h>
#include "fat16.h"
#include "disk.h"
#include "loader.h"

int io_disk_read8(unsigned long off) {
    return disk_read8(off);
}

int io_disk_write8(unsigned long off, int v) {
    disk_write8(off, (unsigned char)v);
    return 0;
}

unsigned io_disk_read16(unsigned long off) {
    return disk_read16(off);
}

unsigned long io_disk_read32(unsigned long off) {
    return disk_read32(off);
}

void io_disk_read(unsigned long off, unsigned char *buf, unsigned n) {
    disk_read(off, buf, (unsigned short)n);
}

int main(void) {
    Fat16Fs fs;
    Fat16DirEnt ent;
    int r;

    display_set_mode(1);

    r = fat16_mount(&fs);
    if (r != FAT16_OK) {
        puts("IO: no FAT16");
        return 1;
    }
    r = fat16_lookup(&fs, 0, "KERNEL.SYS", &ent);
    if (r != FAT16_OK) {
        puts("IO: no KERNEL.SYS");
        return 1;
    }
    sys1_load_and_enter(&fs, &ent);
    puts("IO: KERNEL returned");
    return 1;
}
