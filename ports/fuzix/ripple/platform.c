/* SPDX-License-Identifier: GPL-2.0-only */
#include "platform.h"
#include <irq.h>
#include <kdata.h>
#include <mmio.h>
#include <printf.h>
#include <tty.h>
struct u_data udata;
unsigned ripple_kernel_stack = 32;
unsigned ripple_exec_bank, ripple_exec_pc, ripple_argc;
unsigned long ripple_argv, ripple_envp;

static struct u_data task_data[PTABSIZE];
static struct ripple_context contexts[PTABSIZE];
uint8_t sys_cpu = 0x52, sys_cpu_feat = 0, sys_stubs[1];
uint8_t need_resched, plt_tick_present = 1;
uaddr_t ramtop;
static unsigned indexof(ptptr p) { return (unsigned)(p - ptab); }
static void save_user(unsigned i) {
    ripple_bank_copy(USER_GP, 64 + 2 * i);
    ripple_bank_copy(USER_SB, 65 + 2 * i);
}
static void load_user(unsigned i) {
    ripple_bank_copy(64 + 2 * i, USER_GP);
    ripple_bank_copy(65 + 2 * i, USER_SB);
}
extern void ripple_code_init(void);
void pagemap_init(void) {
    ripple_code_init();
    ramsize = (64UL + 2 * PTABSIZE) * BANK_CELLS / 512;
    procmem = PTABSIZE * 250;
}
void map_init(void) {}
int pagemap_alloc(ptptr p) {
    p->p_page = indexof(p) + 1;
    return 0;
}
extern void ripple_code_free(ptptr);
extern void ripple_code_fork(ptptr);
void pagemap_free(ptptr p) { ripple_code_free(p); }
int pagemap_prepare(struct exec *h) {
    used(h);
    return 0;
}
int pagemap_realloc(struct exec *h, usize_t m) {
    used(h);
    used(m);
    return 0;
}
usize_t pagemap_mem_used(void) { return nproc * 250UL; }
void program_vectors(uint16_t *p) { used(p); }
void set_cpu_type(void) {}
void install_vdso(void) {}
void plt_discard(void) {}
void plt_switchout(void) {
    unsigned i = indexof(udata.u_ptab);
    if (ripple_context_save(&contexts[i]))
        return;
    task_data[i] = udata;
    save_user(i);
    switchin(getproc());
}
void switchin(ptptr p) {
    unsigned i = indexof(p);
    load_user(i);
    udata = task_data[i];
    ripple_kernel_stack = 32 + i;
    p->p_status = P_RUNNING;
    runticks = 0;
    need_resched = 0;
    ripple_context_restore(&contexts[i]);
    panic("switchin returned");
}
int16_t dofork(ptptr p) {
    unsigned parent = indexof(udata.u_ptab), child = indexof(p);
    if (ripple_context_save(&contexts[parent]))
        return 0;
    task_data[child] = udata;
    makeproc(p, &task_data[child]);
    ripple_code_fork(p);
    save_user(child);
    contexts[child] = contexts[parent];
    contexts[child].sb = 32 + child;
    ripple_bank_copy(32 + parent, 32 + child);
    return p->p_pid;
}
void sync_clock(void) {}
uint_fast8_t plt_rtc_secs(void) { return 255; }
uint_fast8_t plt_param(char *s) {
    used(s);
    return 0;
}
void plt_idle(void) { ripple_poll(); }
void ripple_shutdown(void) {
    unsigned i;
    sync();
    /* Mark clean only after all ordinary buffers are on the host disk. */
    storage_commit_all();
    if (storage_get_status() & STORE_CTL_ERROR) {
        kputs("Disk sync FAILED; filesystem left dirty.\n");
        ripple_halt();
    }
    for (i = 0; i < NMOUNTS; i++)
        if (fs_tab[i].m_dev != NO_DEVICE && !(fs_tab[i].m_flags & MS_RDONLY))
            fs_tab[i].m_fs.s_fmod = FMOD_GO_CLEAN;
    sync();
    storage_commit_all();
    if (storage_get_status() & STORE_CTL_ERROR) {
        kputs("Disk sync FAILED.\n");
        ripple_halt();
    }
    kputs("Ripple: disk synced; power off.\n");
    ripple_halt();
}
void plt_monitor(void) { ripple_shutdown(); }
void plt_reboot(void) { ripple_shutdown(); }
void device_init(void) { ripple_disk_init(); }
extern int ripple_valid_code(unsigned long);
extern void ripple_resume_user(void);
/* Kernel frames: R3..R27, GP, PC, PCB, flags, SB, SP, FP (32 cells).
 * Put a copy on the user stack before entering a handler. This leaves no
 * suspended kernel call underneath user code, including handlers that jump
 * out with longjmp, fork, exec, or make another blocking syscall. */
