/* SPDX-License-Identifier: GPL-2.0-only */
#include <kernel.h>
#include <kdata.h>
#include <vm_bulk.h>

/* FUZIX CONFIG_SINGLETASK: child first, parent resumes after child exit.
 * Fixed execution banks preserve pointers embedded in copied C objects. */
struct rbf_context { unsigned sp,fp,sb,ra,rab,s0,s1,s2,s3; };
struct rbf_context rbf_parent_context;
static struct u_data parent_data;
static unsigned parent_saved;
extern unsigned rbf_context_save(struct rbf_context *);
extern void rbf_context_restore(struct rbf_context *);
extern void rbf_restore_parent(void);
static void copy_bank(unsigned source,unsigned dest) {
    if (vm_bulk_copy(source,0,dest,0,BANK_SIZE,VM_BULK_WORDS)!=VM_DEVICE_OK)
        panic("process bank copy");
}
/* Called on a scratch stack in bank 5, after its saved globals were restored.
 * Copying the kernel stack in C on that same destination stack is unsafe. */
void rbf_restore_kernel(void) {
    copy_bank(7,2);
    rbf_context_restore(&rbf_parent_context);
    panic("context restore returned");
}
int16_t dofork(ptptr child) {
    if (parent_saved || udata.u_ptab->p_pid!=1) return -1;
    if (rbf_context_save(&rbf_parent_context)) return child->p_pid;
    parent_data=udata;
    copy_bank(3,5); copy_bank(4,6); copy_bank(2,7);
    parent_saved=1;
    makeproc(child,&udata);
    return 0;
}
void switchin(ptptr p) {
    if (!parent_saved || p!=parent_data.u_ptab || p->p_pid!=1)
        panic("invalid parent restore");
    copy_bank(5,3); copy_bank(6,4);
    udata=parent_data;
    p->p_status=P_RUNNING;
    parent_saved=0;
    rbf_restore_parent();
    panic("switchin returned");
}
void plt_switchout(void) {
    /* Single-task scheduling deliberately keeps the child selected while it
     * waits for console input; no IRQ or runnable-parent spin is needed. */
    while (udata.u_ptab->p_status!=P_READY && udata.u_ptab->p_status!=P_RUNNING)
        plt_idle();
    udata.u_ptab->p_status=P_RUNNING;
}
/* The disposable demo disk has no durability barrier, including child exit. */
void rbf_no_exit_sync(void) {}
usize_t pagemap_mem_used(void) { return nproc*2UL*BANK_SIZE/512; }
