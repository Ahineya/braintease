/* Ripple modifications, 2026-10-19: kernel IO/cache integration. */
#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <tty.h>
#include <netdev.h>

#if defined(CONFIG_LARGE_IO_DIRECT)
#define read_direct(dev, flag)		(!udata.u_sysio && CONFIG_LARGE_IO_DIRECT(dev))
#elif (NBUFS >= 32)
#define read_direct(dev, flag)		(flag & O_DIRECT)
#endif

/* This assumes it's called once before we do I/O. That's wrong and we
   need to integrate this into the I/O loop, but when we do it changes
   how we handle the psleep_flags bit. Pipes wrap before 64k so we can
   shorten the check */
static uint8_t wait_pipe_read(register inoptr ino, uint_fast8_t flag)
{
        while((uint16_t)ino->c_node.i_size == 0) {
                if (ino->c_writers == 0 || psleep_flags(ino, flag)) {
                        udata.u_count = 0;
                        return 0;
                }
        }
	udata.u_count = min(udata.u_count, LOWORD(ino->c_node.i_size));
        return 1;
}

/* Wait for our pipe to become writable. We must have enough space and
   a reader */
static uint8_t wait_pipe_write(register inoptr ino, uint_fast8_t flag)
{
    for (;;) {
        if (!ino->c_readers) {
            if (!udata.u_done) udata.u_error = EPIPE;
            ssig(udata.u_ptab, SIGPIPE);
            return 1;
        }
        if (ino->c_node.i_size < 8UL * BLKSIZE) return 0;
        /* A partial write returns progress rather than waiting indefinitely. */
        if (udata.u_done) return 1;
        if (psleep_flags(ino, flag)) return 1;
    }
}
/* We need this because SDCC otherwise makes a right hash of it */
static uint16_t uoff(void)
{
	return BLKOFF((uint16_t)udata.u_offset);
}

uint16_t umove(uint16_t n)
{
	udata.u_offset += n;
	udata.u_count -= n;
	udata.u_done += n;
	udata.u_base += n;
	return udata.u_done;
}

static blkno_t mapcalc(inoptr ino, usize_t *size, uint_fast8_t m)
{
	*size = min(udata.u_count, BLKSIZE - uoff());
	/* We know offset is positive at this point. The cast makes
	   for better code on many CPUs */
	return bmap(ino, BLOCK((unsigned long)udata.u_offset), m);
}

/* Writei (and readi) need more i/o error handling */
void readi(register inoptr ino, uint_fast8_t flag)
{
#ifdef CONFIG_RIPPLE_IO
    if (getmode(ino) == MODE_R(F_PIPE) && ino->c_dev == NO_DEVICE) { ripple_pipe_io(ino,0,flag); return; }
#endif
	usize_t amount;
	blkno_t pblk;
	bufptr bp;
	uint16_t dev;
	bool ispipe;

	dev = ino->c_dev;
	ispipe = false;
	udata.u_done = 0;

	switch (getmode(ino)) {
	case MODE_R(F_DIR):
	case MODE_R(F_REG):
		/* FIXME: we end up doing the ino - udata comparison 3 times, fix this */
		/* See if end of file will limit read */
		if (ino->c_node.i_size <= udata.u_offset)
			udata.u_count = 0;
                else {
			udata.u_count = min(udata.u_count,
				ino->c_node.i_size - udata.u_offset);
                }
		goto loop;

        case MODE_R(F_SOCK):
#ifdef CONFIG_NET
                udata.u_done = sock_read(ino, flag);
                break;
#endif
	case MODE_R(F_PIPE):
		ispipe = true;
		/* This bit really needs to be inside the loop for pipe cases */
		if (!wait_pipe_read(ino, flag))
		        break;
		goto loop;

	case MODE_R(F_BDEV):
		dev = *(ino->c_node.i_addr);

	      loop:
		while (udata.u_count) {
			pblk = mapcalc(ino, &amount, 1);

#if !defined(read_direct)
			bp = NULL;
#else
			/* FIXME: if we ran a bfind then we should hint bread to avoid a
			   second pointless walk */
			if (pblk != NULLBLK && (bp = bfind(dev, pblk)) == NULL && !ispipe && amount == BLKSIZE && read_direct(major(dev), flag)) {
				/* we can transfer direct from disk to the userspace buffer */
				/* FIXME: allow for async queued I/O here. We want
				   an API something like breadasync() that either
				   does the cdread() or queues for a smart platform
				   or box with floppy tape devices */
				off_t uostash;
				usize_t ucstash;
				uostash = udata.u_offset;	            /* stash file offset */
				ucstash = udata.u_count;		    /* stash byte count */
				udata.u_count = amount;                     /* transfer one sector */
				udata.u_offset = BLK_TO_OFFSET((off_t)pblk);/* replace with sector offset on device */
				((*dev_tab[major(dev)].dev_read) (minor(dev), 1, 0)); /* read */
				udata.u_offset = uostash;		    /* restore file offset */
				udata.u_count = ucstash;                    /* restore byte count */
			} else
#endif
			{
				/* we transfer through the buffer pool */
				if (pblk == NULLBLK)
					bp = zerobuf();
				else if (bp == NULL)
					bp = bread(dev, pblk, 0);
				if (bp == NULL)
					break;
				uputblk(bp, uoff(), amount);
				brelse(bp);
			}
			umove(amount);
			if (ispipe && LOWORD(udata.u_offset) >= FS_DIRECT * BLKSIZE)
				udata.u_offset = 0;
			if (ispipe) {
				ino->c_node.i_size -= amount;
				wakeup(ino);
                selwake_inode(ino, SELECT_OUT);
			}
		}
		/* Compute return value */
		if (udata.u_done == 0 && udata.u_error)
			udata.u_done = (usize_t) -1;
		break;

	case MODE_R(F_CDEV):
		udata.u_done = cdread(ino->c_node.i_addr[0], flag);
		break;

	default:
		udata.u_error = ENODEV;
	}
}

