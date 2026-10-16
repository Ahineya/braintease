/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_FUZIX_PLATFORM_H
#define RIPPLE_FUZIX_PLATFORM_H
#include <kernel.h>
#define KERNEL_GP 3
#define USER_GP 8
#include "mmu.h"
#define BANK_CELLS 64000U
struct ripple_context {
    unsigned sp, fp, sb, ra, rab, s0, s1, s2, s3;
};
struct ripple_request {
    unsigned call;
    long arg[4];
    int error;
};
extern unsigned ripple_kernel_stack;
extern unsigned ripple_exec_bank, ripple_exec_pc, ripple_argc;
extern unsigned long ripple_argv, ripple_envp;
extern unsigned ripple_context_save(struct ripple_context *);
extern void ripple_context_restore(struct ripple_context *);
extern void ripple_enter_user(void);
extern void ripple_halt(void);
extern void ripple_irq(void);
extern void ripple_bank_copy(unsigned, unsigned);
extern int ripple_copy(void *, const void *, usize_t);
extern int ripple_zero(void *, usize_t);
extern unsigned dmem_load(unsigned, unsigned);
extern void dmem_store(unsigned, unsigned, unsigned);
extern void imem_store(unsigned, unsigned, unsigned *);
extern void *ripple_userptr(const void *);
extern void ripple_disk_init(void);
extern void ripple_poll(void);
extern void ripple_shutdown(void);
#endif
