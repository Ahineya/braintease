/*
 *	Implement the usermem services. Can then use either C or asm code for
 *	the block transfers. This code can be banked as part of the kernel if
 *	using the asm helpers but must be in commonmem if not
 */

#include <kernel.h>
#include <kdata.h>
#include <printf.h>

#if !defined(CONFIG_LEVEL_0)

/* Flat mode has to use its own valaddr: tidy this */
#if !defined(CONFIG_FLAT) && !defined(CONFIG_VMMU) && !defined(CONFIG_CUSTOM_VALADDR) && !defined(CONFIG_FLAT_SMALL)

/* This checks to see if a user-supplied address is legitimate */
usize_t valaddr(const uint8_t *base, usize_t size, usize_t need, uint_fast8_t is_write)
{
	/* Cast to deal with gcc6809 fun */
	if ((usize_t)base + size < (usize_t)base)
		size = MAXUSIZE - (usize_t)base + 1;
	if (!base || base < (const uint8_t *)PROGBASE ||
		base > (const uint8_t *)(size_t)udata.u_top)
		size = 0;
	else if (base + size > (const uint8_t *)(size_t)udata.u_top)
		size = (uint8_t *)(size_t)udata.u_top - base;
	if (size < need) {
		udata.u_error = EFAULT;
		return 0;
	}
	return size;
}

usize_t valaddr_r(const uint8_t *base, usize_t size, usize_t need)
{
	return valaddr(base, size, need, 0);
}

usize_t valaddr_w(const uint8_t *base, usize_t size, usize_t need)
{
	return valaddr(base, size, need, 1);
}

#endif

int uget(const void *user, void *dst, usize_t count)
{
	if (!valaddr_r(user,count, count))
		return -1;
	return _uget(user,dst,count);
}

int16_t ugetc(const void *user)
{
	if (!valaddr_r(user, 1, 1))
		return -1;
	return _ugetc(user);
}

uint16_t ugetw(const void *user)
{
	if (!valaddr_r(user, sizeof(uint16_t), sizeof(uint16_t)))
		return -1;
#ifdef MISALIGNED
	if (MISALIGNED(user, 2)) {
		ssig(udata.u_ptab, SIGBUS);
		return -1;
	}
#endif
	return _ugetw(user);
}

int uput(const void *source,   void *user, usize_t count)
{
	if (!valaddr_w(user, count, count))
		return -1;
	return _uput(source,user,count);
}

int uputc(uint16_t value,  void *user)
{
	if (!valaddr_w(user, 1, 1))
		return -1;
	/* u16_t so we don't get wacky 8bit stack games on SDCC */
	return _uputc(value,user);
}

int uputw(uint16_t value, void *user)
{
	if (!valaddr_w(user, sizeof(uint16_t), sizeof(uint16_t)))
		return -1;
#ifdef MISALIGNED
	if (MISALIGNED(user, 2)) {
		ssig(udata.u_ptab, SIGBUS);
		return -1;
	}
#endif
	return _uputw(value,user);
}

int uzero(void *user, usize_t count)
{
	if (!valaddr_w(user, count, count))
		return -1;
	return _uzero(user,count);
}

#ifdef CONFIG_32BIT

uint32_t ugetl(void *uaddr)
{
	if (!valaddr_r(uaddr, sizeof(uint32_t), sizeof(uint32_t)))
		return -1;
#ifdef MISALIGNED
	if (MISALIGNED(uaddr, 4)) {
		ssig(udata.u_ptab, SIGBUS);
		return -1;
	}
#endif
	return _ugetl(uaddr);
}

int uputl(uint32_t val, void *uaddr)
{
	if (!valaddr_w(uaddr, sizeof(uint32_t), sizeof(uint32_t)))
		return -1;
#ifdef MISALIGNED
	if (MISALIGNED(uaddr, 4)) {
		ssig(udata.u_ptab, SIGBUS);
		return -1;
	}
#endif
	return _uputl(val, uaddr);
}

#endif

/*
 *	Optional C language implementation for porting to new processors
 *	or where asm isn't needed
 */
#ifdef CONFIG_USERMEM_C

int _uget(register const uint8_t *user, register uint8_t *dest, register usize_t count)
{
	uint8_t tmp;
	while(count--) {
		BANK_PROCESS;
		tmp = *user++;
		BANK_KERNEL;
		*dest++ = tmp;
	}
	return 0;
}

int16_t _ugetc(const uint8_t *user)
{
	uint8_t tmp;
	BANK_PROCESS;
	tmp = *user;
	BANK_KERNEL;
	return tmp;
}

uint16_t _ugetw(const uint16_t *user)
{
	uint16_t tmp;
	BANK_PROCESS;
	tmp = *user;
	BANK_KERNEL;
	return tmp;
}

int _uput(register const uint8_t *source, register uint8_t *user, register usize_t count)
{
	uint8_t tmp;
	while(count--) {
		tmp = *source++;
		BANK_PROCESS;
		*user++ = tmp;
		BANK_KERNEL;
	}
	return 0;

}

int _uputc(uint16_t value,  uint8_t *user)
{
	BANK_PROCESS;
	*user = value;
	BANK_KERNEL;
	return 0;
}

int _uputw(uint16_t value,  uint16_t *user)
{
	BANK_PROCESS;
	*user = value;
	BANK_KERNEL;
	return 0;
}

int _uzero(uint8_t *user, usize_t count)
{
	BANK_PROCESS;
	while(count--)
		*user++=0;
	BANK_KERNEL;
	return 0;
}

#ifdef CONFIG_32BIT

uint32_t _ugetl(const uint32_t *uaddr)
{
	uint32_t v;
	BANK_PROCESS;
	v = *(uint32_t *)uaddr;
	BANK_KERNEL;
	return v;
}

int _uputl(uint32_t val, uint32_t *uaddr)
{
	BANK_PROCESS;
	*(uint32_t *)uaddr = val;
	BANK_KERNEL;
	return 0;
}

#endif
#endif

#endif