void writei(register inoptr ino, uint_fast8_t flag)
{
#ifdef CONFIG_RIPPLE_IO
    if (getmode(ino) == MODE_R(F_PIPE) && ino->c_dev == NO_DEVICE) { ripple_pipe_io(ino,1,flag); return; }
#endif
	usize_t amount;
	bufptr bp;
	bool ispipe = false;
    unsigned syncflag;
	blkno_t pblk;
	uint16_t dev;

	dev = ino->c_dev;
	udata.u_done = 0;

	switch (getmode(ino)) {

	case MODE_R(F_BDEV):
		dev = *(ino->c_node.i_addr);
		goto loop;

#ifdef CONFIG_NET
	case MODE_R(F_SOCK):
		udata.u_done = sock_write(ino, flag);
		break;
#endif
	case MODE_R(F_PIPE):
		ispipe = true;

	case MODE_R(F_DIR):
	case MODE_R(F_REG):
	      loop:
        syncflag = flag & O_SYNC ? 2 : 1;

		while (udata.u_count) {
			if (ispipe) {
				/* Sleep if empty pipe */
				if (wait_pipe_write(ino, flag))
                    break;
			}
                        if ((HIBYTE32(udata.u_offset) & BLKOVERSIZE32)
#ifdef CONFIG_RIPPLE
                            || udata.u_count > 0x7fffffffUL - (uint32_t)udata.u_offset
#endif
                           ) {
                                udata.u_error = EFBIG;
                                ssig(udata.u_ptab, SIGXFSZ);
                                break;
                        }

            pblk = mapcalc(ino, &amount, 0);
            if (ispipe && amount > 8UL * BLKSIZE - ino->c_node.i_size)
                amount = 8UL * BLKSIZE - ino->c_node.i_size;
			if (pblk == NULLBLK)
				break;	/* No space to make more blocks */

			/* If we are writing an entire block, we don't care
			 * about its previous contents
			 */
			bp = bread(dev, pblk, (amount == BLKSIZE));
			if (bp == NULL)
				break;

			ugetblk(bp, uoff(), amount);

			/* O_SYNC */
			if (bfree(bp, syncflag))
				break;

			umove(amount);
			if (ispipe) {
				if (LOWORD(udata.u_offset) >= FS_DIRECT * BLKSIZE)
					udata.u_offset = 0;
				ino->c_node.i_size += amount;
				/* Wake up any readers */
				wakeup(ino);
                selwake_inode(ino, SELECT_IN);
			}
		}

		/* Update size if file grew */
		if (!ispipe) {
			if (udata.u_offset > ino->c_node.i_size) {
				ino->c_node.i_size = udata.u_offset;
				ino->c_flags |= CDIRTY;
			}
		}
		/* Compute return value */
		if (udata.u_done == 0 && udata.u_error)
			udata.u_done = (usize_t) -1;
		break;

	case MODE_R(F_CDEV):
		udata.u_done = cdwrite(ino->c_node.i_addr[0], flag);
		break;
	default:
		udata.u_error = ENODEV;
	}
}

