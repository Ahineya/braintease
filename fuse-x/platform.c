/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"
#include "pcm.h"
#include "hostfs.h"
#include <kdata.h>
#include <printf.h>
#include <timer.h>
#include <vm_devices.h>
#include <mmio.h>
#include <native_hosted.h>
#include <string.h>
#include "selectwait.h"
#include "graphics.h"

struct u_data udata;
uint8_t sys_cpu = 0x52, sys_cpu_feat, sys_stubs[1];
uint8_t need_resched, plt_tick_present = 1;
uaddr_t ramtop;
static irqflags_t interrupts;
static unsigned long host_last;
static unsigned clock_ready;
unsigned fuse_write_check;
unsigned fuse_running;
unsigned fuse_headless, fuse_telnet, fuse_one_shot, fuse_stopping, fuse_console_closed;
unsigned fuse_gui, fuse_rescue;
const char *fuse_value;
static char saved_value[1024];
char *fuse_boot_parameters = "ro";
static struct u_data task_data[PTABSIZE];
static unsigned indexof(ptptr p) { return (unsigned)(p - ptab); }

irqflags_t __hard_di(void) { irqflags_t previous = interrupts; interrupts = 0; return previous; }
void __hard_ei(void) { interrupts = 1; }
void __hard_irqrestore(irqflags_t previous) { interrupts = previous; }
void set_cpu_type(void) {}
void map_init(void) {}
void program_vectors(uint16_t *p) { used(p); }
void install_vdso(void) {}
void pagemap_init(void) {
    ramsize = 32000;
    procmem = 30000;
}
int pagemap_alloc(ptptr p) {
    p->p_page = indexof(p) + 1;
    p->p_size = 750; /* Provisional charge; resident-memory budgets are not integrated. */
    return 0;
}
void pagemap_free(ptptr p) { ripple_gfx_exit(p); ripple_pcm_exit(p); p->p_page = 0; p->p_size = 0; }
int pagemap_prepare(struct exec *h) { used(h); return 0; }
int pagemap_realloc(struct exec *h, usize_t size) { used(h); used(size); return ENOSYS; }
usize_t pagemap_mem_used(void) { return nproc * 750UL; }
static void select_task(ptptr p) {
    udata = task_data[indexof(p)];
    proc_setstate(p, P_RUNNING);
    runticks = 0;
    proc_sched_switch();
}
void plt_switchout(void) {
    ptptr next;
    sync_clock();
    task_data[indexof(udata.u_ptab)] = udata;
    next = getproc();
    select_task(next);
    rcc_hosted_switch(indexof(next));
}
void switchin(ptptr p) {
    select_task(p);
    rcc_hosted_drop_switch(indexof(p));
    panic("Fuse-X: discarded continuation returned");
}
int16_t dofork(ptptr p) {
    int result;
    unsigned child = indexof(p);
    task_data[child] = udata;
    makeproc(p, &task_data[child]);
    result = rcc_hosted_clone(child);
    if (result < 0) {
        unsigned i;
        for (i=0;i<UFTSIZE;i++) if (task_data[child].u_files[i]!=NO_FILE)
            --of_tab[task_data[child].u_files[i]].o_refs;
        if (task_data[child].u_cwd) i_deref(task_data[child].u_cwd);
        if (task_data[child].u_root) i_deref(task_data[child].u_root);
    }
    if (result <= 0) return result;
    return p->p_pid;
}
int ripple_valid_code(unsigned long p) {
    return rcc_hosted_valid_function(p,0) || rcc_hosted_valid_function(p,1);
}
void *ripple_userptr(const void *p) { return (void *)p; }
void ripple_bank_copy_slow(unsigned source, unsigned dest) {
    memcpy((void *)((unsigned long)dest << 16), (void *)((unsigned long)source << 16), 64000);
}
uint16_t ripple_swab16(uint16_t v) { return (v >> 8) | (v << 8); }
unsigned short rng_get(void) { return mmio_read(MMIO_RNG); }
uint32_t ripple_swab32(uint32_t v) {
    return ((uint32_t)ripple_swab16(v) << 16) | ripple_swab16(v >> 16);
}

/* Reuse the new host clock protocol. There is one guest execution thread;
 * interrupt masking here is logical, never a lock held by the Mac UI thread. */
