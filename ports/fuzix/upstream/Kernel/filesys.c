/* Ripple modifications, 2026-10-19: kernel IO/cache integration. */
#undef DEBUG
#include <kernel.h>
#include <kdata.h>
#include <printf.h>

/*
 * There are only two places in the core kernel that know about buffer
 * data and manipulate it directly. This is one of them, and  mm.c is the
 * other. Please keep it that way if at all possible because at some point
 * we will begin supporting out of memory map buffers.
 */

/* N_open is given a string containing a path name in user space,
 * and returns an inode table pointer.  If it returns NULL, the file
 * did not exist.  If the parent existed, and parent is not null,
 * parent will be filled in with the parents inoptr. Otherwise, parent
 * will be set to NULL.
 * The last node parsed is saved in lastname and is useful to some system
 * calls as they want a parent and to create the new node.
 */

uint8_t lastname[31];

static uint_fast8_t n_open_fault;
static uint_fast8_t n_fault_type;
static uint8_t *name, *nameend;

static uint8_t getcf(void)
{
    if (name == nameend) {
        udata.u_error = n_fault_type;
        n_open_fault = 1;
        return 0;
    }
    return *name;
}

/* Link contents use ordinary file blocks, without a terminating NUL on disk.
 * Keep this separate from readi: pathname lookup must not alter syscall I/O. */
int readlinki(inoptr ino, uint8_t *target)
{
    struct blkbuf *bp;
    blkno_t block;
    unsigned len = ino->c_node.i_size, j;
    if (getmode(ino) != MODE_R(F_LNK)) {
        udata.u_error = EINVAL;
        return -1;
    }
    if (!len || ino->c_node.i_size >= 512) {
        udata.u_error = EIO;
        return -1;
    }
    block = bmap(ino, 0, 1);
    if (!block || block == NULLBLK || !(bp = bread(ino->c_dev, block, 0))) {
        udata.u_error = EIO;
        return -1;
    }
    blktok(target, bp, 0, len);
    brelse(bp);
    for (j = 0; j < len; j++) {
        if (!target[j]) { udata.u_error = EIO; return -1; }
    }
    target[len] = 0;
    return len;
}

inoptr n_open(uint8_t *namep, inoptr *parent)
{
    return n_open_flags(namep, parent, 0);
}

inoptr n_open_flags(uint8_t *namep, inoptr *parent, unsigned flags)
{
    register inoptr wd;
    register inoptr ninode;
    register uint8_t *fp;
    register inoptr temp;
    uint8_t c;
    usize_t len, j, suffix;
    unsigned links = 0;
    int target_len;
    uint8_t pathbuf[512], expanded[512];

    if (parent)
        *parent = NULLINODE;
    len = valaddr_r(namep, 512, 1);
    if (!len)
        return NULLINODE;
    if (_uget(namep, pathbuf, len))
        return NULLINODE;
    for (j = 0; j < len && pathbuf[j]; j++);
    if (j == len) {
        udata.u_error = len == 512 ? ENAMETOOLONG : EFAULT;
        return NULLINODE;
    }
    if (!j) { udata.u_error = ENOENT; return NULLINODE; }
    name = pathbuf;
    nameend = pathbuf + 512;
    n_open_fault = 0;
    n_fault_type = ENAMETOOLONG;

    if(getcf() == '/')
        wd = udata.u_root;
    else
        wd = udata.u_cwd;

    ninode = i_ref(wd);
    i_ref(ninode);

    for(;;)
    {
        /* ninode is the inode we are walking from at this point and wd
           the parent. They may be the same. We hold one reference to each */
        if(ninode)
            magic(ninode);

        /* cheap way to spot rename inside yourself */
        if (udata.u_rename == ninode)
            udata.u_rename = NULLINODE;

        /* See if we are at a mount point.
           If we are a mount point then we swap the parent inode for
           the child inode and swap the reference to the child
           Q: could we set an incore inode flag for mountpoint to speed
              this up ? */
        if(ninode)
            ninode = srch_mt(ninode);

        /* Resolve relative targets from the link's parent, absolute targets
           from the guest root. Expansion is iterative, with a shared limit
           for intermediate and final links (including cycles). */
        if (ninode && getmode(ninode) == MODE_R(F_LNK)) {
            fp = name;
            while (*fp == '/') fp++;
            if ((flags & N_NODEREF) && *name && !*fp) {
                udata.u_error = ENOTDIR;
                goto node_error;
            }
            if (!(flags & N_NOFOLLOW) || *name) {
                if (++links > 40) { udata.u_error = ELOOP; goto node_error; }
                target_len = readlinki(ninode, expanded);
                if (target_len < 0) goto node_error;
                suffix = strlen((char *)name);
                if (target_len + suffix >= 512) {
                    udata.u_error = ENAMETOOLONG;
                    goto node_error;
                }
                memcpy(expanded + target_len, name, suffix + 1);
                memcpy(pathbuf, expanded, target_len + suffix + 1);
                name = pathbuf;
                i_deref(ninode);
                if (*name == '/') {
                    i_deref(wd);
                    wd = i_ref(udata.u_root);
                }
                ninode = i_ref(wd);
                continue;
            }
        }
        /* A trailing slash requires a directory; callers creating a new
           directory may retain the missing terminal component. */
        if (*name == '/') {
            fp = name;
            while (*fp == '/') fp++;
            if (!*fp && ninode && getmode(ninode) != MODE_R(F_DIR)) {
                udata.u_error = ENOTDIR;
                goto node_error;
            }
            if (!*fp && !ninode && !(flags & N_MKDIR)) {
                udata.u_error = ENOENT;
                goto nodir;
            }
        }

        /* Skip any slashes between nodes. The standards say there can be
           multiple slashes */
        while((c = getcf()) == '/')
            ++name;
        /* It is acceptable to end a file path with / */
        if(!c || n_open_fault)           /* No more components of path? */
            break;

        /* If we failed to find our node we are done */
        if(!ninode){
            udata.u_error = ENOENT;
            goto nodir;
        }
        /* Drop the reference to the old parent */
        i_deref(wd);
        /* Make the parent our own node. We now hold a single reference to
           wd/ninode */
        wd = ninode;
        /* If we are still searching and the parent node is not a directory
           we are done and we failed */
        if(getmode(wd) != MODE_R(F_DIR)){
            udata.u_error = ENOTDIR;
            goto nodir;
        }
        /* If we are now allowed to access the directory then we are done */
        if(!(getperm(wd) & OTH_EX)){
            udata.u_error = EACCES;
            goto nodir;
        }

        /* Walk the filename until / or end. Store the filename of up to
           30 characters in lastname which is also used by our callers. It
           is permissible to give longer name that matches the first 30 */
        fp = lastname;
        while((c = getcf()) != '\0') {
            if (c == '/')
                break;
            if (fp != lastname + 30)
                *fp++ = c;
            ++name;
        }
        /* Terminate the lastname buffer with \0 */
        *fp = 0;
        /* We are going up through a mount point if
           either:
           - We are accessing the root inode
           - We are accessing the root inode number of a device
           and:
           - Our path is ..

           FIXME: re-order tests for speed */
        if((wd == udata.u_root || (wd->c_num == ROOTINODE && wd->c_dev != root_dev)) &&
                lastname[0] == '.' && lastname[1] == '.' && lastname[2] == '\0') {
            /* We are doing /../ */
            if (wd == udata.u_root) {
                ninode = i_ref(wd);
                continue;
            }
            /* Find the mount point inode, which is hidden by the mount */
            temp = fs_tab[wd->c_super].m_mntpt;
            /* Take a reference to it */
            i_ref(temp);
            /* Drop the old directory */
            i_deref(wd);
            /* Fall through so we walk the mount point directory .. entry */
            wd = temp;
        }
        /* Find the entry in the directory. ninode will be NULL if we failed or
           valid and referenced if it existed */
        ninode = srch_dir(wd, lastname);
    }
    /* If we faulted then treat it as invalid */
    if (n_open_fault) {
        udata.u_error = n_fault_type;
        goto node_error;
    }

    /* Return the parent node if requested. This is needed by callers that
       do directory manipulation */
    if(parent)
        *parent = wd;
    else
        i_deref(wd);
    /* Check if we failed */
    if(!(parent || ninode))
        udata.u_error = ENOENT;
    /* Return the target node if found. NULL with a valid parent is quite
       possible and indicates the directory exists but the filename is new */
    return ninode;

node_error:
    if (ninode) i_deref(ninode);
nodir:
    if(parent)
        *parent = NULLINODE;
    i_deref(wd);
    return NULLINODE;
}

