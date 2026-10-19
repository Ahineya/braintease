/* SPDX-License-Identifier: GPL-2.0-only */
#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <string.h>
#include <vm_bulk.h>

extern void rbf_stop(unsigned);
uint8_t sys_cpu = 0x52, sys_cpu_feat, sys_stubs[1];
uint8_t need_resched, plt_tick_present;
uaddr_t ramtop;
void pagemap_init(void) {
    if (sizeof(struct dinode) != 64 || sizeof(struct filesys) != 164 ||
        offsetof(struct filesys, s_free) != 5) panic("C16 V3 layout mismatch");
    ramsize = RBF_RAM_BANKS * 1UL * BANK_SIZE / 512;
#ifdef RBF_SHELL
    procmem = 4UL * BANK_SIZE / 512;
#else
    procmem = BANK_SIZE / 512;
#endif
}
int pagemap_alloc(ptptr p) { p->p_page = 3; return 0; }
void map_init(void) {}
void program_vectors(uint16_t *p) { used(p); }
void set_cpu_type(void) {}
uint_fast8_t plt_param(char *s) { used(s); return 0; }
void sync_clock(void) {}
void pagemap_free(ptptr p) { used(p); }
#ifdef RBF_SHELL
extern void rbf_poll(void);
void plt_idle(void) { rbf_poll(); vm_host_idle(1); }
#else
void plt_idle(void) { panic("boot unexpectedly blocked"); }
#endif
#ifndef RBF_SHELL
void plt_switchout(void) { panic("boot attempted task switch"); }
void switchin(ptptr p) { used(p); panic("boot attempted switchin"); }
#endif
void ripple_panic_stop(void) { kputs("\nRBF boot FAILED\n"); rbf_stop(1); }

/* The boot-check bootstrap writes arguments into physical bank 3 directly.
 * The shell profile supplies checked user-memory helpers in usermem.c. */
#ifndef RBF_SHELL
uint32_t ripple_ugetp(void *p) { return *(uint32_t *)p; }
#endif
void *ripple_buffer_copy(void *to, const void *from, usize_t n) {
#ifdef RBF_SHELL
    if (n >= 64) {
        unsigned long dest=(unsigned long)to, source=(unsigned long)from;
        if (vm_bulk_copy(source>>16,(unsigned)source,dest>>16,(unsigned)dest,n,VM_BULK_WORDS)!=VM_DEVICE_OK)
            panic("buffer copy");
        return to;
    }
#endif
    return memcpy(to, from, n);
}
void ripple_buffer_zero(void *p) { memset(p, 0, BLKSIZE); }

/* start.c reaches this in place of exec_or_die, after its real root mount,
 * inode lookup and PID 1 initialization. Verify a regular file through readi,
 * rather than treating the sign-on banner as a successful boot. */
void rbf_boot_complete(void) {
    static const char expected[] = "Riplix kernel running in Brainfuck.\n";
    char buffer[sizeof(expected)];
    inoptr file;
    if (!root || root->c_num != ROOTINODE ||
        !init_process || init_process->p_pid != 1 ||
        init_process->p_status != P_RUNNING || nproc != 1 || nready != 1)
        panic("boot state");
    udata.u_sysio = 1;
#ifdef RBF_SHELL
    /* n_open takes a userspace pathname, even during bootstrap. */
    uput("/BOOT", (void *)PROGBASE, 6);
    file = n_open((uint8_t *)PROGBASE, NULLINOPTR);
#else
    file = n_open((uint8_t *)"/BOOT", NULLINOPTR);
#endif
    if (!file) panic("BOOT file missing");
    if (getmode(file) != MODE_R(F_REG) || file->c_node.i_size != sizeof(expected) - 1)
        panic("BOOT file contents");
    memset(buffer, 0, sizeof(buffer));
    udata.u_base = (uint8_t *)buffer;
    udata.u_count = sizeof(expected) - 1;
    udata.u_done = 0;
    udata.u_offset = 0;
    udata.u_error = 0;
    readi(file, 0);
    if (udata.u_error || udata.u_done != sizeof(expected) - 1 ||
        memcmp(buffer, expected, sizeof(expected))) panic("BOOT file contents");
    i_deref(file);
    kputs(buffer);
    kputs("RBF boot OK: PID 1, root mounted, file verified.\n");
#ifdef RBF_SHELL
    extern void rbf_exec_init(void);
    rbf_exec_init();
    panic("init returned");
#else
    rbf_stop(0);
#endif
}
int main(void) {
    kputs("Riplix / Brainfuck boot profile\n");
    fuzix_main();
    panic("bootstrap returned");
    return 1;
}
