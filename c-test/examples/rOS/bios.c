#include <stdio.h>
#include "fat16.h"
#include "loader.h"

int main(void) {
    Fat16Fs fs;
    Fat16DirEnt ent;
    int r;

    r = fat16_mount(&fs);
    if (r != FAT16_OK) {
        puts("BIOS: no FAT16");
        return 1;
    }
    r = fat16_lookup(&fs, 0, "IO.SYS", &ent);
    if (r != FAT16_OK) {
        puts("BIOS: no IO.SYS");
        return 1;
    }
    sys1_load_and_enter(&fs, &ent);
    puts("BIOS: IO.SYS returned");
    return 1;
}
