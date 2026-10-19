/* SPDX-License-Identifier: GPL-2.0-only */
/* Physical page tables and reference counts are outside the supervisor user
 * window. No pointer into a task's memory is retained across a switch. */
#include "platform.h"
#include <kdata.h>
#include <printf.h>
#include <vm_bulk.h>
extern long ripple_dispatch(struct ripple_request *);
extern unsigned ripple_signal_frame(void);
extern void ripple_resume_user(void);
#define DROOT 0U
#define DLEAF 512U
#define IROOT 1536U
#define ILEAF 2048U
#define PAGES (ILEAF + RIPPLE_CODE_MAP_CELLS)
#define TABLE_CELLS MMU_TABLE_CELLS
#if TABLE_CELLS > 64000UL
#error Ripple MMU code and data tables exceed their reserved bank
#endif
#define RWU (MP_PRESENT | MP_READ | MP_WRITE | MP_USER)
static unsigned cursor, used_pages;
#define KERNEL_PAGE 65535U
static unsigned task_pages[PTABSIZE];
static unsigned long task_data_end[PTABSIZE];
static unsigned ready[PTABSIZE];
/* Highest present page plus one in each virtual bank. Holes remain valid: the
 * walkers still check PRESENT, but never visit entirely unused bank tails. */
static unsigned page_high[PTABSIZE][MMU_HEAP_BANKS + 1U];
static unsigned bank_index(unsigned bank) { return bank == USER_SB ? MMU_HEAP_BANKS : bank - USER_GP; }
static unsigned bank_limit(unsigned task, unsigned bank) {
    return page_high[task][bank_index(bank)] * MMU_PAGE_CELLS;
}
unsigned ripple_trap_action;
static unsigned table_bank(unsigned task) {return MMU_TABLE_BASE + task / MMU_TABLES_PER_BANK;}
static unsigned table_offset(unsigned task) {return (task % MMU_TABLES_PER_BANK) * TABLE_CELLS;}
static unsigned *table(unsigned task, unsigned off) {
    return (unsigned *)(((unsigned long)table_bank(task) << 16) | (table_offset(task) + off));
}
static unsigned *refs(void) { return (unsigned *)((unsigned long)MMU_REF_BANK << 16); }
static unsigned *pte(unsigned task, unsigned bank, unsigned off) {
    unsigned index;
    if (bank >= USER_GP && bank <= USER_HEAP_LAST) index = bank - USER_GP;
    else if (bank == USER_SB) index = MMU_HEAP_BANKS;
    else return NULL;
    return table(task, PAGES + index * 512U + (off / MMU_PAGE_CELLS) * 4U);
}
static unsigned pageid(unsigned *p) {
    return (p[0] - MMU_POOL_BASE) * MMU_BANK_PAGES + p[1] / MMU_PAGE_CELLS;
}
static unsigned allocpage(void) {
    unsigned tries, id;
    for (tries = 0; tries < MMU_POOL_PAGES; tries++) {
        id = cursor++;
        if (cursor == MMU_POOL_PAGES) cursor = 0;
        if (!refs()[id]) {
            refs()[id] = 1;
            ++used_pages;
            return id;
        }
    }
    return 65535U;
}
static void unref(unsigned id) {
    if (id >= MMU_POOL_PAGES || !refs()[id]) panic("page reference");
    if (--refs()[id] == 0) --used_pages;
}
usize_t ripple_mmu_used(void) { return used_pages; }
void ripple_mmu_usage(unsigned long *shared, unsigned long *mapped) {
    unsigned id, count;
    *shared = *mapped = 0;
    for (id = 0; id < MMU_POOL_PAGES; id++) {
        count = refs()[id];
        if (count == KERNEL_PAGE) continue; /* not a process mapping */
        if (count > 1) ++*shared;
        *mapped += count;
    }
}
void ripple_mmu_task_usage(unsigned task, unsigned long *priv, unsigned long *shared) {
    unsigned bank, off, limit, *p;
    *priv = *shared = 0;
    if (!ready[task]) return;
    for (bank = USER_GP; bank <= USER_SB; bank++) {
        if (bank > USER_HEAP_LAST && bank != USER_SB) continue;
        limit=bank_limit(task,bank);
        for (off = 0; off < limit; off += MMU_PAGE_CELLS) {
            p = pte(task, bank, off);
            if (p[2] & MP_PRESENT) {
                if (refs()[pageid(p)] > 1) ++*shared;
                else ++*priv;
            }
        }
    }
}
static void setpage(unsigned *p, unsigned id) {
    p[0] = MMU_POOL_BASE + id / MMU_BANK_PAGES;
    p[1] = (id % MMU_BANK_PAGES) * MMU_PAGE_CELLS;
    p[2] = RWU;
    p[3] = 0;
}
static void *physical(unsigned id) {
    return (void *)(((unsigned long)(MMU_POOL_BASE + id / MMU_BANK_PAGES) << 16) |
                   ((id % MMU_BANK_PAGES) * MMU_PAGE_CELLS));
}
/* Stable bank-local allocations for shared supervisor tables. These pages
 * remain charged until reboot and are never candidates for user COW maps.
 * Check the entire run before reserving it: failure changes no references. */
