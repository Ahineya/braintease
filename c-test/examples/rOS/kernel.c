#include <stdio.h>
#include "fat16.h"
#include "loader.h"
#include "sys.h"

static Fat16Fs g_kfs;
static unsigned g_app_imem = ROS_IMEM_APP;
static unsigned g_app_gp = 32;
static unsigned g_app_sb = 33;

int k_exit(int status) {
    (void)status;
    return 0;
}

int k_exec(char *path) {
    Fat16DirEnt ent;
    int r;

    r = fat16_resolve(&g_kfs, 0, path, &ent);
    if (r != FAT16_OK) {
        return r;
    }
    (void)g_app_imem;
    (void)g_app_gp;
    (void)g_app_sb;
    return sys1_load_and_enter(&g_kfs, &ent);
}

int main(void) {
    Fat16DirEnt ent;
    int r;

    r = fat16_mount(&g_kfs);
    if (r != FAT16_OK) {
        puts("KERNEL: no FAT16");
        return 1;
    }
    r = fat16_lookup(&g_kfs, 0, "COMMAND.COM", &ent);
    if (r != FAT16_OK) {
        puts("KERNEL: no COMMAND.COM");
        return 1;
    }
    sys1_load_and_enter(&g_kfs, &ent);
    puts("KERNEL: COMMAND returned");
    return 1;
}
