#include "kernel.h"
#include "kdata.h"
#include "printf.h"

#if (BLKSIZE == 512) || defined(CONFIG_RIPPLE)

/*
 *	File system routines for the usual 512 byte block size
 */

/* Return the number of blocks an inode occupies assuming all blocks present */
blkno_t inode_blocks(inoptr i)
{
    return (i->c_node.i_size + BLKMASK) >> BLKSHIFT;
}

/* Read an inode */
uint_fast8_t breadi(uint16_t dev, uint16_t ino, void *ptr)
{
    struct blkbuf *buf = bread(dev, (ino / INO_PER_BLOCK) + 2, 0);
    if (buf == NULL)
        return 1;
    blktok(ptr, buf, sizeof(struct dinode) * (ino % INO_PER_BLOCK), sizeof(struct dinode));
    brelse(buf);
    return 0;
}

/* Write an inode */
uint_fast8_t bwritei(register inoptr ino)
{
    struct blkbuf *buf = bread(ino->c_dev, (ino->c_num / INO_PER_BLOCK) + 2, 0);
    if (buf == NULL)
        return 1;
    blkfromk(&ino->c_node, buf, sizeof(struct dinode) * (ino->c_num % INO_PER_BLOCK),
            sizeof(struct dinode));
    bfree(buf, 2);
    return 0;
}

static blkno_t ifetch(register inoptr ip, unsigned off, unsigned rwflg)
{
    register blkno_t *nb = ip->c_node.i_addr  + off;
    if (*nb == 0) {
        if (rwflg || (*nb = blk_alloc(ip->c_dev)) == 0)
            return NULLBLK;
        ip->c_flags |= CDIRTY;
    }
    return *nb;
}

static blkno_t bfetch(uint16_t dev, blkno_t blk, unsigned off, unsigned rwflg)
{
        register bufptr bp = bread(dev, blk, 0);
        blkno_t nb;

        if (bp == NULL) {
            corrupt_fs(dev);
            return NULLBLK;
        }
        off *= sizeof(blkno_t);

        /* Add a sensible blk sized helper that's just
        nb = blknum(bp, off); and to set likewise so in main memory
           code is compact */
        nb = *(blkno_t *)blkptr(bp, off, sizeof(blkno_t));
        if (nb)
            brelse(bp);
        else {
            if (rwflg || !(nb = blk_alloc(dev))) {
                brelse(bp);
                return NULLBLK;
            }
            blkfromk(&nb, bp, off, sizeof(blkno_t));
            bawrite(bp);
        }
        return nb;
}

blkno_t bmap(register inoptr ip, blkno_t bn, unsigned rwflg)
{
    blkno_t blk;
    const unsigned indirect = BLKSIZE / sizeof(blkno_t);

    if(getmode(ip) == MODE_R(F_BDEV))
        return(bn);

    {
        unsigned level, depth;
        uint32_t span = 1;
        if (bn < FS_DIRECT) return ifetch(ip, (unsigned)bn, rwflg);
        bn -= FS_DIRECT;
        for (depth = 1; depth <= FS_LEVELS; depth++) {
            span *= indirect;
            if (bn < span) break;
            bn -= span;
        }
        if (depth > FS_LEVELS) { udata.u_error = EFBIG; return NULLBLK; }
        blk = ifetch(ip, FS_DIRECT + depth - 1, rwflg);
        for (level = depth; level; level--) {
            if (blk == NULLBLK) return blk;
            span /= indirect;
            blk = bfetch(ip->c_dev, blk, (unsigned)(bn / span), rwflg);
            bn %= span;
        }
        return blk;
    }
}

#endif
