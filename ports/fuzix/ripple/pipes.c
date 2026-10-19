/* SPDX-License-Identifier: GPL-2.0-only */
/* Ripple modifications, 2026-10-19: shared supervisor pipe rings. */
#include <kernel.h>
#include <kdata.h>
#ifdef CONFIG_RIPPLE_IO
#define PIPE_CAPACITY (8U * BLKSIZE)
/* Storage belongs to the kernel, including across native fork/COW. Allocated
 * buffers are pooled across inode slots in supervisor storage. No disk inode
 * or block is allocated. */
static struct pipe_ring { uint8_t *data; unsigned head; } rings[ITABSIZE];
static uint8_t *free_rings[ITABSIZE];
static unsigned free_ring_count;

inoptr ripple_pipe_new(void)
{
    inoptr ino;
    for (ino = i_tab; ino < i_tab + ITABSIZE; ++ino)
        if (!ino->c_refs) {
            memset(ino, 0, sizeof(*ino));
            ino->c_magic = CMAGIC;
            ino->c_dev = NO_DEVICE;
            ino->c_num = ino - i_tab + 1;
            ino->c_refs = 1;
            return ino;
        }
    udata.u_error = ENFILE;
    return NULLINODE;
}

void ripple_pipe_release(inoptr ino)
{
    struct pipe_ring *ring=&rings[ino-i_tab];
    if (ring->data) {
        free_rings[free_ring_count++]=ring->data;
        ring->data=NULL;
    }
    ring->head=0;
    ino->c_node.i_size = 0;
}

void ripple_pipe_io(inoptr ino, unsigned writing, unsigned flags)
{
    struct pipe_ring *ring = &rings[ino-i_tab];
    unsigned n, at, first, count;
    irqflags_t irq = di();
    udata.u_done = 0;
    for (;;) {
        count = ino->c_node.i_size;
        if (writing) {
            if (!ino->c_readers) {
                udata.u_error = EPIPE;
                ssig(udata.u_ptab, SIGPIPE);
                break;
            }
            /* A write fitting in the ring is indivisible, even across wrap.
             * Large writes may return available space as a short write. */
            if (count < PIPE_CAPACITY &&
                (udata.u_count > PIPE_CAPACITY ||
                 udata.u_count <= PIPE_CAPACITY-count)) break;
        } else if (count || !ino->c_writers) break;
        if (psleep_flags(ino, flags)) break;
        di();
    }
    if (udata.u_error) goto done;
    n = min(udata.u_count, writing ? PIPE_CAPACITY-count : count);
    if (!n) goto done;
    if (!ring->data) {
        ring->data = free_ring_count ? free_rings[--free_ring_count] :
                     ripple_kernel_alloc(PIPE_CAPACITY);
        if (!ring->data) { udata.u_error = ENOMEM; goto done; }
    }
    at = writing ? (ring->head+count) % PIPE_CAPACITY : ring->head;
    first = min(n, PIPE_CAPACITY-at);
    if (udata.u_sysio) {
        if (writing) {
            memcpy(ring->data+at, udata.u_base, first);
            memcpy(ring->data, udata.u_base+first, n-first);
        } else {
            memcpy(udata.u_base, ring->data+at, first);
            memcpy(udata.u_base+first, ring->data, n-first);
        }
    } else if (writing) {
        if (_uget(udata.u_base, ring->data+at, first) ||
            (n>first && _uget(udata.u_base+first, ring->data, n-first))) goto done;
    } else if (_uput(ring->data+at, udata.u_base, first) ||
               (n>first && _uput(ring->data, udata.u_base+first, n-first))) goto done;
    if (writing) ino->c_node.i_size += n;
    else {
        ring->head = (ring->head+n) % PIPE_CAPACITY;
        ino->c_node.i_size -= n;
    }
    umove(n);
    wakeup(ino);
    selwake_inode(ino, writing ? SELECT_IN : SELECT_OUT);
done:
    if (udata.u_error && !udata.u_done) udata.u_done = (usize_t)-1;
    irqrestore(irq);
}
#endif
