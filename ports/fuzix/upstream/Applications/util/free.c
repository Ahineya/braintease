/* SPDX-License-Identifier: GPL-2.0-only */
/* Ripple adaptation of the upstream free utility: physical KiB and COW usage. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <syscalls.h>
#include "resources.h"

int main(int argc, char **argv)
{
    struct ripple_sysinfo info;
    unsigned long reserved;
    int fd;
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: free\nDisplay physical memory, user pool, sharing, and process slots in KiB.");
        return 0;
    }
    if (argc != 1) { fputs("Usage: free\n", stderr); return 1; }
    fd = open("/dev/proc", O_RDONLY);
    if (fd < 0) { perror("free: /dev/proc"); return 1; }
    if (ioctl(fd, RIPPLE_SYSINFO, &info) || info.version != RIPPLE_SYSINFO_VERSION) {
        perror("free: memory information"); close(fd); return 1;
    }
    close(fd);
    reserved = info.ram_kib - info.pool_kib;
    puts("                   total       used       free  (KiB)");
    printf("Mem:          %10lu %10lu %10lu\n", info.ram_kib,
           reserved + info.used_kib, info.pool_kib - info.used_kib);
    printf("User pool:    %10lu %10lu %10lu\n", info.pool_kib,
           info.used_kib, info.pool_kib - info.used_kib);
    puts("Swap:                  0          0          0");
    printf("Reserved: %lu KiB; shared: %lu KiB; process mappings: %lu KiB\n",
           reserved, info.shared_kib, info.mapped_kib);
    printf("Processes: %u/%u; data/heap limit: %lu cells; stack: %lu cells\n",
           info.processes, info.slots, info.heap_cells, info.stack_cells);
    return 0;
}
