/* SPDX-License-Identifier: GPL-2.0-only */
/* Ripple modifications, 2026-10-19: supervisor storage and bulk ring copies. */
#include <kernel.h>
#include <kdata.h>
#ifdef CONFIG_FUSE_X
#include <native_process.h>
#else
#include "mmu.h"
#endif
void *ripple_kernel_alloc(unsigned cells)
{
    void *p;
    irqflags_t irq=di();
#ifdef CONFIG_FUSE_X
    p=rcc_process_kernel_alloc(cells);
#else
    p=ripple_mmu_kernel_alloc(cells);
#endif
    irqrestore(irq);
    return p;
}

/* Caller masks IRQs through queue observation and update. Copies do not sleep;
 * a failed user copy never advances the queue or publishes partial input. */
int ripple_queue_read(struct s_queue *q, uint8_t *dest, unsigned limit, unsigned sysio)
{
    unsigned n=min(limit,q->q_count), first=min(n,q->q_base+q->q_size-q->q_head);
    if (!n) return 0;
    if (sysio) {
        memcpy(dest,q->q_head,first); memcpy(dest+first,q->q_base,n-first);
    } else if (_uput(q->q_head,dest,first) ||
               (n>first && _uput(q->q_base,dest+first,n-first))) return -1;
    q->q_head+=first;
    if (q->q_head==q->q_base+q->q_size) q->q_head=q->q_base;
    q->q_head+=n-first; q->q_count-=n;
    return n;
}
int ripple_queue_write(struct s_queue *q, const uint8_t *src, unsigned limit, unsigned sysio)
{
    unsigned n=min(limit,q->q_size-q->q_count), first=min(n,q->q_base+q->q_size-q->q_tail);
    if (!n) return 0;
    if (sysio) {
        memcpy(q->q_tail,src,first); memcpy(q->q_base,src+first,n-first);
    } else if (_uget(src,q->q_tail,first) ||
               (n>first && _uget(src+first,q->q_base,n-first))) return -1;
    /* Terminal queues carry octets even though a Ripple C character occupies
     * a 16-bit cell. Match the scalar tty_putc/tty_inproc truncation. */
    {
        unsigned i;
        for (i=0; i<first; ++i) q->q_tail[i]&=255;
        for (i=0; i<n-first; ++i) q->q_base[i]&=255;
    }
    q->q_tail+=first;
    if (q->q_tail==q->q_base+q->q_size) q->q_tail=q->q_base;
    q->q_tail+=n-first; q->q_count+=n;
    return n;
}
