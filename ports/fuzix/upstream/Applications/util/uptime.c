/* SPDX-License-Identifier: GPL-2.0-only */
/* Ripple adaptation of upstream uptime: monotonic guest time and process count.
 * The port has no login accounting; do not invent a utmp user count. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <syscalls.h>
#include <time.h>
#include "resources.h"

static void printload(unsigned n)
{
    printf(" %u.%02u", n / 100, n % 100);
}

int main(int argc, char **argv)
{
    struct timespec res;
    struct ripple_sysinfo info;
    unsigned loads[3];
    unsigned long days, hours, mins;
    int fd;
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: uptime\nDisplay guest uptime, process count, and load averages."); return 0;
    }
    if (argc != 1) { fputs("Usage: uptime\n", stderr); return 1; }
    if (clock_gettime(CLOCK_MONOTONIC, &res)) { perror("uptime: clock"); return 1; }
    fd = open("/dev/proc", O_RDONLY);
    if (fd < 0) { perror("uptime: /dev/proc"); return 1; }
    if (ioctl(fd, RIPPLE_SYSINFO, &info) || info.version != RIPPLE_SYSINFO_VERSION) {
        perror("uptime: system information"); close(fd); return 1;
    }
    close(fd);
    mins = res.tv_sec / 60; hours = mins / 60; days = hours / 24;
    printf("up %lu day%s, %02lu:%02lu:%02lu, %u/%u processes",
           days, days == 1 ? "" : "s", hours % 24, mins % 60,
           res.tv_sec % 60, info.processes, info.slots);
    if (getloadavg(loads, 3) == 3) {
        printf(", load average:"); printload(loads[0]); printload(loads[1]); printload(loads[2]);
    }
    putchar('\n');
    return 0;
}
