/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "defs.h"
#include <termios.h>
#include <sys/wait.h>
#include <string.h>
#define NJOB 16
#define NMEMBER 16
struct member { int pid, status, state; }; /* state: running, stopped, exited */
struct job {
    int group, count, last, notified;
    char name[64];
    struct termios terminal, shell_terminal;
    int saved;
    struct member members[NMEMBER];
};
static struct job jobs[NJOB];
static int enabled, ttyfd, shellgroup, active = -1, background;
static int state(struct job *);
int jobs_enabled(void) { return enabled && !(flags & forked) && getpid() == shellgroup; }
void jobs_init(void) {
    int group;
    if (enabled && getpid() != shellgroup) { enabled = 0; active = -1; }
    if (!(flags & prompt) || enabled) return;
    ttyfd = input;
    group = tcgetpgrp(ttyfd);
    if (group < 0) return;
    /* An interactive subshell starts only when its inherited group owns input. */
    while (group != getpgrp()) {
        kill(0, SIGTTIN);
        group = tcgetpgrp(ttyfd);
        if (group < 0) return;
    }
    signal(SIGTSTP, SIG_IGN); signal(SIGTTIN, SIG_IGN); signal(SIGTTOU, SIG_IGN);
    shellgroup = getpid();
    if (getpgrp() != shellgroup && setpgid(0, 0) < 0) return;
    if (tcsetpgrp(ttyfd, shellgroup) < 0) return;
    enabled = 1;
}
static void label(TREPTR t, char *out) {
    ARGPTR a;
    unsigned n = 0;
    while (t && (t->tretyp & COMMSK) == TFORK) t = ((FORKPTR)t)->forktre;
    if (t && (t->tretyp & COMMSK) == TCOM) {
        for (a = ((COMPTR)t)->comarg; a && n < 62; a = a->argnxt) {
            char *s = a->argval;
            if (n) out[n++] = ' ';
            while (*s && n < 63) out[n++] = *s++ & STRIP;
        }
        out[n] = 0;
    } else strcpy(out, "pipeline");
}
int jobs_begin(TREPTR t, int bg) {
    int i;
    if (!jobs_enabled() || active >= 0) return 0;
    reapchildren();
    for (i = 0; i < NJOB; i++) if (!jobs[i].group) break;
    if (i == NJOB) {
        for (i = 0; i < NJOB; i++) if (state(&jobs[i]) == 2) break;
        if (i == NJOB) error("too many jobs");
    }
    memset(&jobs[i], 0, sizeof(struct job));
    label(t, jobs[i].name);
    tcgetattr(ttyfd, &jobs[i].shell_terminal);
    active = i; background = bg;
    return 1;
}
int jobs_current(void) { return jobs_enabled() && active >= 0; }
void jobs_child(void) {
    int group = jobs[active].group;
    if (setpgid(0, group) < 0) _exit(125);
    enabled = 0;
}
void jobs_child_signals(void) {
    signal(SIGINT, SIG_DFL); signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL); signal(SIGTTIN, SIG_DFL); signal(SIGTTOU, SIG_DFL);
}
void jobs_parent(int pid) {
    struct job *j = &jobs[active];
    if (!j->group) j->group = pid;
    setpgid(pid, j->group); /* Child also sets it before exec. */
    if (j->count < NMEMBER) {
        j->members[j->count].pid = pid;
        j->count++;
    }
    j->last = pid;
    if (!background) tcsetpgrp(ttyfd, j->group);
}
int jobs_status(int pid, int status) {
    int i, m;
    for (i = 0; i < NJOB; i++)
        if (jobs[i].group)
            for (m = 0; m < jobs[i].count; m++)
                if (jobs[i].members[m].pid == pid) {
                    jobs[i].members[m].status = status;
                    jobs[i].members[m].state = WIFSTOPPED(status) ? 1 : 2;
                    jobs[i].notified = 0;
                    return 1;
                }
    return 0;
}
/* A job is stopped only once every surviving pipeline member has stopped. */
static int state(struct job *j) {
    int m, stopped = 0;
    for (m = 0; m < j->count; m++) {
        if (!j->members[m].state) return 0;
        if (j->members[m].state == 1) stopped = 1;
    }
    return stopped ? 1 : 2;
}
static void describe(int i) {
    prs("["); prn(i + 1); prs("] "); prn(jobs[i].group); blank();
    prs(state(&jobs[i]) == 1 ? "Stopped " : state(&jobs[i]) == 2 ? "Done " : "Running ");
    prs(jobs[i].name); newline();
}
static int foreground(int i, int resume) {
    struct job *j = &jobs[i];
    int pid, status, m, result = 0;
    if (resume) tcgetattr(ttyfd, &j->shell_terminal);
    tcsetpgrp(ttyfd, j->group);
    if (resume) {
        if (j->saved) tcsetattr(ttyfd, TCSANOW, &j->terminal);
        for (m = 0; m < j->count; m++) if (j->members[m].state == 1) j->members[m].state = 0;
        kill(-j->group, SIGCONT);
        prs(j->name); newline();
    }
    while (state(j) == 0) {
        pid = waitpid(-j->group, &status, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR) continue;
            break;
        }
        jobs_status(pid, status);
    }
    if (state(j) == 1) j->saved = tcgetattr(ttyfd, &j->terminal) == 0;
    tcsetpgrp(ttyfd, shellgroup);
    for (m = 0; m < j->count; m++)
        if (j->members[m].pid == j->last) {
            status = j->members[m].status;
            result = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSTOPPED(status) ? WSTOPSIG(status) : WTERMSIG(status));
        }
    if (state(j) == 1 || result) tcsetattr(ttyfd, TCSANOW, &j->shell_terminal);
    shell_terminal_accept();
    if (state(j) == 1) { describe(i); j->notified = 1; }
    else j->group = 0;
    return result;
}
void jobs_wait(void) { exitval = foreground(active, 0); exitset(); }
void jobs_end(int owner) {
    if (!owner) return;
    if (background && jobs[active].group) { describe(active); jobs[active].notified = 1; }
    active = -1;
}
void jobs_prompt(void) {
    int i;
    if (!jobs_enabled()) return;
    active = -1;
    tcsetpgrp(ttyfd, shellgroup);
    reapchildren();
    for (i = 0; i < NJOB; i++) if (jobs[i].group && !jobs[i].notified && state(&jobs[i])) {
        describe(i); jobs[i].notified = 1;
    }
}
int jobs_builtin(int argc, char **argv) {
    int i, m, which = -1;
    char *end, *s;
    long n;
    if (strcmp(argv[0], "jobs") && strcmp(argv[0], "fg") && strcmp(argv[0], "bg")) return 0;
    exitval = 0;
    if (!jobs_enabled()) { prs("job control requires an interactive shell\n"); exitval = 1; return 1; }
    reapchildren();
    if (!strcmp(argv[0], "jobs")) {
        if (argc != 1) { prs("usage: jobs\n"); exitval = 1; return 1; }
        for (i = 0; i < NJOB; i++) if (jobs[i].group) {
            describe(i);
            if (state(&jobs[i]) == 2) jobs[i].group = 0;
        }
        return 1;
    }
    if (argc > 2) goto bad;
    if (argc == 2) {
        s = argv[1]; if (*s == '%') s++;
        n = strtol(s, &end, 10);
        if (!*s || *end || n < 1 || n > NJOB) goto bad;
        which = n - 1;
    } else for (i = 0; i < NJOB; i++) if (jobs[i].group && state(&jobs[i]) != 2) which = i;
    if (which < 0 || !jobs[which].group || state(&jobs[which]) == 2) goto bad;
    if (!strcmp(argv[0], "fg")) exitval = foreground(which, 1);
    else {
        for (m = 0; m < jobs[which].count; m++) if (jobs[which].members[m].state == 1) jobs[which].members[m].state = 0;
        if (kill(-jobs[which].group, SIGCONT) < 0) exitval = 1;
        describe(which);
    }
    return 1;
bad:
    prs("no such job\n"); exitval = 1; return 1;
}
void jobs_shutdown(void) {
    int i;
    if (!jobs_enabled()) return;
    for (i = 0; i < NJOB; i++) if (jobs[i].group && state(&jobs[i]) != 2) {
        kill(-jobs[i].group, SIGHUP); kill(-jobs[i].group, SIGCONT);
    }
}

void jobs_await(int wanted) {
    int i, m, pid, status, found = 0, result = 0;
    reapchildren();
    for (i = 0; i < NJOB; i++) if (jobs[i].group) {
        int match = wanted < 0 || wanted == jobs[i].group;
        for (m = 0; m < jobs[i].count; m++) if (wanted == jobs[i].members[m].pid) match = 1;
        if (!match) continue;
        found = 1;
        while (state(&jobs[i]) == 0) {
            pid = waitpid(-jobs[i].group, &status, WUNTRACED);
            if (pid < 0) { if (errno == EINTR) continue; break; }
            jobs_status(pid, status);
        }
        for (m = 0; m < jobs[i].count; m++) if (jobs[i].members[m].pid == jobs[i].last) {
            status = jobs[i].members[m].status;
            result = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSTOPPED(status) ? WSTOPSIG(status) : WTERMSIG(status));
        }
        if (state(&jobs[i]) == 2) jobs[i].group = 0;
    }
    exitval = !found && wanted > 0 ? 127 : result;
    exitset();
}
