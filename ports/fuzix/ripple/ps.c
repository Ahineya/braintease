/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "process-report.h"
#include <getopt.h>
#include <sys/stat.h>
#include <pwd.h>
static struct ripple_procinfo procs[REPORT_SLOTS];
static struct ripple_sysinfo sys;
static int selected(const char *list, unsigned value, int kind) {
    const char *s = list, *next;
    char token[64], *end;
    unsigned long n;
    int match = 0;
    if (!s || !*s) return -1;
    while (s) {
        unsigned length;
        next = strchr(s, ','); length = next ? next - s : strlen(s);
        if (!length || length >= sizeof(token)) return -1;
        memcpy(token, s, length); token[length] = 0;
        n = strtoul(token, &end, 10);
        if (end == token || *end) {
            if (kind == 1) {
                struct passwd *pw = getpwnam(token);
                if (!pw) return -1;
                n = pw->pw_uid;
            } else if (kind == 2) {
                struct stat st;
                char path[80];
                if (token[0] == '/') strcpy(path, token);
                else { strcpy(path, "/dev/"); strcat(path, token); }
                if (stat(path, &st) < 0 || !S_ISCHR(st.st_mode) || major(st.st_rdev) != 2) return -1;
                n = minor(st.st_rdev);
            } else return -1;
        }
        if (n > 32767) return -1;
        if (n == value) match = 1;
        s = next ? next + 1 : NULL;
        if (s && !*s) return -1;
    }
    return match;
}
int main(int argc, char **argv) {
    int fd, opt, all = 0, full = 0, detail = 0, run = 0, invert = 0, terminals = 0;
    unsigned i, mine = 0, show;
    char *pids = NULL, *uids = NULL, *ttys = NULL;
    while ((opt = getopt(argc, argv, "AeaNrlfp:u:U:t:")) != -1) {
        switch (opt) {
        case 'A': case 'e': all = 1; break;
        case 'a': terminals = 1; break;
        case 'N': invert = 1; break;
        case 'r': run = 1; break;
        case 'l': detail = full = 1; break;
        case 'f': full = 1; break;
        case 'p': pids = optarg; break;
        case 'u': case 'U': uids = optarg; break;
        case 't': ttys = optarg; break;
        default: goto usage;
        }
    }
    if (optind != argc) goto usage;
    if ((pids && selected(pids, 65535, 0) < 0) || (uids && selected(uids, 65535, 1) < 0) ||
        (ttys && selected(ttys, 65535, 2) < 0)) goto usage;
    fd = open("/dev/proc", O_RDONLY);
    if (fd < 0 || process_snapshot(fd, &sys, procs) < 0) { perror("ps"); return 1; }
    close(fd);
    for (i = 0; i < sys.slots; i++) if (procs[i].pid == getpid()) mine = procs[i].tty;
    if (full) printf("  UID   PID  PPID  PGID   SID S TTY      TIME  MAP-K");
    else printf("  PID S TTY      TIME  MAP-K");
    if (detail) printf(" PRIV-K SHRD-K PRI NI");
    puts(" CMD");
    for (i = 0; i < sys.slots; i++) {
        struct ripple_procinfo *p = &procs[i];
        if (!p->state) continue;
        show = all || (terminals && p->tty) || (pids && selected(pids, p->pid, 0) == 1) ||
               (uids && selected(uids, p->uid, 1) == 1) || (ttys && selected(ttys, p->tty, 2) == 1);
        if (!all && !terminals && !pids && !uids && !ttys) show = p->uid == getuid() && p->tty == mine;
        if (run && p->state != 1 && p->state != 2) show = 0;
        if (invert) show = !show;
        if (!show) continue;
        if (full) printf("%5u %5u %5u %5u %5u ", p->uid, p->pid, p->ppid, p->pgrp, p->session);
        else printf("%5u ", p->pid);
        printf("%c %3u ", process_state(p->state), p->tty);
        process_time(p);
        printf(" %6lu", p->mapped_kib);
        if (detail) printf(" %6lu %6lu %3u %2u", p->private_kib, p->shared_kib, p->priority, p->nice);
        printf(" %s%s\n", p->name, p->state == 7 ? " <defunct>" : "");
    }
    return 0;
usage:
    fputs("usage: ps [-AeaNrlf] [-p PID,...] [-u USER,...] [-t TTY,...]\n", stderr);
    return 1;
}