void sync_clock(void) {
    VmHostTime now;
    unsigned long sample, elapsed, deciseconds;
    unsigned remainder;
    irqflags_t flags = di();
    if (clock_ready) {
        if (vm_host_monotonic(&now) != VM_DEVICE_OK) {
            ripple_select_clock(NULL);
            irqrestore(flags);
            return;
        }
        proc_sched_sample(now.seconds_low, (unsigned)(now.nanoseconds / 1000000UL));
        ripple_select_clock(&now);
        ripple_pcm_clock(&now);
        sample = now.seconds_low * 100UL + now.nanoseconds / 10000000UL;
        elapsed = sample - host_last;
        host_last = sample;
        deciseconds = elapsed / 10;
        remainder = ticks_this_dsecond + elapsed % 10;
        ticks_this_dsecond = remainder % 10;
        deciseconds += remainder / 10;
        if (deciseconds) timer_advance(deciseconds);
    }
    irqrestore(flags);
}
void plt_clock_init(void) {
    VmHostTime now;
    sync_clock();
    if (vm_host_realtime(&now) != VM_DEVICE_OK) panic("Fuse-X: realtime clock unavailable");
    tod.low = now.seconds_low;
    tod.high = now.seconds_high;
    proc_sched_switch();
}
uint_fast8_t plt_rtc_secs(void) { return 255; }
uint_fast8_t plt_param(char *p) { used(p); return 0; }
void plt_idle(void) {
    ripple_gfx_poll();
    if (fuse_running && rcc_hosted_stopping()) fuse_request_shutdown();
    fuse_poll_input(); sync_clock(); ripple_network_poll();
    if(!nready) rcc_hosted_idle(1);
}
/* Ignored/default SIGCONT may wake a timed sleeper without interrupting it.
 * Keep its existing deadline instead of treating every wake as EINTR. */
arg_t _pause(void) {
    int timeout=(int)udata.u_argn;
    irqflags_t irq=di();
    if(timeout<0) {irqrestore(irq);udata.u_error=EINVAL;return -1;}
    if(timeout) {udata.u_ptab->p_timeout=timeout+1;ptimer_insert();}
    for (;;) {
        psleep(0);
        if(chksigs()) {irqrestore(irq);udata.u_error=EINTR;return -1;}
        if(timeout && udata.u_ptab->p_timeout==1) {irqrestore(irq);return 0;}
    }
}
void fuse_request_shutdown(void) {
    fuse_stopping = 1;
    if (init_process) pwake(init_process);
}
unsigned fuse_stop_tasks(unsigned sig) {
    ptptr p;
    unsigned count=0;
    for(p=ptab;p<ptab_end;p++) if(p->p_status && p->p_status!=P_ZOMBIE && p->p_pid!=1) {
        ++count;
        if(sig) {
            ssig(p,sig);
            if(sig==SIGTERM) ssig(p,SIGCONT);
        }
    }
    return count;
}
void plt_monitor(void) {
    if(!fuse_running || udata.u_ptab->p_pid==1) fuse_shutdown();
    fuse_request_shutdown();
    doexit(0);
}
void plt_reboot(void) { plt_monitor(); }
void ripple_panic_stop(void) { exit(1); }
void doexec(uaddr_t address) { used(address); panic("Fuse-X: invalid legacy exec entry"); }

/* The explicit bootstrap command stops at the actual /init loading boundary.
 * It never reports that a userspace executable has been loaded or entered. */
void plt_discard(void) { if (!fuse_running) fuse_boot_check(); else fuse_host_mount(); }

static void dispatch_signals(void) {
    /* unix_syscall has completed all kernel work and released resources.
     * These are resumable calls: a handler may block, fork, exec or longjmp
     * out, safely discarding this now-inactive syscall continuation. */
    while (udata.u_cursig) {
        unsigned sig = udata.u_cursig;
        void (*handler)(int) = (void (*)(int))udata.u_sigvec[sig];
        if (!rcc_hosted_valid_function((unsigned long)handler,1)) doexit(SIGSEGV);
        udata.u_cursig = 0;
        udata.u_sigvec[sig] = SIG_DFL;
        handler(sig);
        chksigs();
    }
}

/* Only called between exec readi calls, with no filesystem buffer pinned.
 * udata (including the loader's file position) is saved by plt_switchout.
 * Signal handlers remain deferred until exec commits or returns an error. */
void fuse_exec_yield(unsigned long *slice, unsigned waiting) {
    VmHostTime clock;
    unsigned long now;
    unsigned error=udata.u_error;
    arg_t result=udata.u_retval;
    if(vm_host_monotonic(&clock)!=VM_DEVICE_OK) return;
    now=clock.seconds_low*1000UL+clock.nanoseconds/1000000UL;
    if(!waiting && now-*slice<5) return;
    *slice=now;
    sync_clock();
    ripple_gfx_poll();
    fuse_poll_input();
    ripple_network_poll();
    if(rcc_hosted_stopping()) fuse_request_shutdown();
    if(nready>1) {
        /* switchout() checks fatal signals before returning: this loader still
         * owns an inode and staging memory. Defer that check until cleanup. */
        irqflags_t irq=di();
        proc_setstate(udata.u_ptab, P_READY);
        plt_switchout();
        irqrestore(irq);
    }
    else if(waiting) rcc_hosted_idle(1);
    udata.u_error=error;
    udata.u_retval=result;
}