void *ripple_mmu_kernel_alloc(unsigned cells) {
    unsigned pages, bank, start, i, id;
    void *result;
    if (!cells || cells > BANK_CELLS) return NULL;
    pages = (unsigned)(((unsigned long)cells + MMU_PAGE_CELLS - 1) / MMU_PAGE_CELLS);
    for (bank = 0; bank < MMU_RAM_BANKS - MMU_POOL_BASE; ++bank) {
        for (start = 0; start + pages <= MMU_BANK_PAGES; ++start) {
            id = bank * MMU_BANK_PAGES + start;
            for (i = 0; i < pages && !refs()[id + i]; ++i) {}
            if (i != pages) { start += i; continue; }
            for (i = 0; i < pages; ++i) refs()[id + i] = KERNEL_PAGE;
            used_pages += pages;
            result = physical(id);
            if (ripple_zero(result, cells)) panic("kernel allocation clear");
            return result;
        }
    }
    return NULL;
}
void ripple_mmu_kernel_free(void *pointer, unsigned cells) {
    unsigned long address=(unsigned long)pointer;
    unsigned bank=address>>16, off=(unsigned)address, pages, id, i;
    irqflags_t flags=di();
    if(!cells || bank<MMU_POOL_BASE || bank>=MMU_RAM_BANKS || off%MMU_PAGE_CELLS ||
       cells>BANK_CELLS-off) panic("kernel allocation free");
    pages=(unsigned)(((unsigned long)cells+MMU_PAGE_CELLS-1)/MMU_PAGE_CELLS);
    id=(bank-MMU_POOL_BASE)*MMU_BANK_PAGES+off/MMU_PAGE_CELLS;
    for(i=0;i<pages;i++) if(refs()[id+i]!=KERNEL_PAGE) panic("kernel allocation owner");
    for(i=0;i<pages;i++) refs()[id+i]=0;
    used_pages-=pages;
    irqrestore(flags);
}
/* Resolve a validated read-only user span to one physical page for DMA. */
int ripple_mmu_span(unsigned long ptr,unsigned write,unsigned *bank,unsigned *off,unsigned *run) {
    unsigned *p=pte((unsigned)(udata.u_ptab-ptab),(unsigned)(ptr>>16),(unsigned)ptr);
    if((unsigned)ptr>=BANK_CELLS||!p||(p[2]&(MP_PRESENT|MP_READ))!=(MP_PRESENT|MP_READ)||(write&&!(p[2]&MP_WRITE)))return -1;
    *bank=p[0];*off=p[1]+(unsigned)ptr%MMU_PAGE_CELLS;
    *run=MMU_PAGE_CELLS-(unsigned)ptr%MMU_PAGE_CELLS;
    return 0;
}
int ripple_mmu_source(unsigned long ptr,unsigned *bank,unsigned *off,unsigned *run) {
    return ripple_mmu_span(ptr,0,bank,off,run);
}
void ripple_mmu_init(void) {
    memset(refs(), 0, MMU_POOL_PAGES);
    memset(ready, 0, sizeof(ready));
    cursor = used_pages = 0;
    memset(task_pages,0,sizeof(task_pages));
}
void ripple_mmu_free(unsigned task) {
    unsigned bank, off, limit, *p;
    if (!ready[task]) return;
    for (bank = USER_GP; bank <= USER_SB; bank++) {
        if (bank > USER_HEAP_LAST && bank != USER_SB) continue;
        limit=bank_limit(task,bank);
        for (off = 0; off < limit; off += MMU_PAGE_CELLS) {
            p = pte(task, bank, off);
            if (p[2] & MP_PRESENT) unref(pageid(p));
        }
    }
    ready[task] = 0;
    task_pages[task] = 0;
    task_data_end[task] = 0;
    memset(page_high[task],0,sizeof(page_high[task]));
}
int ripple_mmu_alloc(unsigned task) {
    unsigned bank, *p;
    if(ripple_zero(table(task,0),TABLE_CELLS))panic("MMU tables");
    table(task,DROOT)[0] = table_bank(task);
    table(task,DROOT)[1] = table_offset(task) + DLEAF;
    for (bank = 0; bank < RIPPLE_CODE_LEAVES; bank++) {
        table(task,IROOT + bank * 2U)[0] = table_bank(task);
        table(task,IROOT + bank * 2U)[1] = table_offset(task) + ILEAF + bank * 1024U;
    }
    for (bank = USER_GP; bank <= USER_SB; bank++) {
        p = pte(task,bank,0);
        if (!p) continue;
        table(task,DLEAF + bank * 4U)[0] = table_bank(task);
        table(task,DLEAF + bank * 4U)[1] = table_offset(task) + PAGES + (bank == USER_SB ? MMU_HEAP_BANKS : bank - USER_GP) * 512U;
        table(task,DLEAF + bank * 4U)[2] = RWU | MP_PAGED;
    }
    memset(page_high[task],0,sizeof(page_high[task]));
    ready[task] = 1;
    task_pages[task] = 0;
    return 0;
}
void ripple_mmu_select(unsigned task) { ripple_mmu_roots(table_bank(task),table_offset(task)); }
static int newpage(unsigned task, unsigned bank, unsigned off) {
    unsigned id, *p = pte(task,bank,off);
    if (p[2] & MP_PRESENT) return 0;
    id = allocpage();
    if (id == 65535U) return -1;
    if (ripple_zero(physical(id), MMU_PAGE_CELLS)) panic("page clear");
    setpage(p,id);
    if (page_high[task][bank_index(bank)] <= off / MMU_PAGE_CELLS)
        page_high[task][bank_index(bank)] = off / MMU_PAGE_CELLS + 1;
    ++task_pages[task];
    return 0;
}
int ripple_mmu_exec_data(unsigned task, unsigned long cells) {
    unsigned bank, off, limit, *p;
    unsigned available = MMU_POOL_PAGES - used_pages;
    unsigned data_pages;
    if (cells >= 16UL * BANK_CELLS) return -1;
    data_pages = (unsigned)((cells + MMU_PAGE_CELLS - 1UL) / MMU_PAGE_CELLS);
    if (data_pages < MMU_BANK_PAGES) data_pages = MMU_BANK_PAGES;
    /* Exec may reclaim this task's private pages. Check before destroying its
     * old map so ENOMEM leaves the existing executable usable. These helpers
     * do not sleep, and kernel execution is not preemptively scheduled. */
    if (ready[task]) {
        for (bank = USER_GP; bank <= USER_SB; bank++) {
            if (bank > USER_HEAP_LAST && bank != USER_SB) continue;
            limit=bank_limit(task,bank);
            for (off = 0; off < limit; off += MMU_PAGE_CELLS) {
                p = pte(task, bank, off);
                if ((p[2] & MP_PRESENT) && refs()[pageid(p)] == 1) ++available;
            }
        }
    }
    if (available < data_pages + MMU_BANK_PAGES) return -1;
    ripple_mmu_free(task);
    ripple_mmu_alloc(task);
    for (off = 0; off < data_pages; off++)
        if (newpage(task,USER_GP + off / MMU_BANK_PAGES,
                    (off % MMU_BANK_PAGES) * MMU_PAGE_CELLS)) return -1;
    for (off = 0; off < BANK_CELLS; off += MMU_PAGE_CELLS)
        if (newpage(task,USER_SB,off)) return -1;
    task_data_end[task] = ((USER_GP + cells / BANK_CELLS) << 16) | (cells % BANK_CELLS);
    ptab[task].p_size = task_pages[task];
    ripple_mmu_select(task);
    return 0;
}
int ripple_mmu_exec(unsigned task) { return ripple_mmu_exec_data(task, 0); }
/* RXE2 keeps its private binding table in GP and bank-aligns module globals.
 * Only populated pages are allocated; alignment holes consume no physical RAM. */