#ifdef CONFIG_RIPPLE_IO
#include "namecache.inc"
/* Small direct-mapped inode index. Validate the complete identity on every
 * hit: host/socket/anonymous inode allocation can recycle cache slots. */
static uint8_t inode_index[256];
#endif

/* Srch_dir is given an inode pointer of an open directory and a string
 * containing a filename, and searches the directory for the file.  If
 * it exists, it opens it and returns the inode pointer, otherwise NULL.
 * This depends on the fact that ba_read will return unallocated blocks
 * as zero-filled, and a partially allocated block will be padded with
 * zeroes.
 */

inoptr srch_dir(register inoptr wd, uint8_t *compname)
{
#ifdef CONFIG_RIPPLE_IO
    struct name_entry *e=name_lookup(wd,compname);
    unsigned number;
    off_t at=0;
    if (e) return e->number ? i_open(wd->c_dev,e->number) : NULLINODE;
    number=directory_find(wd,compname,&at);
    if (!udata.u_error) name_remember(wd,compname,number,at);
    return number ? i_open(wd->c_dev,number) : NULLINODE;
#else
    register struct direct *d;
    register blkno_t curblock;
    register struct blkbuf *buf;
    uint_fast8_t curentry;
    int nblocks;
    uint16_t inum;

    nblocks = inode_blocks(wd);

    for(curblock=0; curblock < nblocks; ++curblock) {
        buf = bread(wd->c_dev, bmap(wd, curblock, 1), 0);
        if (buf == NULL)
            break;
        for(curentry = 0; curentry < (BLKSIZE / DIR_LEN); ++curentry) {
            d = blkptr(buf, curentry * DIR_LEN, DIR_LEN);
            if(namecomp(compname, d->d_name)) {
                inum = d->d_ino;
                brelse(buf);
                return i_open(wd->c_dev, inum);
            }
        }
        brelse(buf);
    }
    return NULLINODE;
#endif
}


/* Srch_mt sees if the given inode is a mount point. If so it
 * dereferences it, and references and returns a pointer to the
 * root of the mounted filesystem.
 */

inoptr srch_mt(inoptr ino)
{
    register struct mount *m = &fs_tab[0];
    register uint_fast8_t j;

    for(j=0; j < NMOUNTS; ++j){
        if(m->m_dev != NO_DEVICE &&  m->m_mntpt == ino) {
            i_deref(ino);
            return i_open(m->m_dev, ROOTINODE);
        }
        m++;
    };
    return ino;
}


/* Most of the time we open the first argument with no
 * parent info needed. So we have a helper
 */

inoptr n_open_argn(void)
{
    return n_open((uint8_t *)udata.u_argn, NULL);
}

/* I_open is given an inode number and a device number,
 * and makes an entry in the inode table for them, or
 * increases it reference count if it is already there.
 * An inode # of zero means a newly allocated inode.
 *
 * Once we support sleeping on bigger boxes during I/O we will need
 * a lock (superblock lock perhaps) to cover allocation of blocks and
 * inodes.
 */

