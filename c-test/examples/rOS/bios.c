#include <stdio.h>
#include "fat16.h"
#include "loader.h"
#include "bootmsg.h"

int main(void) {
    Fat16Fs fs;
    Fat16DirEnt ent;
    int r;

    boot_stage("rOS BIOS");
    boot_item("CPU", "Ripple 16-bit");

    r = fat16_mount(&fs);
    if (r != FAT16_OK) {
        boot_fail("no FAT16 volume");
        return 1;
    }
    boot_item("Disk", "FAT16");

    r = fat16_lookup(&fs, 0, "IO.SYS", &ent);
    if (r != FAT16_OK) {
        boot_fail("IO.SYS not found");
        return 1;
    }
    boot_load("IO.SYS");
    sys1_load_and_enter(&fs, &ent);
    boot_fail("IO.SYS returned");
    return 1;
}
