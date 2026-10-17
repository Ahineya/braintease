/* SPDX-License-Identifier: GPL-2.0-only */
#include "platform.h"
#include <irq.h>
#include <kdata.h>
#include <mmio.h>
#include <printf.h>
#include <tty.h>
#include <timer.h>
#include <vm_devices.h>
#include "graphics.h"
struct u_data udata;
unsigned ripple_kernel_stack = 32;
unsigned ripple_exec_bank, ripple_exec_pc, ripple_argc;
unsigned long ripple_argv, ripple_envp;

/* Task snapshots must not crowd the kernel's single GP bank as slots grow. */
#define task_data ((struct u_data *)((unsigned long)MMU_TASK_DATA_BANK << 16))
typedef char task_snapshots_fit_bank[(sizeof(struct u_data) * PTABSIZE <= 64000UL) ? 1 : -1];
static struct ripple_context contexts[PTABSIZE];
uint8_t sys_cpu = 0x52, sys_cpu_feat = 0, sys_stubs[1];
uint8_t need_resched, plt_tick_present = 1;
uaddr_t ramtop;
static unsigned indexof(ptptr p) { return (unsigned)(p - ptab); }
extern void ripple_code_init(void);
void pagemap_init(void) {
    ripple_code_init();
    ripple_mmu_init();
    ramsize = MMU_RAM_BANKS * (unsigned long)BANK_CELLS / 512;
    procmem = MMU_POOL_PAGES;
}
void map_init(void) {}
int pagemap_alloc(ptptr p) {
    if (!udata.u_ptab) {
        if (ripple_mmu_exec(indexof(p))) return ENOMEM;
    } else ripple_mmu_alloc(indexof(p));
    p->p_page = indexof(p) + 1;
    return 0;
}
extern void ripple_code_free(ptptr);
extern void ripple_code_fork(ptptr);
void pagemap_free(ptptr p) {
    ripple_gfx_exit(p);
    ripple_code_free(p);
    ripple_mmu_free(indexof(p));
    p->p_size = 0;
}
int pagemap_prepare(struct exec *h) {
    used(h);
    return 0;
}
int pagemap_realloc(struct exec *h, usize_t m) {
    used(h);
    used(m);
    return 0;
}
usize_t pagemap_mem_used(void) {
    return ripple_mmu_used();
}
void program_vectors(uint16_t *p) { used(p); }
void set_cpu_type(void) {}
void install_vdso(void) {}
void plt_discard(void) {}
void plt_switchout(void) {
    unsigned i = indexof(udata.u_ptab);
    if (ripple_context_save(&contexts[i]))
        return;
    task_data[i] = udata;
    switchin(getproc());
}
void switchin(ptptr p) {
    unsigned i = indexof(p);
    sync_clock();
    ripple_mmu_select(i);
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
    ripple_mmu_free(child);
    if (ripple_mmu_fork(parent,child)) panic("fork mappings");
    contexts[child] = contexts[parent];
    contexts[child].sb = 32 + child;
    ripple_bank_copy(32 + parent, 32 + child);
    return p->p_pid;
}
static unsigned long host_seconds;
static unsigned host_fraction;
static unsigned clock_ready;

/* The instruction IRQ remains a scheduling opportunity. Only this clock
 * advances elapsed time. No host time is synthesized from guest instructions. */
void sync_clock(void) {
    VmHostTime now;
    unsigned long seconds, deciseconds, credit;
    unsigned fraction, elapsed_fraction, remainder;
    irqflags_t flags = di();
    if (!clock_ready || vm_host_monotonic(&now) != VM_DEVICE_OK) {
        irqrestore(flags);
        return;
    }
    seconds = now.seconds_low - host_seconds;
    fraction = now.nanoseconds / 10000000UL;
    if (fraction < host_fraction) {
        --seconds;
        elapsed_fraction = fraction + 100 - host_fraction;
    } else elapsed_fraction = fraction - host_fraction;
    host_seconds = now.seconds_low;
    host_fraction = fraction;
    if (seconds || elapsed_fraction) {
        /* Sampled guest CPU time, not the host process's CPU usage. Limit
         * attribution after debugger/host pauses rather than billing all of
         * a long pause to whichever process happened to be selected. */
        credit = seconds || elapsed_fraction > 10 ? 10 : elapsed_fraction;
        if (!inswap && udata.u_ptab && udata.u_ptab->p_status == P_RUNNING) {
            if (udata.u_insys) udata.u_ptab->p_stime += credit;
            else udata.u_ptab->p_utime += credit;
        }
        deciseconds = seconds * 10 + elapsed_fraction / 10;
        remainder = ticks_this_dsecond + elapsed_fraction % 10;
        deciseconds += remainder / 10;
        ticks_this_dsecond = remainder % 10;
        if (deciseconds) timer_advance(deciseconds);
    }
    irqrestore(flags);
}

void plt_clock_init(void) {
    VmHostTime now;
    irqflags_t flags = di();
    sync_clock();
    if (vm_host_realtime(&now) == VM_DEVICE_OK) {
        tod.low = now.seconds_low;
        tod.high = now.seconds_high;
    }
    irqrestore(flags);
}
uint_fast8_t plt_rtc_secs(void) { return 255; }
uint_fast8_t plt_param(char *s) {
    used(s);
    return 0;
}
void plt_idle(void) {
    irqflags_t flags;
    ripple_poll();sync_clock();ripple_network_poll();ripple_gfx_poll();
    /* No runnable process: yield the host briefly, while retaining fine input
     * polling and host-clock timeout progress rather than spinning billions
     * of guest instructions between terminal events. */
    flags=di();vm_host_idle(1);irqrestore(flags);
}
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
void device_init(void) { ripple_disk_init(); ripple_network_init(); }
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
    if (valaddr_w((void *)(((unsigned long)USER_SB<<16)|sp),32,32)!=32) doexit(SIGSEGV);
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
    if (uget(user, &req, sizeof(req))
        || valaddr_w((void *)ripple_userptr(user),sizeof(req),sizeof(req))!=sizeof(req)
       )
        return -1;
    sync_clock();
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
    sync_clock();
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
    ripple_network_poll();
    ripple_gfx_poll();
    udata.u_ininterrupt = 0;
    irq_ack();
    /* Signals must also reach a user computation which makes no syscalls.
     * The normal trampoline leaves IRQ_BUSY with IRET before entering C. */
    return
           ripple_mmu_saved_mode() &&
           !udata.u_insys &&
           ((need_resched && nready > 1) ||
            (udata.u_ptab->p_flags & PFL_CHKSIG));
}
int main(void) {
    void (*handler)(void) = ripple_irq;
    unsigned long target = (unsigned long)handler;
    irq_set_enable(0);
    {
        VmHostTime now;
        if (vm_host_monotonic(&now) != VM_DEVICE_OK) {
            kputs("Host clock unavailable; update the VM.\n");
            ripple_halt();
        }
        host_seconds = now.seconds_low;
        host_fraction = now.nanoseconds / 10000000UL;
        clock_ready = 1;
    }
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