inoptr i_open(uint16_t dev, uint16_t ino)
{
    register inoptr nindex;
    register inoptr j;
    struct mount *m;
    bool isnew = false;

    validchk(dev, PANIC_IOPEN);

    if(!ino){        /* ino==0 means we want a new one */
        isnew = true;
        ino = i_alloc(dev);
        if(!ino) {
            udata.u_error = ENOSPC;
            return NULLINODE;
        }
    }

    m = fs_tab_get(dev);

    /* Maybe make this DEBUG only eventually - the fs_tab_get cost
       is higher than ideal */
    if(ino < ROOTINODE || ino >= (m->m_fs.s_isize - 2) * INO_PER_BLOCK) {
        kputs("i_open: bad inode number\n");
        return NULLINODE;
    }

#ifdef CONFIG_RIPPLE_IO
    {
        unsigned index=inode_index[(dev*31U+ino)&255U];
        if (index) {
            j=&i_tab[index-1];
            if (j->c_dev==dev && j->c_num==ino) { nindex=j; goto found; }
        }
    }
#endif
    nindex = NULLINODE;
    for(j = i_tab; j<i_tab + ITABSIZE; j++){
        if(!j->c_refs) // free slot?
            nindex = j;

        if(j->c_dev == dev && j->c_num == ino) {
            nindex = j;
            goto found;
        }
    }
    /* Not already in the table. */

    if(!nindex){      /* No unrefed slots in inode table */
        udata.u_error = ENFILE;
        return(NULLINODE);
    }

    if (breadi(dev, ino, &nindex->c_node))
        return NULLINODE;

    nindex->c_dev = dev;
    nindex->c_num = ino;
    nindex->c_super = m - fs_tab;
    nindex->c_magic = CMAGIC;
    nindex->c_flags = (m->m_flags & MS_RDONLY) ? CRDONLY : 0;
found:
#ifdef CONFIG_RIPPLE_IO
    inode_index[(dev*31U+ino)&255U]=nindex-i_tab+1;
#endif
    if(isnew) {
        if(nindex->c_node.i_nlink || nindex->c_node.i_mode & F_MASK)
            goto badino;
        /* A deleted device retains its device number in i_addr[0].
           Anonymous pipes also allocate here, without going through
           newfile(), so clear the complete recycled on-disk inode. */
        memset(&nindex->c_node, 0, sizeof(nindex->c_node));
    } else {
        if(!(nindex->c_node.i_nlink && nindex->c_node.i_mode & F_MASK))
            goto badino;
    }
#if defined(CONFIG_LEVEL_2) || defined(CONFIG_SELECT)
    if (!nindex->c_refs)
        memset(nindex->c_select, 0, sizeof(nindex->c_select));
#endif
    nindex->c_refs++;
    return nindex;

badino:
    kputs("i_open: bad disk inode\n");
    return NULLINODE;
}

bool emptydir(register inoptr wd)
{
    struct direct curentry;

    udata.u_offset =  2 * DIR_LEN;	/* . .. ignored */

    do
    {
        udata.u_count = DIR_LEN;
        udata.u_base  = (uint8_t *)&curentry;
        udata.u_sysio = true;
        readi(wd, 0);
        /* On media error assume the directory is full so we fail safe */
        if (udata.u_error)
            return false;

        /* Read until EOF or name is found.  readi() advances udata.u_offset */
        if (*curentry.d_name)
            return false;
    } while(udata.u_done == DIR_LEN);

    return true;
}


/* Ch_link modifies or makes a new entry in the directory for the name
 * and inode pointer given. The directory is searched for oldname.  When
 * found, it is changed to newname, and it inode # is that of *nindex.
 * A oldname of "" matches a unused slot, and a nindex of NULLINODE
 * means an inode # of 0.  A return status of 0 means there was no
 * space left in the filesystem, or a non-empty oldname was not found,
 * or the user did not have write permission.
 */

/* This needs a proper home */
void namecpy(register uint8_t *to, register uint8_t *from, unsigned len)
{
    while(*from && len--)
        *to++ = *from++;
    while(len--)
        *to++ = 0;
}

/* TODO: we should cache the first free node seen during a directory walk
   so we can speed up insertions after checking for a name ? */
static bool ch_link_mode(register inoptr wd, uint8_t *oldname, uint8_t *newname, inoptr nindex, bool checkperm)
{
    struct direct curentry;

    if (wd->c_flags & CRDONLY) {
        udata.u_error = EROFS;
        return false;
    }
    /* FIXME: for modern style permissions we should also check whether
       wd has the sticky bit set and if so require ownership or root */
    if(checkperm && !(getperm(wd) & OTH_WR))
    {
        udata.u_error = EACCES;
        return false;
    }
    /* Inserting a new blank entry ? */
    if (!*newname && nindex != NULLINODE) {
        udata.u_error = EEXIST;
        return false;
    }

#ifdef CONFIG_RIPPLE_IO
    {
        struct name_entry *e=name_lookup(wd,*oldname?oldname:newname);
        off_t at;
        if (e && ((*oldname && e->number) || (!*oldname && !e->number))) {
            at=e->offset;
            udata.u_offset=at; udata.u_count=DIR_LEN;
            udata.u_base=(uint8_t *)&curentry; udata.u_sysio=true;
            readi(wd,0);
            if (udata.u_error) return false;
            if (!udata.u_done || !namecomp(oldname,curentry.d_name)) e=NULL;
        } else e=NULL;
        if (!e) {
            directory_find(wd,oldname,&at);
            if (udata.u_error) return false;
            udata.u_offset=at; udata.u_count=DIR_LEN;
            udata.u_base=(uint8_t *)&curentry; udata.u_sysio=true;
            readi(wd,0);
        }
        name_forget(wd);
    }
#else
    /* Search the directory for the desired slot. */
    udata.u_offset = 0;
    for(;;) {
        udata.u_count = DIR_LEN;
        udata.u_base = (uint8_t *)&curentry;
        udata.u_sysio = true;
        readi(wd, 0);
        if (udata.u_error || !udata.u_done || namecomp(oldname,curentry.d_name)) break;
    }
#endif
    if (udata.u_error) return false;
    if (!udata.u_done && *oldname) { udata.u_error=ENOENT; return false; }

    namecpy(curentry.d_name, newname, FILENAME_LEN);
#ifdef CONFIG_RIPPLE
    curentry.d_padding = 0;
#endif

    if (nindex)
        curentry.d_ino = nindex->c_num;
    else
        curentry.d_ino = 0;

    /* If an existing slot is being used, we must back up the file offset */
    if (udata.u_done)
        udata.u_offset -= DIR_LEN;

    udata.u_count = DIR_LEN;
    udata.u_base  = (unsigned char*)&curentry;
    udata.u_sysio = true;
    writei(wd, 0);

    if (udata.u_error)
        return false;

    setftime(wd, A_TIME|M_TIME|C_TIME);     /* Sets CDIRTY */

    /* Update file length to next block */
    if (BLKOFF(wd->c_node.i_size))
        wd->c_node.i_size += BLKSIZE - BLKOFF(wd->c_node.i_size);

    return true; // success
}

