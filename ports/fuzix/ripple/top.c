/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "process-report.h"
#include <termios.h>
#include <signal.h>
#include <getopt.h>
static struct ripple_procinfo current[REPORT_SLOTS], previous[REPORT_SLOTS];
static struct ripple_sysinfo sys;
static struct termios terminal;
static int raw;
static struct termios running;
static volatile int quitting;
static void interrupted(int sig) { quitting = 1; signal(sig, interrupted); }
static void cleanup(void) {
    if (raw) { tcsetattr(0, TCSANOW, &terminal); printf("\033[?25h\033[?1049l"); fflush(stdout); }
}
static int number(char *s, unsigned *value, unsigned max) {
    char *end;
    unsigned long n = strtoul(s, &end, 10);
    if (!*s || *end || !n || n > max) return -1;
    *value = n; return 0;
}
static void suspended(int sig) {
    (void)sig;
    if (raw) { tcsetattr(0, TCSANOW, &terminal); printf("\033[?25h\033[?1049l"); fflush(stdout); }
    kill(getpid(), SIGTSTP);
    signal(SIGTSTP, suspended);
    if (raw) { tcsetattr(0, TCSANOW, &running); printf("\033[?1049h\033[?25l"); fflush(stdout); }
}
int main(int argc, char **argv) {
    int fd, opt, batch = 0, first = 1;
    unsigned frames = 0, delay = 1, frame = 0, i;
    unsigned cpu[REPORT_SLOTS], order[REPORT_SLOTS];
    while ((opt = getopt(argc, argv, "bn:d:")) != -1) {
        if (opt == 'b') batch = 1;
        else if (opt == 'n') { if (number(optarg, &frames, 32767)) goto usage; }
        else if (opt == 'd') { if (number(optarg, &delay, 3276)) goto usage; }
        else goto usage;
    }
    if (optind != argc) goto usage;
    if (!isatty(0) || !isatty(1)) { batch = 1; if (!frames) frames = 1; }
    fd = open("/dev/proc", O_RDONLY);
    if (fd < 0) { perror("top"); return 1; }
    if (!batch && tcgetattr(0, &terminal) == 0) {
        struct termios t = terminal;
        t.c_lflag &= ~(ICANON | ECHO); t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 1;
        if (tcsetattr(0, TCSANOW, &t) < 0) { perror("top"); return 1; }
        running = t;
        raw = 1; atexit(cleanup); printf("\033[?1049h\033[?25l");
    }
    signal(SIGTSTP, suspended);
    signal(SIGINT, interrupted); signal(SIGTERM, interrupted); signal(SIGHUP, interrupted);
    while (!quitting && (!frames || frame < frames)) {
        if (process_snapshot(fd, &sys, current) < 0) { perror("top"); return 1; }
        for (i = 0; i < sys.slots; i++) {
            unsigned long elapsed = current[i].sample_ticks - previous[i].sample_ticks;
            unsigned long before = previous[i].user_ticks + previous[i].system_ticks;
            unsigned long now = current[i].user_ticks + current[i].system_ticks;
            cpu[i] = 0; order[i] = i;
            if (!first && elapsed && current[i].pid == previous[i].pid && current[i].start_ticks == previous[i].start_ticks) {
                unsigned long value = (now - before) * 1000UL / elapsed;
                cpu[i] = value > 1000 ? 1000 : value;
            }
        }
        for (i = 1; i < sys.slots; i++) {
            unsigned j = i, slot = order[i];
            while (j && cpu[order[j - 1]] < cpu[slot]) { order[j] = order[j - 1]; --j; }
            order[j] = slot;
        }
        if (raw) printf("\033[H\033[J");
        printf("top - up %lus, %u/%u processes; user pool %lu/%lu KiB\n", sys.uptime_ticks / 10, sys.processes, sys.slots, sys.used_kib, sys.pool_kib);
        puts("  PID  PPID  PGID S  CPU%  MAP-K PRIV-K SHRD-K     TIME CMD");
        for (i = 0; i < sys.slots; i++) {
            unsigned slot = order[i];
            struct ripple_procinfo *p = &current[slot];
            if (!p->state) continue;
            printf("%5u %5u %5u %c %3u.%u %6lu %6lu %6lu ", p->pid, p->ppid, p->pgrp, process_state(p->state), cpu[slot] / 10, cpu[slot] % 10,
                   p->mapped_kib, p->private_kib, p->shared_kib);
            process_time(p); printf(" %s\n", p->name);
        }
        fflush(stdout); frame++; first = 0;
        memcpy(previous, current, sizeof(current));
        if (frames && frame == frames) break;
        if (raw) {
            __ktime_t t;
            unsigned long end;
            _time(&t, 1); end = t.low + delay * 10UL;
            do {
                char c;
                if (read(0, &c, 1) == 1 && (c == 'q' || c == 'Q' || c == 4)) quitting = 1;
                _time(&t, 1);
            } while (!quitting && (long)(t.low - end) < 0);
        } else sleep(delay);
    }
    close(fd); return 0;
usage:
    fputs("usage: top [-b] [-n COUNT] [-d SECONDS]\n", stderr); return 1;
}
