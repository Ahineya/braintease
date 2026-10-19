/* Ripple modifications, 2026-10-19: kernel IO/cache integration. */
#include <kernel.h>
#include <version.h>
#include <kdata.h>
#include <printf.h>
#include <userstructs.h>

void updoff(void)
{
	/* Update current file pointer */
	of_tab[udata.u_files[udata.u_argn]].o_ptr = udata.u_offset;
}


/*******************************************
  seek (file, offset, flag)         Function 9        ?
  int16_t file;
  uint32_t *offset;
  int16_t flag;
 ********************************************/
#define file   (uint16_t)udata.u_argn
#define offset (uint32_t *)udata.u_argn1
#define flag   (int16_t)udata.u_argn2

/* We copy the 32bit offset in and out rather than passing it
   as a 32bit OS might */
arg_t _lseek(void)
{
	static off_t zero;
	inoptr ino;
	struct oft *o;
	off_t p;
	off_t n;
	register off_t *pt;

	if (uget(offset, &n, sizeof(n)))
	        return -1;

	if ((ino = getinode(file)) == NULLINODE)
		return (-1);

	if (getmode(ino) == MODE_R(F_PIPE)) {
		udata.u_error = ESPIPE;
		return (-1);
	}

	o = &of_tab[udata.u_files[file]];
	p = o->o_ptr;

	/* 32bit maths is really messy on some processors and the three cases
	   produce three sets of 32bit maths operations. Instead we take a
	   pointer to what to use as the offset and do the maths once */

	if (flag == 0)
		pt = &zero;
	else if (flag == 1)
		pt = &p;
	else if (flag == 2)
		pt = (off_t *)&ino->c_node.i_size;
	else
                goto bad;

	p = *pt + n;

	if (p < 0)
	        goto bad;

        o->o_ptr = p;
	return uput(&p, offset, sizeof(n));
bad:
	udata.u_error = EINVAL;
	return (-1);

}

#undef file
#undef offset
#undef flag



/*******************************************
  sync()
 ********************************************/

arg_t _sync(void)
{
	sync();
	return 0;
}




/*******************************************
  stat (path, buf)                 Function 15
  char *path;
  char *buf;
 ********************************************/
#define path (uint8_t *)udata.u_argn
#define buf (uint8_t *)udata.u_argn1

arg_t _stat(void)
{
	register inoptr ino;
	register int err;
	if (!(ino = n_open_argn()))
		return (-1);
	err = stcpy(ino, buf);
	i_deref(ino);
	return err;
}

arg_t _lstat(void)
{
    inoptr ino = n_open_flags(path, NULL, N_NOFOLLOW);
    int err;
    if (!ino) return -1;
    err = stcpy(ino, buf);
    i_deref(ino);
    return err;
}

/* readlink deliberately does not append a NUL, and permits truncation. */
arg_t _readlink(void)
{
    uint8_t target[512];
    inoptr ino;
    int len;
    usize_t count = (usize_t)udata.u_argn2;
    if (!count || (susize_t)count < 0) { udata.u_error = EINVAL; return -1; }
    ino = n_open_flags(path, NULL, N_NOFOLLOW);
    if (!ino) return -1;
    len = readlinki(ino, target);
    i_deref(ino);
    if (len < 0) return -1;
    if (count > (usize_t)len) count = len;
    if (uput(target, buf, count)) return -1;
    return count;
}

#undef path
#undef buf


/*******************************************
  fstat (fd, buf)                  Function 16
  int16_t fd;
  char *buf;
 ********************************************/
#define fd (int16_t)udata.u_argn
#define buf (uint8_t *)udata.u_argn1

arg_t _fstat(void)
{
	inoptr ino;

	if ((ino = getinode(fd)) == NULLINODE)
		return (-1);

	return stcpy(ino, buf);
}

#undef fd
#undef buf


/* Utility for stat and fstat */
int stcpy(register inoptr ino, uint8_t *buf)
{
	static struct _uzistat st;

	st.st_dev = ino->c_dev;
	st.st_ino = ino->c_num;
	st.st_rdev = ino->c_node.i_addr[0];
	memcpy(&st.st_mode, &ino->c_node.i_mode, 4 * sizeof(uint16_t));
	memcpy(&st.st_size, &ino->c_node.i_size, 4 * sizeof(uint32_t));
	return uput(&st, buf, sizeof(st));
}