bool ch_link(register inoptr wd, uint8_t *oldname, uint8_t *newname, inoptr nindex)
{
    return ch_link_mode(wd, oldname, newname, nindex, true);
}

/* rename has already checked both parents. Updating a directory's own .. is
   a kernel metadata operation, even when the directory is not writable. */
bool ch_link_parent(inoptr directory, inoptr parent)
{
    return ch_link_mode(directory, (uint8_t *)"..", (uint8_t *)"..", parent, false);
}

/* Namecomp compares two strings to see if they are the same file name.
 * It stops at FILENAME_LEN chars or a null or a slash. It returns 0 for difference.
 *
 * TODO: This generates crap code on most compilers so we probably ought to
 * turn it into platform asm code.
 */
bool namecomp(register uint8_t *n1, register uint8_t *n2) // return true if n1 == n2
{
    register uint_fast8_t n = FILENAME_LEN;

    while(*n1 && *n1 != '/') {
        if(*n1++ != *n2++)
            return false; // mismatch
        n--;
        if(n==0)
            return true; // match
    }

    return (*n2 == '\0' || *n2 == '/');
}


/* Newfile is given a pointer to a directory and a name, and creates
 * an entry in the directory for the name, dereferences the parent,
 * and returns a pointer to the new inode.  It allocates an inode
 * number, and creates a new entry in the inode table for the new
 * file, and initializes the inode table entry for the new file.
 * The new file will have one reference, and 0 links to it.
 * Better make sure there isn't already an entry with the same name.
 *
 * Returns the new inode locked so nobody can access it before ready. We need
 * to think hard about newfile taking a callback to fix up the ino struct so
 * it's cleaner ???
 */

inoptr newfile(register inoptr pino, uint8_t *name)
{
    register inoptr nindex;
    register uint_fast8_t j;

    /* No parent? */
    if (!pino) {
        udata.u_error = ENXIO;
        goto nogood;
    }

    /* We check getperm before CRDONLY because if you reverse these two
       it breaks gcc 68hc11 3.4 */
    if (!(getperm(pino) & OTH_WR)) {
        udata.u_error = EPERM;
        goto nogood;
    }

    /* First see if parent is writeable */
    if (pino->c_flags & CRDONLY) {
        udata.u_error = EROFS;
        goto nogood;
    }

    if (!(nindex = i_open(pino->c_dev, 0))) {
        udata.u_error = ENFILE;
        goto nogood;
    }

    /* This does not implement BSD style "sticky" groups */
    nindex->c_node.i_uid = udata.u_euid;
    nindex->c_node.i_gid = udata.u_egid;

    nindex->c_node.i_mode = F_REG;   /* For the time being */
    nindex->c_node.i_nlink = 1;
    nindex->c_node.i_size = 0;
    for (j = 0; j < 20; j++) {
        nindex->c_node.i_addr[j] = 0;
    }
    wr_inode(nindex);
    if (!ch_link(pino, (uint8_t *)"", name, nindex)) {
        nindex->c_node.i_nlink = 0;
        nindex->c_flags |= CDIRTY;
        i_deref(nindex);
	/* ch_link sets udata.u_error */
        goto nogood;
    }
    i_deref(pino);
    return nindex;

nogood:
    i_deref(pino);
    return NULLINODE;
}


/* Check the given device number, and return its address in the mount
 * table.  Also time-stamp the superblock of dev, and mark it modified.
 * Used when freeing and allocating blocks and inodes.
 */

fsptr getdev(uint16_t dev)
{
    register struct mount *mnt;
    time_t t;

    mnt = fs_tab_get(dev);

    if (!mnt || mnt->m_fs.s_mounted == 0) {
        panic(PANIC_GD_BAD);
        /* Return needed to persuade SDCC all is ok */
        return NULL;
    }
    if (!(mnt->m_flags & MS_RDONLY)) {
        rdtime(&t);
        mnt->m_fs.s_time = t.low;
        mnt->m_fs.s_timeh = t.high;
        mnt->m_fs.s_fmod = FMOD_DIRTY;
    }
    return &mnt->m_fs;
}


/* Returns true if the magic number of a superblock is corrupt. */

bool inline baddev(fsptr dev)
{
    return(dev->s_mounted != SMOUNTED);
}


/* I_alloc finds an unused inode number, and returns it, or 0
 * if there are no more inodes available.
 *
 * This will need to happen under the superblock lock once we do sleeping
 */

