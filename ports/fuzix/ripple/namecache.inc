/* SPDX-License-Identifier: GPL-2.0-only */
/* Ripple modifications, 2026-10-19. Disk namespace cache; host mounts bypass
 * srch_dir and retain host replacement/containment semantics. Entries contain
 * disk identities, never inode pointers that could survive slot recycling. */
#define NAME_CACHE_SIZE 64
static struct name_entry {
    unsigned valid, dev, parent, number;
    off_t offset;
    uint8_t name[FILENAME_LEN+1];
} name_cache[NAME_CACHE_SIZE];

static struct name_entry *name_slot(inoptr dir, uint8_t *text)
{
    unsigned hash = dir->c_dev * 31U + dir->c_num, n;
    for (n=0; n<FILENAME_LEN && text[n]; ++n) hash = hash*33U + text[n];
    return &name_cache[hash % NAME_CACHE_SIZE];
}
static struct name_entry *name_lookup(inoptr dir, uint8_t *text)
{
    struct name_entry *e = name_slot(dir,text);
    return e->valid && e->dev==dir->c_dev && e->parent==dir->c_num &&
           namecomp(e->name,text) ? e : NULL;
}
void ripple_namecache_drop(uint16_t dev)
{
    unsigned n;
    for (n=0; n<NAME_CACHE_SIZE; ++n)
        if (name_cache[n].dev==dev) name_cache[n].valid=0;
}
static void name_forget(inoptr dir)
{
    unsigned n;
    for (n=0; n<NAME_CACHE_SIZE; ++n)
        if (name_cache[n].dev==dir->c_dev && name_cache[n].parent==dir->c_num)
            name_cache[n].valid=0;
}
static void name_remember(inoptr dir, uint8_t *text, unsigned number, off_t offset)
{
    struct name_entry *e = name_slot(dir,text);
    e->dev=dir->c_dev; e->parent=dir->c_num; e->number=number; e->offset=offset;
    namecpy(e->name,text,FILENAME_LEN); e->name[FILENAME_LEN]=0; e->valid=1;
}
/* One block traversal supplies both the match and first unused insertion slot.
 * Namespace writes validate cached positions before using them. */
static unsigned directory_find(inoptr dir, uint8_t *text, off_t *position)
{
    blkno_t b, count=inode_blocks(dir);
    unsigned j;
    struct direct *d;
    bufptr bp;
    off_t free_at=-1;
    for (b=0; b<count; ++b) {
        bp=bread(dir->c_dev,bmap(dir,b,1),0);
        if (!bp) return 0;
        for (j=0; j<BLKSIZE/DIR_LEN; ++j) {
            d=blkptr(bp,j*DIR_LEN,DIR_LEN);
            if (!d->d_name[0] && free_at<0) free_at=BLK_TO_OFFSET(b)+j*DIR_LEN;
            if (namecomp(text,d->d_name)) {
                unsigned number=d->d_ino;
                *position=BLK_TO_OFFSET(b)+j*DIR_LEN;
                brelse(bp); return number;
            }
        }
        brelse(bp);
    }
    *position=free_at<0?BLK_TO_OFFSET(count):free_at;
    return 0;
}
