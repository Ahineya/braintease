#include <kernel.h>
#include <version.h>
#include <kdata.h>
#include <printf.h>

#ifdef CONFIG_LEVEL_2

/* Select maps live in the in-core inode. The on-disk address array is
 * also used for pipe data and is too small for larger process tables. */

uint16_t pipesel = 0;

void seladdwait(struct selmap *s)
{
	uint16_t p = udata.u_ptab - ptab;
	s->map[p >> 3] |= (1 << (p & 7));
}

void selrmwait(struct selmap *s)
{
	uint16_t p = udata.u_ptab - ptab;
	s->map[p >> 3] &= ~(1 << (p & 7));
}

void selwake(struct selmap *s)
{
	ptptr p = ptab;
	uint16_t i;

	for (i = 0; i < maxproc; i++) {
		if (s->map[i >> 3] & (1 << (i & 7)))
			pwake(p);
		p++;
	}
}

/* Set our select bits on the inode */
void selwait_inode(inoptr i, uint_fast8_t smask, uint_fast8_t setit)
{
    struct selmap *s = i->c_select;
    uint16_t bit = udata.u_ptab - ptab;
    uint8_t mask = 1 << (bit & 7);
    uint16_t byte = bit >> 3;
    uint_fast8_t n;

    for (n = 0; n < 3; n++) {
        if (smask & (1 << n)) {
            if (setit)
                s[n].map[byte] |= mask;
            else
                s[n].map[byte] &= ~mask;
        }
    }
}

/* Wake an inode for select */
void selwake_inode(inoptr i, uint16_t mask)
{
	struct selmap *s = i->c_select;
	irqflags_t irq = di();
	if (mask & SELECT_IN)
		selwake(s);
	s++;
	if (mask & SELECT_OUT)
		selwake(s);
	s++;
	if (mask & SELECT_EX)
		selwake(s);
	irqrestore(irq);
}

void selwake_dev(uint_fast8_t major, uint_fast8_t minor, uint16_t mask)
{
	irqflags_t irq = di();
	uint16_t v = (major << 8) | minor;
	inoptr i = i_tab;

	while (i <= &i_tab[ITABSIZE - 1]) {	/* Convoluted form to keep SDCC happy */
		if (i->c_refs && i->c_node.i_addr[0] == v)
			selwake_inode(i, mask);
		i++;
	}
	irqrestore(irq);
}

static int pipesel_test(inoptr i, uint_fast8_t bits)
{
    uint16_t mask = 0;
    /* Data or EOF; inode references also include the reader itself. */
    if (i->c_node.i_size || !i->c_writers)
        mask |= SELECT_IN;
    if (i->c_node.i_size < 8 * BLKSIZE)
        mask |= SELECT_OUT;
    return mask & bits;
}

static int pipesel_begin(inoptr i, uint_fast8_t bits)
{
    pipesel++;
    selwait_inode(i, bits, 1);
    return pipesel_test(i, bits);
}

static void pipesel_end(inoptr i)
{
	pipesel--;
	/* Clear out wait masks */
	selwait_inode(i, SELECT_IN | SELECT_OUT | SELECT_EX, 0);
}

void selwake_pipe(inoptr i, uint16_t mask)
{
	if (pipesel)
		selwake_inode(i, mask);
}


/*******************************************
  _select (nfd, base)            Function 72
  int nfd;
  uint16_t *base;
 *******************************************/
#define nfd (uint16_t)udata.u_argn
#define base (uint16_t *)udata.u_argn1

arg_t _select(void)
{
    irqflags_t irq;
    uint16_t seltype = SELECT_BEGIN;
    inoptr ino;
    uint16_t m, registered = 0;
    uint_fast8_t i;
    char n, want;
    uint16_t inr = 0, outr = 0, exr = 0;
    uint16_t in = ugetw(base);
    uint16_t out = ugetw(base + 2);
    uint16_t ex = ugetw(base + 4);
    uint16_t sum = in | out | ex;
    int error;

    if (nfd > UFTSIZE || nfd > 16) {
        udata.u_error = EINVAL;
        return -1;
    }
    if (!nfd)
        sum = 0;
    else
        sum &= (uint16_t)(0xFFFFU >> (16 - nfd));

    /* Validate before disabling interrupts or starting registrations. */
    for (i = 0, m = 1; i < nfd; i++, m <<= 1) {
        if ((sum & m) && getinode(i) == NULLINODE)
            return -1;
    }

    udata.u_ptab->p_timeout = ugetw(base + 6);
    if (udata.u_ptab->p_timeout)
        ptimer_insert();

    irq = di();
    do {
        for (i = 0, m = 1; i < nfd; i++, m <<= 1) {
            if (!(sum & m))
                continue;
            want = 0;
            if (in & m)
                want |= SELECT_IN;
            if (out & m)
                want |= SELECT_OUT;
            if (ex & m)
                want |= SELECT_EX;
            ino = getinode(i);
            if (ino == NULLINODE)
                goto done;
            n = want;
            switch (getmode(ino)) {
            case MODE_R(F_BDEV):
            case MODE_R(F_REG):
                outr |= m;
                inr |= m;
                break;
            case MODE_R(F_DIR):
                inr |= m;
                break;
            case MODE_R(F_PIPE):
                if (seltype == SELECT_BEGIN) {
                    n = pipesel_begin(ino, want);
                    registered |= m;
                } else
                    n = pipesel_test(ino, want);
                goto setbits;
            case MODE_R(F_CDEV):
                if (d_ioctl(ino->c_node.i_addr[0], seltype, &n) == -1) {
                    udata.u_error = 0;
                    n = SELECT_IN | SELECT_OUT;
                } else if (seltype == SELECT_BEGIN) {
                    selwait_inode(ino, want, 1);
                    registered |= m;
                }
            setbits:
                if (n & SELECT_IN)
                    inr |= m;
                if (n & SELECT_OUT)
                    outr |= m;
                if (n & SELECT_EX)
                    exr |= m;
                break;
            }
        }
        inr &= in;
        outr &= out;
        exr &= ex;
        if (inr || outr || exr || udata.u_ptab->p_timeout <= 1)
            break;
        if (psleep_flags(&udata.u_ptab->p_timeout, 0))
            break;
        irqrestore(irq);
        irq = di();
        seltype = SELECT_TEST;
    } while (1);

done:
    irqrestore(irq);
    udata.u_ptab->p_timeout = 0;
    error = udata.u_error;
    /* Balance each registration once, including after an interrupted wait. */
    for (i = 0, m = 1; i < nfd; i++, m <<= 1) {
        if (!(registered & m))
            continue;
        ino = getinode(i);
        if (ino == NULLINODE)
            continue;
        if (getmode(ino) == MODE_R(F_CDEV)) {
            d_ioctl(ino->c_node.i_addr[0], SELECT_END, NULL);
            selwait_inode(ino, SELECT_IN | SELECT_OUT | SELECT_EX, 0);
        } else if (getmode(ino) == MODE_R(F_PIPE))
            pipesel_end(ino);
    }
    udata.u_error = error;
    if (error)
        return -1;
    if (uputw(inr, base) || uputw(outr, base + 2) || uputw(exr, base + 4))
        return -1;
    return 0; /* The userspace wrapper counts the ready descriptors. */
}

#undef nfd
#undef base

#endif