int16_t doclose(uint_fast8_t uindex)
{
	oft_index_t oftindex;
	struct oft *oftp;
	register inoptr ino;
	uint16_t flush_dev = NO_DEVICE;
	uint8_t m;

	if (!(ino = getinode(uindex)))
		return (-1);

	oftindex = udata.u_files[uindex];
	oftp = of_tab + udata.u_files[uindex];
	m = O_ACCMODE(oftp->o_access);

	if (oftp->o_refs == 1) {
		if (isdevice(ino))
			d_close((int) (ino->c_node.i_addr[0]));
		if (getmode(ino) == MODE_R(F_REG) && m)
			flush_dev = ino->c_dev;
#ifdef CONFIG_NET
		if (issocket(ino))
			sock_close(ino);
#endif
		if (m != O_RDONLY)
			ino->c_writers--;
		if (m != O_WRONLY)
			ino->c_readers--;
	}
    if (getmode(ino) == MODE_R(F_PIPE)) {
        wakeup(ino);selwake_inode(ino, SELECT_IN|SELECT_OUT);
    }
	udata.u_files[uindex] = NO_FILE;
	uf_cloexec_clear(&udata, uindex);
	oft_deref(oftindex);

	/* if we closed a file in write mode, flush the device's cache after inode has been deferenced */
	if(flush_dev != NO_DEVICE)
		d_flush(flush_dev);

	return (0);
}

inoptr rwsetup(bool is_read, uint_fast8_t * flag)
{
	inoptr ino;
	register struct oft *oftp;

	udata.u_sysio = false;	/* I/O to user data space */
	udata.u_base = (unsigned char *) udata.u_argn1;	/* buf */
	udata.u_count = (susize_t) udata.u_argn2;	/* nbytes */

	if ((ino = getinode(udata.u_argn)) == NULLINODE) {
		/* kprintf("[WRS: rwsetup(): getinode(%x) fails]", udata.u_argn); */
		return (NULLINODE);
	}

	oftp = of_tab + udata.u_files[udata.u_argn];
	*flag = oftp->o_access;
	if (O_ACCMODE(oftp->o_access) == (is_read ? O_WRONLY : O_RDONLY)) {
		udata.u_error = EBADF;
		return (NULLINODE);
	}
	setftime(ino, is_read ? A_TIME : (A_TIME | M_TIME | C_TIME));

	if (getmode(ino) == MODE_R(F_REG) && is_read == 0
	    && (oftp->o_access & O_APPEND))
		oftp->o_ptr = ino->c_node.i_size;
	/* Initialize u_offset from file pointer */
	udata.u_offset = oftp->o_ptr;
	return ino;
}

/*
 *	FIXME: could we rewrite this so we just passed the oft slot and
 *	did the work in that then picked it up in open. That would feel
 *	less like ugly layering violations and is probably shorter and
 *	much cleaner, and gets rid of the itmp ugly.
 *
 *	FIXME. Need so IS_TTY(dev) defines too and minor(x) etc
 */
int dev_openi(inoptr *ino, uint16_t flag)
{
        int ret;
        register inoptr i = *ino;
        uint16_t da = i->c_node.i_addr[0];
        /* Handle the special casing where we need to know about inodes */

        /* /dev/tty processing */
        if (da == 0x0200) {
                if (!udata.u_ctty) {
                        udata.u_error = ENODEV;
                        return -1;
                }
                i_deref(i);
                *ino = i = udata.u_ctty;
                da = i->c_node.i_addr[0];
                i_ref(i);
                /* fall through opening the real device */
        }
        /* normal device opening */
        ret = d_open((int)da, flag);
        /* errors and non tty opens */
        if (ret != 0 || (da & 0xFF00) != 0x0200)
                return ret;
        /* tty post processing */
        tty_post(i, da & 0xFF, flag);
        return 0;
}

static void sync_mounts(void)
{
	register struct mount *m;
	register bufptr buf;

	for (m = fs_tab; m < fs_tab + NMOUNTS; m++) {
		if (m->m_dev != NO_DEVICE &&
			m->m_fs.s_fmod != FMOD_CLEAN) {
			/* GO_CLEAN means write a CLEAN to the media */
			if (m->m_fs.s_fmod == FMOD_GO_CLEAN)
				m->m_fs.s_fmod = FMOD_CLEAN;
			buf = bread(m->m_dev, 1, 1);
			if (buf) {
				blkfromk(&m->m_fs, buf, 0, sizeof(struct filesys));
				bfree(buf, 2);
			}
		}
	}
}

void sync(void)
{
	register inoptr ino;

	/* Write out modified inodes */

	for (ino = i_tab; ino < i_tab + ITABSIZE; ++ino)
		if (ino->c_refs > 0 && (ino->c_flags & CDIRTY)) {
			wr_inode(ino);
			ino->c_flags &= ~CDIRTY;
		}
	sync_mounts();
	/* WRS: also call d_flush(dev) here for each dirty dev ? */
	bufsync();		/* Clear buffer pool */
#ifdef CONFIG_RIPPLE
	/* Immediate metadata writes may leave no dirty guest buffers. The VM
	 * still has a write cache, so sync must always issue its barrier. */
	{
		unsigned i;
		for (i = 0; i < NMOUNTS; ++i)
			if (fs_tab[i].m_dev != NO_DEVICE && d_flush(fs_tab[i].m_dev))
				udata.u_error = EIO;
	}
#endif
}
