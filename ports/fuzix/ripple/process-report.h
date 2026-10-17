/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_PROCESS_REPORT_H
#define RIPPLE_PROCESS_REPORT_H
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "resources.h"
#define REPORT_SLOTS 32
static char process_state(unsigned state) {
    if (state == 1 || state == 2) return 'R';
    if (state == 3) return 'S';
    if (state == 4) return 'D';
    if (state == 5) return 'T';
    if (state == 6) return 'F';
    if (state == 7) return 'Z';
    return '?';
}
static int process_snapshot(int fd, struct ripple_sysinfo *sys, struct ripple_procinfo *p) {
    unsigned i, n;
    if (ioctl(fd, RIPPLE_SYSINFO, sys) < 0 || sys->version != RIPPLE_SYSINFO_VERSION ||
        sys->slots > REPORT_SLOTS) return -1;
    for (i = 0; i < sys->slots; i++) {
        memset(&p[i], 0, sizeof(p[i])); p[i].slot = i;
        if (ioctl(fd, RIPPLE_PROCINFO, &p[i]) < 0 || p[i].version != RIPPLE_PROCINFO_VERSION) return -1;
        for (n = 0; n < 8; n++) if (p[i].name[n] && (p[i].name[n] < 32 || p[i].name[n] > 126)) p[i].name[n] = '?';
        p[i].name[8] = 0;
    }
    return 0;
}
static void process_time(struct ripple_procinfo *p) {
    unsigned long seconds = (p->user_ticks + p->system_ticks) / p->cpu_hz;
    printf("%02lu:%02lu:%02lu", seconds / 3600, seconds / 60 % 60, seconds % 60);
}
#endif
