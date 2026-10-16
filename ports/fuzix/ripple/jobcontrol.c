/* SPDX-License-Identifier: GPL-2.0-only */
/* Session/job-control subset of FUZIX level2.c and syscall_level2.c. */
#include "platform.h"
#include <kdata.h>
#include <tty.h>

static ptptr findpid(unsigned pid) {
    ptptr p;
    for (p = ptab; p < ptab_end; p++)
        if (p->p_status && p->p_pid == pid) return p;
    return NULL;
}
arg_t _setpgid(void) {
    ptptr self = udata.u_ptab, p, t;
    unsigned pid = udata.u_argn, group = udata.u_argn1;
    int exists = 0;
    if ((long)udata.u_argn < 0 || (long)udata.u_argn1 < 0 || group >= MAXPID) {
        udata.u_error = EINVAL; return -1;
    }
    t = pid ? findpid(pid) : self;
    if (!t || (t != self && t->p_pptr != self)) {
        udata.u_error = ESRCH; return -1;
    }
    if (t->p_session != self->p_session || t->p_session == t->p_pid) goto denied;
    if (t != self && (t->p_flags & PFL_EXECED)) {
        udata.u_error = EACCES; return -1;
    }
    if (!group) group = t->p_pid;
    for (p = ptab; p < ptab_end; p++)
        if (p->p_status && p->p_pgrp == group && p->p_session == self->p_session) exists = 1;
    if (group != t->p_pid && !exists) goto denied;
    t->p_pgrp = group;
    return 0;
denied:
    udata.u_error = EPERM; return -1;
}
arg_t _setsid(void) {
    ptptr p, self = udata.u_ptab;
    for (p = ptab; p < ptab_end; p++)
        if (p->p_status && p->p_pgrp == self->p_pid) {
            udata.u_error = EPERM; return -1;
        }
    self->p_session = self->p_pgrp = self->p_pid;
    self->p_tty = 0;
    udata.u_ctty = NULL;
    return self->p_pid;
}
arg_t _getsid(void) {
    ptptr p = udata.u_argn ? findpid(udata.u_argn) : udata.u_ptab;
    if (!p) { udata.u_error = ESRCH; return -1; }
    return p->p_session;
}
static int orphan(unsigned group, unsigned session) {
    ptptr p;
    for (p = ptab; p < ptab_end; p++)
        if (p->p_status && p->p_status != P_ZOMBIE && p->p_pgrp == group &&
            p->p_pptr && p->p_pptr->p_status && p->p_pptr->p_status != P_ZOMBIE &&
            p->p_pptr->p_pgrp != group && p->p_pptr->p_session == session) return 0;
    return 1;
}
static int jobop(unsigned minor, unsigned sig, struct tty *t, int reading) {
    ptptr p = udata.u_ptab;
    for (;;) {
        if (udata.u_sysio || p->p_tty != minor || !t->pgrp || p->p_pgrp == t->pgrp ||
            (t->flag & TTYF_DEAD)) return 0;
        if ((p->p_sig[1].s_held & sigmask(sig)) || udata.u_sigvec[sig] == SIG_IGN) {
            if (!reading) return 0;
        } else if (!orphan(p->p_pgrp, p->p_session)) {
            sgrpsig(p->p_pgrp, sig);
            if (!chksigs()) continue;
            udata.u_error = EINTR;
            return 1;
        }
        udata.u_error = EIO;
        return 1;
    }
}
uint8_t jobcontrol_in(uint8_t minor, struct tty *t) {
    if (!jobop(minor, SIGTTIN, t, 1)) return 0;
    if (!udata.u_done) udata.u_done = (usize_t)-1;
    return 1;
}
uint8_t jobcontrol_out(uint8_t minor, struct tty *t) {
    if (!(t->termios.c_lflag & TOSTOP) || !jobop(minor, SIGTTOU, t, 0)) return 0;
    if (!udata.u_done) udata.u_done = (usize_t)-1;
    return 1;
}
uint8_t jobcontrol_ioctl(uint8_t minor, struct tty *t, uarg_t req) {
    switch (req) {
    case TCSETS: case TCSETSW: case TCSETSF: case TIOCSPGRP:
    case TIOCFLUSH: case TIOCOSTOP: case TIOCOSTART: case TIOCSWINSZ: case TIOCHANGUP:
        return jobop(minor, SIGTTOU, t, 0);
    }
    return 0;
}
int tcsetpgrp(struct tty *t, char *data) {
    unsigned group;
    ptptr p;
    if (uget(data, &group, sizeof(group))) return -1;
    if (!group || group >= MAXPID) { udata.u_error = EINVAL; return -1; }
    for (p = ptab; p < ptab_end; p++)
        if (p->p_status && p->p_pgrp == group && p->p_session == udata.u_ptab->p_session &&
            t->session == p->p_session) {
            t->pgrp = group;
            return 0;
        }
    udata.u_error = EPERM;
    return -1;
}
