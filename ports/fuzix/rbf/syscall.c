/* SPDX-License-Identifier: GPL-2.0-only */
#include <kernel.h>
#include <kdata.h>
#include <printf.h>
struct request { unsigned call; long arg[4]; int error; };
extern void rbf_stop(unsigned);
extern int rbf_execve(void);
long rbf_dispatch(struct request *user) {
    struct request req;
    long result = -1;
    if (uget(user,&req,sizeof(req)) || !valaddr_w((void *)user,sizeof(req),sizeof(req)))
        return -1;
    udata.u_error = 0;
    udata.u_sysio = 0;
    udata.u_insys = 1;
    udata.u_callno = req.call;
    udata.u_argn = req.arg[0]; udata.u_argn1 = req.arg[1];
    udata.u_argn2 = req.arg[2]; udata.u_argn3 = req.arg[3];
    switch (req.call) {
    case 0:
        if (udata.u_ptab->p_pid!=1) { __exit(); break; }
        kputs("\nRBF shell shutdown.\n"); rbf_stop((unsigned)req.arg[0]); break;
    case 57:
        if (udata.u_ptab->p_pid!=1) { udata.u_error=EPERM; break; }
        kputs("\nRBF shell shutdown.\n");
        rbf_stop(0); break;
    case 1: result = _open(); break;
    case 2: result = _close(); break;
    case 7: result = _read(); break;
    case 8: result = _write(); break;
    case 10: result = _chdir(); break;
    case 17: result = _dup(); break;
    case 18: result = _getpid(); break;
    case 19: result = _getppid(); break;
    case 23: result = rbf_execve(); break;
    case 32: result = _fork(); break;
    case 54: result = _uname(); break;
    case 55: result = _waitpid(); break;
    case 24: {
        inoptr directory = getinode((uint_fast8_t)req.arg[0]);
        if (!directory) break;
        if (getmode(directory) != MODE_R(F_DIR)) { udata.u_error=ENOTDIR; break; }
        result = _getdirent(); break;
    }
    default: udata.u_error = ENOSYS; break;
    }
    _uputw(udata.u_error,(uint16_t *)((uint8_t *)user+offsetof(struct request,error)));
    udata.u_insys = 0;
    return result;
}