int ripple_mmu_exec_layout(unsigned task,const unsigned *banks,const unsigned long *cells,unsigned count,unsigned bindings) {
    unsigned available=MMU_POOL_PAGES-used_pages,needed=MMU_BANK_PAGES;
    unsigned i,bank,off,limit,*p;
    unsigned long n,end=(unsigned long)(USER_GP+1)<<16;
    unsigned saved_code[RIPPLE_CODE_MAP_CELLS];
    if(bindings>BANK_CELLS)return -1;
    needed+=(bindings+MMU_PAGE_CELLS-1)/MMU_PAGE_CELLS;
    for(i=0;i<count;i++) {
        if(banks[i]<=USER_GP || banks[i]>USER_HEAP_LAST || cells[i]>(USER_HEAP_LAST+1UL-banks[i])*BANK_CELLS) return -1;
        n=(cells[i]+MMU_PAGE_CELLS-1)/MMU_PAGE_CELLS;
        if(n>MMU_POOL_PAGES-needed) return -1;
        needed+=(unsigned)n;
        if(cells[i]) end=((banks[i]+cells[i]/BANK_CELLS)<<16)|(cells[i]%BANK_CELLS);
    }
    if((end>>16)>USER_HEAP_LAST) return -1;
    if(ready[task]) for(bank=USER_GP;bank<=USER_SB;bank++) {
        limit=bank_limit(task,bank);
        for(off=0;off<limit;off+=MMU_PAGE_CELLS) {
            p=pte(task,bank,off);
            if((p[2]&MP_PRESENT)&&refs()[pageid(p)]==1) ++available;
        }
    }
    if(available<needed) return -1;
    /* Keep the old instruction map visible until exec publishes its complete
     * replacement. A transient empty map would discard still-valid JIT regions
     * for the shared libraries, despite their instruction pages never changing. */
    if(ripple_copy(saved_code,table(task,ILEAF),RIPPLE_CODE_MAP_CELLS))panic("save module code map");
    ripple_mmu_free(task);ripple_mmu_alloc(task);
    if(ripple_copy(table(task,ILEAF),saved_code,RIPPLE_CODE_MAP_CELLS))panic("restore module code map");
    for(off=0;off<BANK_CELLS;off+=MMU_PAGE_CELLS)
        if(newpage(task,USER_SB,off)) panic("module stack allocation");
    for(off=0;off<bindings;off+=MMU_PAGE_CELLS)
        if(newpage(task,USER_GP,off))panic("module bindings allocation");
    for(i=0;i<count;i++) for(n=0;n<cells[i];n+=MMU_PAGE_CELLS)
        if(newpage(task,banks[i]+(unsigned)(n/BANK_CELLS),(unsigned)(n%BANK_CELLS))) panic("module data allocation");
    task_data_end[task]=end;ptab[task].p_size=task_pages[task];ripple_mmu_select(task);return 0;
}
void ripple_mmu_code_clear(unsigned task) {
    if(ripple_zero(table(task,ILEAF),RIPPLE_CODE_MAP_CELLS)) panic("MMU code map");
}
void ripple_mmu_code_add(unsigned task,unsigned virtual_bank,unsigned base,unsigned long count) {
    unsigned bank,banks=(unsigned)((count+BANK_CELLS-1UL)/BANK_CELLS);
    if(virtual_bank>RIPPLE_CODE_MAP_CELLS/4U || banks>RIPPLE_CODE_MAP_CELLS/4U-virtual_bank) panic("MMU module code range");
    for(bank=0;bank<banks;bank++) {
        unsigned *p=table(task,ILEAF+(virtual_bank+bank)*4U);
        p[0]=base+bank;p[1]=0;p[2]=MP_PRESENT|MP_READ|MP_EXEC|MP_USER;p[3]=0;
    }
}
void ripple_mmu_code(unsigned task, unsigned base, unsigned long count) {
    unsigned bank;
    unsigned banks = (unsigned)((count + BANK_CELLS - 1UL) / BANK_CELLS);
    if(ripple_zero(table(task,ILEAF),RIPPLE_CODE_MAP_CELLS))panic("MMU code map");
    for (bank = 0; bank < banks; bank++) {
        unsigned *p = table(task, ILEAF + bank * 4U);
        p[0] = base + bank;
        p[1] = 0;
        p[2] = (unsigned long)bank * BANK_CELLS < count ? MP_PRESENT|MP_READ|MP_EXEC|MP_USER : 0;
    }
    ripple_mmu_flush_code();
}
int ripple_mmu_fork(unsigned parent, unsigned child) {
    unsigned bank, off, limit, *src, *dst;
    ripple_mmu_alloc(child);
    if(ripple_copy(table(child,ILEAF),table(parent,ILEAF),RIPPLE_CODE_MAP_CELLS))panic("fork code map");
    memcpy(page_high[child],page_high[parent],sizeof(page_high[child]));
    for (bank = USER_GP; bank <= USER_SB; bank++) {
        if (bank > USER_HEAP_LAST && bank != USER_SB) continue;
        limit=bank_limit(parent,bank);
        for (off=0; off<limit; off+=MMU_PAGE_CELLS) {
            src=pte(parent,bank,off); dst=pte(child,bank,off);
            if (!(src[2]&MP_PRESENT)) continue;
            ++refs()[pageid(src)];
            src[2]=(src[2]&~MP_WRITE)|MP_COW;
            memcpy(dst,src,4);
        }
    }
    task_pages[child] = task_pages[parent];
    task_data_end[child] = task_data_end[parent];
    ptab[child].p_size = task_pages[child];
    /* A reused child root may still have compiled code from its previous
     * executable. Fork replaces its instruction map as well as data PTEs. */
    ripple_mmu_flush_code();
    return 0;
}
static int writable(unsigned *p) {
    unsigned old, id;
    if (p[2]&MP_WRITE) return 0;
    if (!(p[2]&MP_COW)) return -1;
    old=pageid(p);
    if (refs()[old]>1) {
        id=allocpage();
        if (id==65535U) return -1;
        if (ripple_copy(physical(id),physical(old),MMU_PAGE_CELLS)) panic("COW copy");
        unref(old);
        setpage(p,id);
    } else p[2]=(p[2]|MP_WRITE)&~MP_COW;
    ripple_mmu_flush();
    return 0;
}
usize_t ripple_mmu_validate(unsigned long v, usize_t size, unsigned write) {
    unsigned task = (unsigned)(udata.u_ptab - ptab), bank = v >> 16, off = (unsigned)v;
    usize_t valid = 0;
    unsigned n, *p;
    while (size && off < BANK_CELLS && (p=pte(task,bank,off)) != NULL &&
           (p[2]&(MP_PRESENT|MP_READ))==(MP_PRESENT|MP_READ) &&
           (!write || (p[2]&(MP_WRITE|MP_COW)))) {
        if (write && writable(p)) {udata.u_error=ENOMEM;break;}
        n=MMU_PAGE_CELLS-off%MMU_PAGE_CELLS;
        if (n>BANK_CELLS-off) n=BANK_CELLS-off;
        if ((usize_t)n>size) n=(unsigned)size;
        valid+=n; size-=n; off+=n;
        if (off==BANK_CELLS) {off=0;bank++;}
    }
    return valid;
}
unsigned ripple_fault(void) {
    unsigned cause=ripple_mmu_cause(), bank=ripple_mmu_fault_bank(), off=ripple_mmu_fault_off();
    unsigned task=(unsigned)(udata.u_ptab-ptab), *p;
    p=off<BANK_CELLS ? pte(task,bank,off) : NULL;
    if (cause==3 && p && (p[2]&(MP_PRESENT|MP_COW))==(MP_PRESENT|MP_COW)) {
        if (!writable(p)) return 0;
        if (!ripple_mmu_saved_mode()) panic("unvalidated kernel COW write");
        ripple_trap_action=2;
        return 1;
    }
    if (!ripple_mmu_saved_mode()) panic("kernel MMU fault");
    ripple_trap_action=cause==1 ? 1 : (cause==5 || cause==6 ? 3 : 2);
    return 1;
}
void ripple_trap(void) {
    unsigned *f=(unsigned *)((unsigned long)ripple_kernel_stack<<16);
    unsigned long ptr=((unsigned long)f[6]<<16)|f[5];
    long result;
    if (ripple_trap_action!=1) {
        udata.u_insys=1;
        __hard_ei();
        doexit(ripple_trap_action==3 ? SIGILL : SIGSEGV);
    }
    if (f[6]==65535U) ptr=((unsigned long)USER_GP<<16)|f[5];
    if (f[6]==65534U) ptr=((unsigned long)USER_SB<<16)|f[5];
    result=ripple_dispatch((struct ripple_request *)ptr);
    /* fork/scheduling can resume this C frame on another kernel stack. */
    f=(unsigned *)((unsigned long)ripple_kernel_stack<<16);
    f[3]=(unsigned)result; f[4]=(unsigned)((unsigned long)result>>16);
    ripple_signal_frame();
    ripple_resume_user();
    panic("MMU trap returned");
}
/* Packed pointers have a gap between canonical banks. brk/sbrk count cells,
 * not those gaps. Static globals occupy a prefix of the data/heap window. */