uint16_t i_alloc(uint16_t devno)
{
    staticfast fsptr dev;
    staticfast blkno_t blk;
    register struct dinode *di;
    staticfast uint16_t j;
    register struct blkbuf *buf;
    uint16_t k;
    unsigned ino;
#ifdef CONFIG_RIPPLE_IO
    static blkno_t inode_cursor[NMOUNTS];
    blkno_t walked, first;
#endif

    if(baddev(dev = getdev(devno)))
        goto corrupt;

tryagain:
    if(dev->s_ninode) {
        if(!(dev->s_tinode))
            goto corrupt;
        ino = dev->s_inode[--dev->s_ninode];
        if(ino < 2 || ino >=(dev->s_isize-2)*INO_PER_BLOCK)
            goto corrupt;
        --dev->s_tinode;
        return(ino);
    }
    /* We must scan the inodes, and fill up the table */

#ifdef CONFIG_RIPPLE_IO
    {
        inoptr cached;
        for (cached=i_tab; cached<i_tab+ITABSIZE; ++cached)
            if (cached->c_dev==devno && (cached->c_flags&CDIRTY)) wr_inode(cached);
    }
#else
    sync();           /* Make on-disk inodes consistent */
#endif
    k = 0;
#ifdef CONFIG_RIPPLE_IO
    first=inode_cursor[fs_tab_get(devno)-fs_tab];
    if (first<2 || first>=dev->s_isize) first=2;
    for(walked=0,blk=first; walked<dev->s_isize-2; ++walked) {
#else
    for(blk = 2; blk < dev->s_isize; blk++) {
#endif
        buf = bread(devno, blk, 0);
        if (buf == NULL)
            goto corrupt;
        for(j=0; j < INO_PER_BLOCK; j++) {
            /* Optimisation: add offsetof and use that to reduce blkptr range */
            di = blkptr(buf, sizeof(struct dinode) * j, sizeof(struct dinode));
            if(!(di->i_mode || di->i_nlink))
                dev->s_inode[k++] = INO_PER_BLOCK * (blk - 2) + j;
            if(k == FILESYS_TABSIZE) {
                brelse(buf);
                goto done;
            }
        }
        brelse(buf);
#ifdef CONFIG_RIPPLE_IO
        if (++blk>=dev->s_isize) blk=2;
#endif
    }

done:
#ifdef CONFIG_RIPPLE_IO
    inode_cursor[fs_tab_get(devno)-fs_tab]=blk;
#endif
    if(!k) {
        if(dev->s_tinode)
            goto corrupt;
        udata.u_error = ENOSPC;
        return(0);
    }
    dev->s_ninode = k;
    goto tryagain;

corrupt:
    kputs("i_alloc: corrupt superblock\n");
    dev->s_mounted = 1;
    udata.u_error = ENOSPC;
    return(0);
}


/* I_free is given a device and inode number, and frees the inode.
 * It is assumed that there are no references to the inode in the
 * inode table or in the filesystem.
 *
 * This will need to happen under the superblock lock once we do sleeping
 */

void i_free(uint16_t devno, uint16_t ino)
{
    register fsptr dev;

    if(baddev(dev = getdev(devno)))
        return;

    if(ino < 2 || ino >=(dev->s_isize-2)*INO_PER_BLOCK)
        panic(PANIC_IFREE_BADI);

#ifdef CONFIG_RIPPLE_IO
    ripple_namecache_drop(devno);
#endif
    ++dev->s_tinode;
    if(dev->s_ninode < FILESYS_TABSIZE)
        dev->s_inode[dev->s_ninode++] = ino;
}


/* Blk_alloc is given a device number, and allocates an unused block
 * from it. A returned block number of zero means no more blocks.
 *
 * This will need to happen under the superblock lock once we do sleeping
 */

blkno_t blk_alloc(uint16_t devno)
{
    register fsptr dev;
    register struct blkbuf *buf;
    blkno_t newno;

    if(baddev(dev = getdev(devno)))
        goto corrupt2;

    if(dev->s_nfree <= 0 || dev->s_nfree > FILESYS_TABSIZE)
        goto corrupt;

    newno = dev->s_free[--dev->s_nfree];
    if(!newno)
    {
        if(dev->s_tfree != 0)
            goto corrupt;
        udata.u_error = ENOSPC;
        ++dev->s_nfree;
        return(0);
    }

    /* See if we must refill the s_free array */

    if(!dev->s_nfree)
    {
        buf = bread(devno, newno, 0);
        if (buf == NULL)
            goto corrupt;
        blktok(&dev->s_nfree, buf, 0,
            sizeof(dev->s_nfree) + FILESYS_TABSIZE * sizeof(blkno_t));
        /* This assumes no padding: this is an UZI era assumption */
        brelse(buf);
    }

    validblk(devno, newno);

    if(!dev->s_tfree)
        goto corrupt;
    --dev->s_tfree;

   /*
    * FIXME: When we implement the rest of the bigger block size fs support
    * this routine is responsible for zeroing the entire extent not just the
    * BLKSIZE byte block
    */
    /* Zero out the new block */
    buf = bread(devno, newno, 2);
    if (buf == NULL)
        goto corrupt;
    blkzero(buf);
    bawrite(buf);
    return newno;

corrupt:
    kputs("blk_alloc: corrupt\n");
    dev->s_mounted = 1;
corrupt2:
    udata.u_error = ENOSPC;
    return 0;
}


/* Blk_free is given a device number and a block number,
 * and frees the block.
 *
 * This will need to happen under the superblock lock once we do sleeping
 */

void blk_free(uint16_t devno, blkno_t blk)
{
    fsptr dev;
    struct blkbuf *buf;

    if(!blk)
        return;

    if(baddev(dev = getdev(devno)))
        return;

    validblk(devno, blk);

    if(dev->s_nfree == FILESYS_TABSIZE) {
        buf = bread(devno, blk, 1);
        if (buf) {
            /* nfree must directly preceed the blocks and without padding. That's
               the assumption UZI always had */
            blkfromk(&dev->s_nfree, buf, 0, sizeof(dev->s_nfree) + FILESYS_TABSIZE * sizeof(blkno_t));
            bawrite(buf);
            dev->s_nfree = 0;
        } else
            dev->s_mounted = 1;
    }

    ++dev->s_tfree;
    dev->s_free[(dev->s_nfree)++] = blk;
}


/* Oft_alloc and oft_deref allocate and dereference(and possibly free)
 * entries in the open file table.
 */

oft_result_t oft_alloc(void)
{
    register struct oft *ofp;
    register oft_index_t j = 0;

    for(ofp = of_tab; ofp < of_tab + OFTSIZE ; ++ofp, ++j) {
        if(ofp->o_refs == 0) {
            ofp->o_refs = 1;
            ofp->o_inode = NULLINODE;
            return j;
        }
    }
    udata.u_error = ENFILE;
    return -1;
}

/*
 *	To minimise storage we don't track exclusive locks explicitly. We know
 *	that if we are dropping an exclusive lock then we must be the owner,
 *	and if we are dropping a lock that is not exclusive we must own one of
 *	the non exclusive locks.
 */
void deflock(register struct oft *ofptr)
{
    register inoptr i = ofptr->o_inode;
    register uint_fast8_t c = i->c_flags & CFLOCK;

    if (ofptr->o_access & O_FLOCK) {
        if (c == CFLEX)
            c = 0;
        else
            c--;
        i->c_flags = (i->c_flags & ~CFLOCK) | c;
        wakeup(&i->c_flags);
    }
}

/*
 *	Drop a reference in the open file table. If this is the last reference
 *	from a user file table then drop any file locks, dereference the inode
 *	and mark empty
 */
void oft_deref(oft_index_t of)
{
    register struct oft *ofptr;

    ofptr = of_tab + of;
    if(!(--ofptr->o_refs) && ofptr->o_inode) {
        deflock(ofptr);
        i_deref(ofptr->o_inode);
        ofptr->o_inode = NULLINODE;
    }
}


/* Uf_alloc finds an unused slot in the user file table.*/

int_fast8_t uf_alloc_n(uint_fast8_t base)
{
    register oft_index_t *p;

    if (base >= UFTSIZE) {
        udata.u_error = EINVAL;
        return -1;
    }
    p = udata.u_files + base;
    while(p < udata.u_files + UFTSIZE) {
        if(*p == NO_FILE)
            return p - udata.u_files;
        p++;
    }
    udata.u_error = EMFILE;
    return -1;
}

int_fast8_t uf_alloc(void)
{
    return uf_alloc_n(0);
}

void uf_copy(uint_fast8_t oldd, uint_fast8_t newd)
{
    udata.u_files[newd] = udata.u_files[oldd];
    ++of_tab[udata.u_files[oldd]].o_refs;
    /* dup, dup2 and F_DUPFD create a handle with close-on-exec cleared. */
    uf_cloexec_clear(&udata, newd);
}

int uf_alloc_copy(uint_fast8_t oldd, uint_fast8_t base)
{
    int_fast8_t newd = uf_alloc_n(base);
    if (newd >= 0)
        uf_copy(oldd, newd);
    return newd;
}

/* I_deref decreases the reference count of an inode, and frees it from
 * the table if there are no more references to it.  If it also has no
 * links, the inode itself and its blocks(if not a device) is freed.
 */

void i_deref(register inoptr ino)
{
    uint_fast8_t mode = getmode(ino);

    magic(ino);

    if(!ino->c_refs)
        panic(PANIC_INODE_FREED);

#if defined(CONFIG_RIPPLE) && defined(CONFIG_NET)
    /* Ripple sockets use only the inode cache, without a disk allocation. */
    if (issocket(ino) && ino->c_dev == NO_DEVICE) {
        if (!--ino->c_refs)
            memset(ino, 0, sizeof(*ino));
        return;
    }
#endif

    if (mode == MODE_R(F_PIPE)) {
        wakeup((uint8_t *)ino);
#ifdef CONFIG_RIPPLE_IO
        if (ino->c_dev == NO_DEVICE) {
            if (ino->c_refs == 1) ripple_pipe_release(ino);
            if (!--ino->c_refs) memset(ino,0,sizeof(*ino));
            return;
        }
#endif
    }

    /* If the inode has no links and no refs, it must have
       its blocks freed. */

    if(!(--ino->c_refs || ino->c_node.i_nlink))
        /*
           SN (mcy)
           */
        if (mode == MODE_R(F_REG) || mode == MODE_R(F_DIR) || mode == MODE_R(F_PIPE) || mode == MODE_R(F_LNK))
            f_trunc(ino);

    /* If the inode was modified, we must write it to disk. */
    if(!(ino->c_refs) && (ino->c_flags & CDIRTY))
    {
        if(!(ino->c_node.i_nlink))
        {
            ino->c_node.i_mode = 0;
            i_free(ino->c_dev, ino->c_num);
        }
        wr_inode(ino);
    }
}

void corrupt_fs(uint16_t devno)
{
    struct mount *mnt = fs_tab_get(devno);
    mnt->m_fs.s_mounted = 1;
    kputs("filesystem corrupt.\n");
}
/* Wr_inode writes out the given inode in the inode table out to disk,
 * and resets its dirty bit.
 */

void wr_inode(register inoptr ino)
{
/*    struct blkbuf *buf;
    blkno_t blkno;
*/
    magic(ino);

#if defined(CONFIG_RIPPLE) && defined(CONFIG_NET)
    if (issocket(ino) && ino->c_dev == NO_DEVICE) {
        ino->c_flags &= ~CDIRTY;
        return;
    }
#endif

#ifdef CONFIG_RIPPLE_IO
    if (getmode(ino) == MODE_R(F_PIPE) && ino->c_dev == NO_DEVICE) {
        ino->c_flags &= ~CDIRTY;
        return;
    }
#endif
    if (bwritei(ino))
        corrupt_fs(ino->c_dev);
    else
        ino->c_flags &= ~CDIRTY;
}


/* isdevice(ino) returns true if ino points to a device */
bool isdevice(inoptr ino)
{
    return getmode(ino) == MODE_R(F_CDEV) || getmode(ino) == MODE_R(F_BDEV);
}


/* This returns the device number of an inode representing a device */
uint16_t devnum(inoptr ino)
{
    return (uint16_t)ino->c_node.i_addr[0];
}


/*
 *	f_trunc_blocks frees all the blocks associated with the file, if it
 *	is a disk file. The blocks are freed in reverse order. This is
 *	very important so that they end up on the freelist in the
 *	order we want to allocate them.
 */
int f_trunc_blocks(register inoptr ino, blkno_t nblock)
{
#ifdef CONFIG_RIPPLE_IO
    if (getmode(ino)==MODE_R(F_DIR)) name_forget(ino);
#endif
    unsigned level, j;
    uint32_t indirect = BLKSIZE / sizeof(blkno_t), span = 1;
    uint32_t start = FS_DIRECT, keep;
    uint16_t dev = ino->c_dev;
    if (ino->c_flags & CRDONLY) { udata.u_error = EROFS; return -1; }
    for (level = 1; level <= FS_LEVELS; level++) {
        span *= indirect;
        start += span;
    }
    for (level = FS_LEVELS; level; level--) {
        start -= span;
        keep = nblock > start ? min(nblock - start, span) : 0;
        j = FS_DIRECT + level - 1;
        freeblk(dev, ino->c_node.i_addr[j], level, keep);
        if (!keep) ino->c_node.i_addr[j] = 0;
        span /= indirect;
    }
    for (j = FS_DIRECT; j > nblock; ) {
        --j;
        freeblk(dev, ino->c_node.i_addr[j], 0, 0);
        ino->c_node.i_addr[j] = 0;
    }
    ino->c_flags |= CDIRTY;
    return 0;
}


/* Truncate a file back to nothing using f_trunc_blocks and then write
   the inode size as 0 */
int f_trunc(register inoptr ino)
{
    /* Is it worth checking size already 0 ? */
    if (f_trunc_blocks(ino, 0))
        return -1;
     ino->c_node.i_size = 0;
     return 0;
}

/* Companion function to f_trunc().

   This is the one case where we can't hide the difference between an internal
   and external buffer cache cleanly. The external one has a somewhat higher
   overhead (we could mitigate it by batching perhaps) and also size.

   This is annoying and it would be nice one day to find a clean solution */

/* nblock is the number of data blocks retained below this node. */

/* TODO:
    - split and optimize non BLKBUF_EXTERNAL case to use pointers
    - possibly split 400 and 512 sized block cases so we can get divisions
      out of the path here and down to shift and mask
 */
void freeblk(uint16_t dev, blkno_t blk, uint_fast8_t level, blkno_t nblock)
{
    struct blkbuf *buf;
    blkno_t child;
    uint32_t span = 1, first, keep;
    unsigned depth;
    register int16_t j;
#ifndef CONFIG_BLKBUF_EXTERNAL
    register blkno_t *entries;
#endif

    if (!blk)
        return;
    if (level) {
        buf = bread(dev, blk, 0);
        if (buf == NULL) {
            corrupt_fs(dev);
            return;
        }
        for (depth = 1; depth < level; depth++) span *= BLKSIZE / sizeof(blkno_t);
        first = nblock / span;
#ifndef CONFIG_BLKBUF_EXTERNAL
        entries = blkptr(buf, 0, BLKSIZE);
#endif
        for (j = BLKSIZE / sizeof(blkno_t) - 1; j >= 0; --j) {
            if ((uint16_t)j < first)
                break;
            keep = j == first ? nblock % span : 0;
#ifdef CONFIG_BLKBUF_EXTERNAL
            blktok(&child, buf, j * sizeof(blkno_t), sizeof(blkno_t));
#else
            child = entries[j];
#endif
            freeblk(dev, child, level - 1, keep);
            /* A retained pointer block must not reference freed children. */
            if (nblock && !keep) {
#ifdef CONFIG_BLKBUF_EXTERNAL
                child = 0;
                blkfromk(&child, buf, j * sizeof(blkno_t), sizeof(blkno_t));
#else
                entries[j] = 0;
#endif
            }
        }
        if (nblock)
            bawrite(buf);
        else
            brelse(buf);
    }
    if (nblock)
        return;
#ifdef CONFIG_TRIM
    d_ioctl(dev, HDIO_TRIM_KERNEL, (void *)&blk);
#endif
    blk_free(dev, blk);
}

/* Validblk panics if the given block number is not a valid
 *  data block for the given device.
 */
void validblk(uint16_t dev, register blkno_t num)
{
    register struct mount *mnt;

    mnt = fs_tab_get(dev);

    if(mnt == NULL || mnt->m_fs.s_mounted == 0) {
        panic(PANIC_VALIDBLK_NM);
        return;
    }

    if(num < mnt->m_fs.s_isize || num >= mnt->m_fs.s_fsize)
        panic(PANIC_VALIDBLK_INV);
}


/* This returns the inode pointer associated with a user's file
 * descriptor, checking for valid data structures.
 */
inoptr getinode(uint_fast8_t uindex)
{
    register oft_index_t oftindex;
    register inoptr inoindex;

    if(uindex >= UFTSIZE || udata.u_files[uindex] == NO_FILE) {
        udata.u_error = EBADF;
        return NULLINODE;
    }

    oftindex = udata.u_files[uindex];

    if(oftindex >= OFTSIZE || oftindex == NO_FILE)
        panic(PANIC_GETINO_BADT);

    if((inoindex = of_tab[oftindex].o_inode) < i_tab || inoindex >= i_tab+ITABSIZE)
        panic(PANIC_GETINO_OFT);

    magic(inoindex);
    return(inoindex);
}


/* Super returns true if we are the superuser */
bool super(void)
{
    return(udata.u_euid == 0);
}

/* Similar but this helper sets the error code */
bool esuper(void)
{
    if (udata.u_euid) {
        udata.u_error = EPERM;
        return -1;
    }
    return 0;
}

/* Getperm looks at the given inode and the effective user/group ids,
 * and returns the effective permissions in the low-order 3 bits.
 */
uint8_t getperm(register inoptr ino)
{
    int mode;

    mode = ino->c_node.i_mode;
    /* Root bypasses read/write and directory search permissions, but a
       nondirectory still needs at least one execute bit. Keep access(X_OK)
       and both executable loaders consistent, before script fallback. */
    if(super())
        return getmode(ino) == MODE_R(F_DIR) || (mode & 0111) ? 07 : 06;

    if(ino->c_node.i_uid == udata.u_euid)
        mode >>= 6;
    else if(ino->c_node.i_gid == udata.u_egid)
        mode >>= 3;
#ifdef CONFIG_LEVEL_2
    /* BSD process groups */
    else if (in_group(ino->c_node.i_gid))
        mode >>= 3;
#endif

    return(mode & 07);
}


/* This sets the times of the given inode, according to the flags. */
void setftime(register inoptr ino, register uint_fast8_t flag)
{
    if (ino->c_flags & CRDONLY) return;

    /* Anonymous resources still have observable in-core timestamps, but no
     * disk metadata or mount policy. Keep socket/pipe fstat behavior intact. */
#ifdef CONFIG_RIPPLE_IO
    if (ino->c_dev != NO_DEVICE) {
#endif
        if (flag == A_TIME && fs_tab[ino->c_super].m_flags & MS_NOATIME)
            return;
        ino->c_flags |= CDIRTY;
#ifdef CONFIG_RIPPLE_IO
    }
#endif

    if(flag & A_TIME)
        rdtime32(&(ino->c_node.i_atime));
    if(flag & M_TIME)
        rdtime32(&(ino->c_node.i_mtime));
    if(flag & C_TIME)
        rdtime32(&(ino->c_node.i_ctime));
}

uint8_t getmode(inoptr ino)
{
    /* Shifting by 9 (past permissions) might be more logical but
       8 happens to be cheap */
    return (ino->c_node.i_mode & F_MASK) >> 8;
}

static struct mount *newfstab(void)
{
    register struct mount *m = fs_tab;
    register uint_fast8_t i;
    for (i = 0; i < NMOUNTS; i++) {
        if (m->m_dev == NO_DEVICE)
            return m;
        m++;
    }
    return NULL;
}

struct mount *fs_tab_get(uint16_t dev)
{
    register struct mount *m = fs_tab;
    register uint_fast8_t i;
    for (i = 0; i < NMOUNTS; i++) {
        if (m->m_dev == dev)
            return m;
        m++;
    }
    return NULL;
}

/* Fmount places the given device in the mount table with mount point info. */
struct mount *fmount(uint16_t dev, register inoptr ino, uint16_t flags)
{
    register struct mount *m;
    register struct filesys *fp;
    register bufptr buf;

    if(d_open(dev, 0) != 0)
        return NULL;    /* Bad device */

    m = newfstab();
    if (m == NULL) {
        udata.u_error = EMFILE;
        return NULL;	/* Table is full */
    }

    fp = &m->m_fs;

    /* Get the buffer with the superblock (block 1) */
    buf = bread(dev, 1, 0);
    if (buf == NULL)
        return NULL;
    blktok(fp, buf, 0, sizeof(struct filesys));
    brelse(buf);

#ifdef DEBUG
#ifdef CONFIG_RIPPLE
    kprintf("fp->s_mounted=0x%x, fp->s_isize=0x%x, fp->s_fsize=0x%lx\n",
    fp->s_mounted, fp->s_isize, (unsigned long)fp->s_fsize);
#else
    kprintf("fp->s_mounted=0x%x, fp->s_isize=0x%x, fp->s_fsize=0x%x\n",
    fp->s_mounted, fp->s_isize, fp->s_fsize);
#endif
#endif

    /* See if there really is a filesystem on the device */
    if(fp->s_mounted != SMOUNTED  ||  fp->s_isize >= fp->s_fsize ||
        fp->s_shift > FS_MAX_SHIFT) {
        udata.u_error = EINVAL;
        return NULL;
    }

    if (fp->s_fmod == FMOD_DIRTY) {
        kputs("warning: mounting dirty file system, forcing r/o.\n");
        flags |= MS_RDONLY;
    }
    if (!(flags & MS_RDONLY))
        /* Dirty - and will write dirty mark back to media */
        fp->s_fmod = FMOD_DIRTY;
    else	/* Clean in memory, don't write it back to media */
        fp->s_fmod = FMOD_CLEAN;
    m->m_mntpt = ino;
    if(ino)
        ++ino->c_refs;
    m->m_flags = flags;
    /* Makes our entry findable */
    m->m_dev = dev;

    /* Mark the filesystem dirty on disk */
    sync();

    return m;
}


void magic(inoptr ino)
{
    if(ino->c_magic != CMAGIC)
        panic(PANIC_CORRUPTI);
}

/* This is a helper function used by _unlink and _rename; it doesn't really
 * belong here, but needs to be in common code as it's used from two different
 * syscall banks.
 *
 * FIXME: this could be more efficient if we remembered which directory offset
 * we found the node at lookup time
 */
arg_t unlinki(inoptr ino, inoptr pino, uint8_t *fname)
{
	if (getmode(ino) == MODE_R(F_DIR)) {
		udata.u_error = EISDIR;
		return -1;
	}

	/* Remove the directory entry (ch_link checks perms) */
	if (!ch_link(pino, fname, (uint8_t *)"", NULLINODE))
		return -1;

	/* Decrease the link count of the inode */
	if (!(ino->c_node.i_nlink--)) {
		ino->c_node.i_nlink += 2;
		kprintf("_unlink: bad nlink\n");
	}
	setftime(ino, C_TIME);
	return (0);
}