static void fuse_checkpoint(void) {
    unsigned error=udata.u_error;
    arg_t result=udata.u_retval;
    udata.u_insys=1;
    sync_clock();
    ripple_gfx_poll();
    fuse_poll_input();
    ripple_network_poll();
    if(rcc_hosted_stopping()) fuse_request_shutdown();
    chksigs();
    if(nready>1 && need_resched) switchout();
    chksigs();
    udata.u_error=error;
    udata.u_retval=result;
    udata.u_insys=0;
    dispatch_signals();
}

static arg_t syscall_impl(unsigned number, unsigned long a, unsigned long b, unsigned long c, unsigned long d, int *error) {
    arg_t result;
    sync_clock();
    ripple_gfx_poll();
    ripple_network_poll();
    if(fuse_running && rcc_hosted_stopping()) fuse_request_shutdown();
    udata.u_insys = 1;
    udata.u_sysio = 0;
    udata.u_callno = number;
    udata.u_argn = a; udata.u_argn1 = b; udata.u_argn2 = c; udata.u_argn3 = d;
    unix_syscall(); /* Includes the exit clock sample and slice check. */
    ripple_network_poll();
    result = udata.u_retval;
    _uputw(udata.u_error, (uint16_t *)error);
    udata.u_insys = 0;
    dispatch_signals();
    return result;
}

arg_t fuse_syscall(unsigned number, unsigned long a, unsigned long b, unsigned long c, int *error) {
    return syscall_impl(number,a,b,c,0,error);
}
/* This request layout is shared with the existing userspace syscall wrappers. */
struct native_request { unsigned call; long arg[4]; int error; };
long fuse_image_syscall(unsigned long address) {
    struct native_request *r=(void *)address;
    if (!valaddr_r((void *)r,sizeof(*r),sizeof(*r)) || !valaddr_w((void *)r,sizeof(*r),sizeof(*r))) return -1;
    return syscall_impl(r->call,r->arg[0],r->arg[1],r->arg[2],r->arg[3],&r->error);
}

int main(int argc, char **argv) {
    VmHostTime now;
    int i;
    if (argc < 3 ||
        (strcmp(argv[1], "--run") && strcmp(argv[1], "--boot-check") && strcmp(argv[1], "--write-check") &&
         strcmp(argv[1], "--read-check"))) {
        kputs("usage: fuse-x --run ROOTFS.IMG [--share HOST_DIRECTORY] [--headless] [--gui] [--telnet] [--one-shot] [--rescue]\n"
              "       fuse-x --boot-check ROOTFS.IMG\n"
              "       fuse-x --write-check ROOTFS.IMG VALUE\n"
              "       fuse-x --read-check ROOTFS.IMG VALUE\n");
        return 2;
    }
    fuse_running = !strcmp(argv[1], "--run");
    fuse_write_check = !strcmp(argv[1], "--write-check");
    if(fuse_running) {
        for(i=3;i<argc;i++) {
            if(!strcmp(argv[i],"--headless")) fuse_headless=1;
            else if(!strcmp(argv[i],"--gui")) fuse_gui=1;
            else if(!strcmp(argv[i],"--telnet")) fuse_telnet=1;
            else if(!strcmp(argv[i],"--one-shot")) fuse_one_shot=1;
            else if(!strcmp(argv[i],"--rescue")) fuse_rescue=1;
            else if(!strcmp(argv[i],"--share") && i+1<argc && !*fuse_host_directory) {
                ++i;
                if(!*argv[i] || strlen(argv[i])>=sizeof(fuse_host_directory))return 2;
                strcpy(fuse_host_directory,argv[i]);
            } else return 2;
        }
        if(fuse_headless && fuse_one_shot)return 2;
        if(fuse_headless && fuse_gui)return 2;
    } else if (argc == 4) {
        if (strlen(argv[3]) > 1023) return 2;
        strcpy(saved_value, argv[3]);
        fuse_value = saved_value;
    }
    if (!fuse_running && (!strcmp(argv[1], "--boot-check") ? argc!=3 : argc!=4)) return 2;
    if (fuse_write_check || fuse_running) fuse_boot_parameters = "rw";
    kputs("Fuse-X: native Riplix kernel bootstrap\n");
    fuse_disk_open(argv[2], fuse_write_check || fuse_running);
    if (vm_host_monotonic(&now) != VM_DEVICE_OK) panic("Fuse-X: host clock unavailable");
    host_last = now.seconds_low * 100UL + now.nanoseconds / 10000000UL;
    clock_ready = 1;
    rcc_hosted_bind_syscall(fuse_image_syscall);
    rcc_hosted_bind_checkpoint(fuse_checkpoint);
    fuzix_main();
    panic("Fuse-X: kernel initialization returned");
    return 1;
}
