/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_FUZIX_MMU_H
#define RIPPLE_FUZIX_MMU_H
#include "mmu-profile.h"
#include "code.h"
#define USER_SB 31
#define USER_HEAP_LAST 30
#define MMU_HEAP_BANKS 23U
/* Pack tables within banks: three per bank with 23 heap banks and 1024 code banks.
 * This leaves room for 64 private kernel stacks without shrinking user RAM. */
#define MMU_TABLE_CELLS (2048U + RIPPLE_CODE_MAP_CELLS + (MMU_HEAP_BANKS + 1U) * 512U)
#define MMU_TABLES_PER_BANK (64000UL / MMU_TABLE_CELLS)
#define MMU_TABLE_BASE 96
#define MMU_TABLE_BANKS ((PTABSIZE + MMU_TABLES_PER_BANK - 1) / MMU_TABLES_PER_BANK)
#define MMU_REF_BANK (MMU_TABLE_BASE + MMU_TABLE_BANKS)
#define MMU_TASK_DATA_BANK (MMU_REF_BANK + 1)
/* Two supervisor banks hold the outer FAT chain, outside kernel GP. */
#define MMU_DISK_MAP_BANK (MMU_TASK_DATA_BANK + 1)
/* Physical scanout storage, separate from the COW process pool. */
#define MMU_GRAPHICS_BANK (MMU_DISK_MAP_BANK + 2)
#define MMU_GRAPHICS_CELLS (5UL * 64000UL)
#define MMU_POOL_BASE 128
#define MMU_RAM_BANKS 256
#define MMU_PAGE_CELLS 512U
#define MMU_BANK_PAGES 125U
#define MMU_POOL_PAGES ((MMU_RAM_BANKS - MMU_POOL_BASE) * MMU_BANK_PAGES)
#if MMU_TABLE_CELLS > 64000UL
#error Ripple MMU tables exceed their reserved bank
#endif
#if 32 + PTABSIZE > MMU_TABLE_BASE || MMU_GRAPHICS_BANK + 5 > MMU_POOL_BASE
#error Overlapping Ripple process stacks or MMU metadata
#endif
#define MP_PRESENT 1U
#define MP_READ 2U
#define MP_WRITE 4U
#define MP_EXEC 8U
#define MP_USER 16U
#define MP_PAGED 32U
#define MP_COW 64U
extern void ripple_mmu_init(void);
/* Permanent supervisor storage, at most one bank per object. Call under di(). */
extern void *ripple_mmu_kernel_alloc(unsigned);
extern usize_t ripple_mmu_used(void);
extern void ripple_mmu_usage(unsigned long *, unsigned long *);
extern void ripple_mmu_task_usage(unsigned, unsigned long *, unsigned long *);
extern int ripple_mmu_alloc(unsigned);
extern void ripple_mmu_select(unsigned);
extern void ripple_mmu_free(unsigned);
extern int ripple_mmu_fork(unsigned, unsigned);
extern int ripple_mmu_exec(unsigned);
extern int ripple_mmu_exec_data(unsigned, unsigned long);
extern void ripple_mmu_code(unsigned, unsigned, unsigned long);
extern usize_t ripple_mmu_validate(unsigned long, usize_t, unsigned);
extern void ripple_mmu_roots(unsigned, unsigned);
extern void ripple_mmu_flush(void);
extern void ripple_mmu_flush_code(void);
extern unsigned ripple_mmu_cause(void);
extern unsigned ripple_mmu_fault_bank(void);
extern unsigned ripple_mmu_fault_off(void);
extern unsigned ripple_mmu_saved_mode(void);
extern unsigned ripple_trap_action;
extern int ripple_mmu_span(unsigned long, unsigned, unsigned *, unsigned *, unsigned *);
extern int ripple_mmu_source(unsigned long, unsigned *, unsigned *, unsigned *);
#endif
