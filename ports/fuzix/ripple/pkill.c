/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Signal guest processes using the same bounded snapshot as ps. */
#include "process-report.h"
#include <signal.h>
#include <regexp.h>
#include <errno.h>

static struct ripple_procinfo procs[REPORT_SLOTS];
static struct ripple_sysinfo sys;
static const struct signal_name { const char *name; int number; } signals[] = {
    {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ILL", SIGILL},
    {"TRAP", SIGTRAP}, {"ABRT", SIGABRT}, {"IOT", SIGIOT}, {"BUS", SIGBUS},
    {"FPE", SIGFPE}, {"KILL", SIGKILL}, {"USR1", SIGUSR1}, {"SEGV", SIGSEGV},
    {"USR2", SIGUSR2}, {"PIPE", SIGPIPE}, {"ALRM", SIGALRM}, {"TERM", SIGTERM},
    {"STKFLT", SIGSTKFLT}, {"CHLD", SIGCHLD}, {"CONT", SIGCONT}, {"STOP", SIGSTOP},
    {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN}, {"TTOU", SIGTTOU}, {"URG", SIGURG},
    {"XCPU", SIGXCPU}, {"XFSZ", SIGXFSZ}, {"VTALRM", SIGVTALRM}, {"PROF", SIGPROF},
    {"WINCH", SIGWINCH}, {"IO", SIGIO}, {"POLL", SIGPOLL}, {"PWR", SIGPWR},
    {"SYS", SIGSYS}, {NULL, 0}
};

static void usage(FILE *out) {
    fputs("usage: pkill [-SIGNAL | --signal SIGNAL] [-x] [-e] [--] PATTERN\n"
          "Signal processes whose names match PATTERN (default: TERM).\n"
          "  -x  match the whole name   -e  print signalled names and PIDs\n"
          "  -l  list signals          -h  show help\n"
          "Names are limited to the eight characters reported by ps.\n", out);
}

static int signal_number(const char *text) {
    char name[12];
    unsigned n = 0, i;
    if (!*text) return -1;
    if (*text >= '0' && *text <= '9') {
        do {
            if (*text < '0' || *text > '9') return -1;
            n = n * 10 + *text++ - '0';
            if (n >= NSIGS) return -1;
        } while (*text);
        return n;
    }
    for (i = 0; text[i]; i++) {
        unsigned c = text[i];
        if (i + 1 >= sizeof(name)) return -1;
        name[i] = c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
    }
    name[i] = 0;
    text = !strncmp(name, "SIG", 3) ? name + 3 : name;
    for (i = 0; signals[i].name; i++)
        if (!strcmp(text, signals[i].name)) return signals[i].number;
    return -1;
}

int main(int argc, char **argv) {
    int fd, sig = SIGTERM, exact = 0, echo = 0, i = 1;
    unsigned slot, sent = 0;
    unsigned self = getpid();
    char expression[262];
    regexp *pattern;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        const char *arg = argv[i++];
        if (!strcmp(arg, "--")) break;
        if (!strcmp(arg, "-x")) exact = 1;
        else if (!strcmp(arg, "-e")) echo = 1;
        else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) { usage(stdout); return 0; }
        else if (!strcmp(arg, "-l") || !strcmp(arg, "--list-signals")) {
            unsigned n;
            for (n = 0; signals[n].name; n++)
                printf("%2d %s\n", signals[n].number, signals[n].name);
            return 0;
        } else {
            if (!strcmp(arg, "--signal")) {
                if (i == argc) goto bad_usage;
                arg = argv[i++];
            } else if (!strncmp(arg, "--signal=", 9)) arg += 9;
            else arg++;
            sig = signal_number(arg);
            if (sig < 0) { fprintf(stderr, "pkill: unknown signal: %s\n", arg); return 2; }
        }
    }
    if (argc - i != 1 || !*argv[i] || strlen(argv[i]) > 256) goto bad_usage;
    if (exact) { strcpy(expression, "^("); strcat(expression, argv[i]); strcat(expression, ")$"); }
    else strcpy(expression, argv[i]);
    /* The shared regexp library diagnoses malformed expressions and exits 2. */
    pattern = regcomp(expression);
    if (!pattern) { fputs("pkill: invalid regular expression\n", stderr); return 2; }
    fd = open("/dev/proc", O_RDONLY);
    if (fd < 0) { perror("pkill: /dev/proc"); free(pattern); return 3; }
    if (process_snapshot(fd, &sys, procs) < 0) {
        fputs("pkill: cannot read process snapshot\n", stderr);
        close(fd); free(pattern); return 3;
    }
    close(fd);
    for (slot = 0; slot < sys.slots; slot++) {
        struct ripple_procinfo *p = &procs[slot];
        if (!p->state || p->state == 6 || p->state == 7 || p->pid <= 1 || p->pid == self)
            continue;
        if (!regexec(pattern, p->name)) continue;
        if (kill(p->pid, sig) < 0) {
            if (errno != ESRCH) fprintf(stderr, "pkill: %s (%u): %s\n", p->name, p->pid, strerror(errno));
        } else {
            ++sent;
            if (echo) printf("%s (%u)\n", p->name, p->pid);
        }
    }
    free(pattern);
    return sent ? 0 : 1;
bad_usage:
    usage(stderr);
    return 2;
}