static unsigned long dense(unsigned long p) {
    return (p>>16)*BANK_CELLS+(unsigned)p;
}
arg_t brk_extend(uaddr_t ptr) {
    unsigned bank=ptr>>16, off=(unsigned)ptr;
    return bank<USER_GP || bank>USER_HEAP_LAST || off>=BANK_CELLS ||
        ptr<task_data_end[(unsigned)(udata.u_ptab-ptab)] ? ENOMEM : 0;
}
/* Record precisely the pages added by this growth attempt. Pre-existing
 * mappings (including the unused part of exec's initial data bank) survive
 * failure unchanged. Reserve one bit per heap page, rounding up the final word. */
#define HEAP_PAGES (MMU_HEAP_BANKS * MMU_BANK_PAGES)
static void trim_banks(unsigned task) {
    unsigned bank, *high;
    for (bank=USER_GP; bank<=USER_SB; ++bank) {
        if (bank>USER_HEAP_LAST && bank!=USER_SB) continue;
        high=&page_high[task][bank_index(bank)];
        while (*high && !(pte(task,bank,(*high-1)*MMU_PAGE_CELLS)[2]&MP_PRESENT)) --*high;
    }
}
static void rollback(unsigned task, unsigned *added) {
    unsigned index, *p;
    for (index = 0; index < HEAP_PAGES; index++) {
        if (!(added[index / 16] & (1U << (index % 16)))) continue;
        p = pte(task, USER_GP + index / MMU_BANK_PAGES,
                (index % MMU_BANK_PAGES) * MMU_PAGE_CELLS);
        unref(pageid(p)); p[2] = 0; --task_pages[task];
    }
    trim_banks(task);
    ripple_mmu_flush();
    udata.u_ptab->p_size=task_pages[task];
}
arg_t _brk(void) {
    unsigned long target=udata.u_argn, old=udata.u_break;
    unsigned task=(unsigned)(udata.u_ptab-ptab), bank, off;
    unsigned added[(HEAP_PAGES + 15) / 16], index, *p;
    unsigned long pos, end;
    if ((udata.u_error=brk_extend(target))!=0) return -1;
    if (target>old) {
        memset(added, 0, sizeof(added));
        pos=dense(old); end=dense(target);
        while (pos<end) {
            bank=(unsigned)(pos/BANK_CELLS); off=(unsigned)(pos%BANK_CELLS);
            p = pte(task, bank, off);
            if (!(p[2] & MP_PRESENT)) {
                if (newpage(task,bank,off)) {
                    rollback(task,added);
                    udata.u_error=ENOMEM;return -1;
                }
                index = (bank - USER_GP) * MMU_BANK_PAGES + off / MMU_PAGE_CELLS;
                added[index / 16] |= 1U << (index % 16);
            }
            pos+=MMU_PAGE_CELLS-off%MMU_PAGE_CELLS;
        }
        ripple_mmu_flush();
        if (ripple_mmu_validate(old,end-dense(old),1)!=end-dense(old)) {
            rollback(task,added);
            return -1;
        }
        if (ripple_zero((void *)old,end-dense(old))) {
            rollback(task,added);
            return -1;
        }
    } else if (target < old) {
        /* Keep the partial last page; release complete pages above the break.
         * A later growth zeros the newly exposed tail even if it was shared. */
        pos = (dense(target) + MMU_PAGE_CELLS - 1) / MMU_PAGE_CELLS * MMU_PAGE_CELLS;
        end = dense(old);
        while (pos < end) {
            bank = (unsigned)(pos / BANK_CELLS); off = (unsigned)(pos % BANK_CELLS);
            p = pte(task, bank, off);
            if (p[2] & MP_PRESENT) { unref(pageid(p)); p[2] = 0; --task_pages[task]; }
            pos += MMU_PAGE_CELLS;
        }
        ripple_mmu_flush();
    }
    trim_banks(task);
    udata.u_ptab->p_size=task_pages[task];
    udata.u_break=target;
    return 0;
}
arg_t _sbrk(void) {
    unsigned long old=udata.u_break, linear=dense(old);
    long change=udata.u_argn;
    unsigned long next=linear+change;
    if ((change>0 && next<linear)||(change<0 && next>linear)) {udata.u_error=ENOMEM;return -1;}
    udata.u_argn=((next/BANK_CELLS)<<16)|(next%BANK_CELLS);
    if (_brk()) return -1;
    return (arg_t)old;
}