unsigned ripple_signal_frame(void) {
    unsigned *f = (unsigned *)((unsigned long)ripple_kernel_stack << 16);
    unsigned sig = udata.u_cursig, sp, i;
    unsigned long handler;
    if (!sig)
        return 0;
    handler = (unsigned long)udata.u_sigvec[sig];
    if (!ripple_valid_code(handler) || !ripple_valid_code(udata.u_sigtramp))
        doexit(SIGSEGV);
    sp = f[31];
    if (f[30] != USER_SB || sp > BANK_CELLS - 64)
        doexit(SIGSEGV);
    for (i = 0; i < 32; i++)
        dmem_store(USER_SB, sp + i, f[i + 1]);
    f[5] = sig;                       /* A0 */
    f[6] = (unsigned)handler;         /* A1: code offset */
    f[7] = handler >> 16;             /* A2: code bank */
    f[8] = sp;                        /* A3: saved frame offset */
    f[27] = (unsigned)udata.u_sigtramp;
    f[28] = udata.u_sigtramp >> 16;
    f[31] = sp + 32;
    udata.u_cursig = 0;
    udata.u_sigvec[sig] = SIG_DFL;     /* FUZIX/System V one-shot signal() */
    return 1;
}
static void signal_return(unsigned long ptr) {
    unsigned *f = (unsigned *)((unsigned long)ripple_kernel_stack << 16);
    unsigned off = (unsigned)ptr, i;
    unsigned frame[32];
    if ((ptr >> 16) != USER_SB || off > BANK_CELLS - 32)
        doexit(SIGSEGV);
    for (i = 0; i < 32; i++)
        frame[i] = dmem_load(USER_SB, off + i);
    if (frame[25] != USER_GP || frame[29] != USER_SB ||
        frame[30] > BANK_CELLS - 1 || frame[31] > BANK_CELLS - 1 ||
        !ripple_valid_code(((unsigned long)frame[27] << 16) | frame[26]))
        doexit(SIGSEGV);
    __hard_di();
    for (i = 0; i < 32; i++)
        f[i + 1] = frame[i];
    f[29] = 2; /* Restore ordinary user interrupt state, never IRQ_BUSY. */
    udata.u_insys = 0;
    ripple_resume_user();
}
long ripple_dispatch(struct ripple_request *user) {
    struct ripple_request req;
    if (uget(user, &req, sizeof(req)))
        return -1;
    udata.u_insys = 1;
    __hard_ei();
    if (req.call == 100)
        signal_return(req.arg[0]);
    udata.u_callno = req.call;
    udata.u_argn = req.arg[0];
    udata.u_argn1 = req.arg[1];
    udata.u_argn2 = req.arg[2];
    udata.u_argn3 = req.arg[3];
    unix_syscall();
    _uputw(udata.u_error, (uint16_t *)((uint8_t *)user +
                                       offsetof(struct ripple_request, error)));
    __hard_di();
    udata.u_insys = 0;
    return udata.u_retval;
}
unsigned ripple_tick(void) {
    udata.u_ininterrupt = 1;
    ripple_poll();
    timer_interrupt();
    udata.u_ininterrupt = 0;
    irq_ack();
    /* Signals must also reach a user computation which makes no syscalls.
     * The normal trampoline leaves IRQ_BUSY with IRET before entering C. */
    return !udata.u_insys &&
           ((need_resched && nready > 1) ||
            (udata.u_ptab->p_flags & PFL_CHKSIG));
}
int main(void) {
    void (*handler)(void) = ripple_irq;
    unsigned long target = (unsigned long)handler;
    irq_set_enable(0);
    irq_set_stack(4, 1);
    irq_set_vector((unsigned)(target >> 16), (unsigned)target);
    irq_set_timer(100000UL);
    ripple_kernel_stack = 32;
    fuzix_main();
    return 0;
}

void ripple_preempt(void) {
    udata.u_insys = 1;
    udata.u_ptab->p_status = P_READY;
    plt_switchout();
    __hard_ei();
    chksigs();
    __hard_di();
    ripple_signal_frame();
    udata.u_insys = 0;
    ripple_resume_user();
}

/* Unlike an explicit shutdown, a panic cannot certify filesystem consistency.
 */
void ripple_panic_stop(void) {
    __hard_di();
    kputs("\nKernel stopped; filesystem was not marked clean.\n");
    storage_commit_all();
    ripple_halt();
}
