/* SPDX-License-Identifier: GPL-2.0-only */
/* Ripple modifications, 2026-10-19: bounded collision chains, slot+1 links. */
#define BUFFER_BUCKETS 64
static unsigned buffer_head[BUFFER_BUCKETS], buffer_next[NBUFS];
static unsigned buffer_bucket(uint16_t dev, blkno_t block)
{
    return (dev*31U + block + (block>>16)) & (BUFFER_BUCKETS-1);
}
static void buffer_remove(bufptr bp)
{
    unsigned slot=bp-bufpool+1;
    unsigned *link=&buffer_head[buffer_bucket(bp->bf_dev,bp->bf_blk)];
    while (*link) {
        if (*link==slot) { *link=buffer_next[slot-1]; break; }
        link=&buffer_next[*link-1];
    }
    buffer_next[slot-1]=0;
}
static void buffer_insert(bufptr bp)
{
    unsigned slot=bp-bufpool+1, bucket=buffer_bucket(bp->bf_dev,bp->bf_blk);
    buffer_next[slot-1]=buffer_head[bucket]; buffer_head[bucket]=slot;
}

/* Complete writes before one barrier per device; failed transfers and barriers
 * keep their buffers dirty so a later sync can retry them. */
static void ripple_bufsync(void)
{
    bufptr bp;
    unsigned m, failed;
    uint16_t dev;
    uint8_t written[NBUFS];
    memset(written,0,sizeof(written));
    for (bp=bufpool; bp<bufpool_end; ++bp) {
        if (bp->bf_dev==NO_DEVICE || !bp->bf_dirty) continue;
        if (bdwrite(bp)!=BLKSIZE) { udata.u_error=EIO; continue; }
        written[bp-bufpool]=1;
    }
    for (bp=bufpool; bp<bufpool_end; ++bp) {
        if (!written[bp-bufpool]) continue;
        dev=bp->bf_dev;
        failed=d_flush(dev);
        if (failed) udata.u_error=EIO;
        for (m=bp-bufpool; m<NBUFS; ++m)
            if (written[m] && bufpool[m].bf_dev==dev) {
                written[m]=0;
                if (!failed) bufpool[m].bf_dirty=false;
            }
    }
}