/*******************************************
  dup (oldd)                       Function 17
  int16_t oldd;
 ********************************************/
#define oldd (uint16_t)udata.u_argn

arg_t _dup(void)
{
	int_fast8_t newd;
	if (getinode(oldd) == NULLINODE)
		return (-1);
	return uf_alloc_copy(oldd, 0);
}

#undef oldd


/*******************************************
  dup2 (oldd, newd)                Function 36        ?
  int16_t oldd;
  int16_t newd;
 ********************************************/
#define oldd (int16_t)udata.u_argn
#define newd (int16_t)udata.u_argn1

arg_t _dup2(void)
{
	if (getinode(oldd) == NULLINODE)
		return -1;

	if (newd < 0 || newd >= UFTSIZE) {
		udata.u_error = EBADF;
		return -1;
	}

	/* No-op - but we must not close and dup so catch it */
	if (newd == oldd)
		return oldd;

	if (udata.u_files[newd] != NO_FILE)
		doclose(newd);

	uf_copy(oldd, newd);
	return newd;
}

#undef oldd
#undef newd


/*******************************************
  umask (mask)                     Function 21        ?
  int mask;
 ********************************************/
#define mask (int16_t)udata.u_argn

arg_t _umask(void)
{
	unsigned int omask;

	omask = udata.u_mask;
	udata.u_mask = mask & 0777;
	return (omask);
}

#undef mask



/*******************************************
  ioctl (fd, request, data)        Function 29
  int  fd;
  int  request;
  char *data;
 ********************************************/
#define fd (int)udata.u_argn
#define request (unsigned)udata.u_argn1
#define data (char *)udata.u_argn2

arg_t _ioctl(void)
{
	register inoptr ino;
	uint16_t dev;
	uint_fast8_t rclass = ((uint8_t)(request >> 8)) & 0xC0;
	register struct oft *oftp;

	if ((ino = getinode(fd)) == NULLINODE)
		return -1;

	oftp = of_tab + udata.u_files[fd];

	if (!(isdevice(ino) || issocket(ino)) || rclass == IOCTL_CLASS_KERNEL) {
		udata.u_error = ENOTTY;
		return -1;
	}

	if (rclass == IOCTL_CLASS_SUPER && esuper())
	        return -1;
	if (rclass == IOCTL_CLASS_WRONLY && O_ACCMODE(oftp->o_access) == O_RDONLY) {
		udata.u_error = EPERM;
		return -1;
	}
#ifdef CONFIG_NET
	if (issocket(ino))
		return sock_ioctl(ino, request, data);
#endif
	dev = ino->c_node.i_addr[0];

	return d_ioctl(dev, request, data);
}

#undef fd
#undef request
#undef data



/*******************************************
close (uindex)                    Function 2
int16_t uindex;
********************************************/
#define uindex (int8_t)udata.u_argn

arg_t _close(void)
{
	return (doclose(uindex));
}

#undef uindex


/*******************************************
pipe (fildes)                    Function 40		?
int fildes[];
********************************************/
#define fildes (int *)udata.u_argn

arg_t _pipe(void)
{
	int_fast8_t u1, u2, oft1, oft2;
	register inoptr ino;
	register struct oft *oftp;

    if (!valaddr_w((void *)fildes, 2*sizeof(int), 2*sizeof(int))) return -1;
	if ((u1 = uf_alloc()) == -1)
		goto nogood;
	if ((oft1 = oft_alloc()) == -1)
		goto nogood;
	udata.u_files[u1] = oft1;

	if ((u2 = uf_alloc()) == -1)
		goto nogood2;
	if ((oft2 = oft_alloc()) == -1)
		goto nogood2;

#ifdef CONFIG_RIPPLE_IO
    ino = ripple_pipe_new();
#else
    ino = i_open(root_dev, 0);
#endif
	if (!ino) {
		oft_deref(oft2);
		goto nogood2;
	}

	if (ino->c_flags & CRDONLY) {
		udata.u_error = EROFS;
		goto nogood3;
	}

	udata.u_files[u2] = oft2;

	oftp = of_tab + oft1;
	oftp->o_ptr = 0;
	oftp->o_inode = ino;
	oftp->o_access = O_RDONLY;

	oftp = of_tab + oft2;
	oftp->o_ptr = 0;
	oftp->o_inode = ino;
	oftp->o_access = O_WRONLY;

	++ino->c_refs;
	ino->c_node.i_mode = F_PIPE | 0777;	/* No permissions necessary on pipes */
	ino->c_node.i_nlink = 0;	/* a pipe is not in any directory */
	ino->c_readers++;
	ino->c_writers++;

    /* Validation happened before allocation; still unwind both descriptors if
     * a platform copy reports a late error. */
    if (uputi(u1,fildes) || uputi(u2,fildes+1)) {
        unsigned error=udata.u_error;
        doclose(u1); doclose(u2); udata.u_error=error;
        return -1;
    }
    return 0;

      nogood3:
	i_deref(ino);
      nogood2:
	oft_deref(oft1);
	udata.u_files[u1] = NO_FILE;
      nogood:
	return (-1);
}

#undef fildes


/*******************************************
unlink (path)                     Function 6
char *path;
********************************************/
#define path (uint8_t *)udata.u_argn

arg_t _unlink(void)
{
	register inoptr ino;
	inoptr pino;
	int r;

	ino = n_open_flags(path, &pino, N_NOFOLLOW | N_NODEREF);

	/* Node must exist and have a parent */
	if (!(pino && ino)) {
		if (pino)	/* parent exists */
			i_deref(pino);
		if (udata.u_error == 0)
			udata.u_error = ENOENT;
		return (-1);
	}
	r = unlinki(ino, pino, lastname);
	i_deref(pino);
	i_deref(ino);
	return r;
}

#undef path



/*******************************************
read (d, buf, nbytes)             Function 7
int16_t  d;
char   *buf;
uint16_t nbytes;
********************************************/
#define d (int16_t)udata.u_argn
#define buf (uint8_t *)udata.u_argn1
#define nbytes (susize_t)udata.u_argn2

static arg_t readwrite(uint_fast8_t reading)
{
	inoptr ino;
	uint_fast8_t flag;

	if (!nbytes)
		return 0;

	if ((ssize_t)nbytes < 0) {
		udata.u_error = EINVAL;
	        return -1;
	}

	/* Reading from disk is writing to user space and vice versa... */
	if (!valaddr(buf, nbytes, nbytes, reading))
	        return -1;

	/* Set up u_base, u_offset, ino; check permissions, file num. */
	if ((ino = rwsetup(reading, &flag)) == NULLINODE)
		return -1;	/* bomb out if error */

	(reading ? readi : writei)(ino, flag);
	updoff();

	return udata.u_done;
}

arg_t _read(void)
{
	return readwrite(1);
}

#undef d
#undef buf
#undef nbytes

/*******************************************
getdirent (d, buf, nbytes)       Function 59
int16_t  d;
char   *buf;
uint16_t nbytes;
********************************************/
#define d (int16_t)udata.u_argn
#define buf (uint8_t *)udata.u_argn1
#define nbytes udata.u_argn2

/*
 *	Trivial helper to isolate directory size changes
 *	if we make them.
 */
arg_t _getdirent(void)
{
        if (nbytes < DIR_LEN) {
                udata.u_error = ENOSPC;
                return -1;
        }
#ifdef CONFIG_RIPPLE_IO
        nbytes -= nbytes % DIR_LEN;
#else
        nbytes = DIR_LEN;
#endif
        return _read();
}

#undef d
#undef buf
#undef nbytes


/*******************************************
write (d, buf, nbytes)            Function 8
int16_t  d;
char   *buf;
uint16_t nbytes;
********************************************/
#define d (int16_t)udata.u_argn
#define buf (uint8_t *)udata.u_argn1
#define nbytes (susize_t)udata.u_argn2

arg_t _write(void)
{
	return readwrite(0);
}

#undef d
#undef buf
#undef nbytes

/*******************************************
nosys ()                   Function: various
********************************************/

arg_t _nosys(void)
{
        udata.u_error = ENOSYS;
        return -1;
}
