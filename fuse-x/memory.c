/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"
#include <kdata.h>
#include <string.h>

/* Native globals use bank 1, continuation frames use 2..4. The heap window
 * starts at 8 and argv/env live in 31. Kernel globals at 240 are never user-copy
 * destinations. This software contract is not a host privilege boundary. */
usize_t valaddr(const uint8_t *base, usize_t size, usize_t need, uint_fast8_t write) {
    unsigned long ptr = (unsigned long)base;
    unsigned bank = ptr >> 16, off = (unsigned)ptr;
    unsigned long end;
    used(write);
    if (off >= 64000 || !ptr) size = 0;
    else {
        if (bank >= 1 && bank <= 4) end = 5UL * 64000;
        else if (bank >= 8 && bank <= 23) end = 24UL * 64000;
        else if (bank == 31) end = 32UL * 64000;
        else { end = 0; size = 0; }
        ptr = bank * 64000UL + off;
        if (size && size > end - ptr) size = end - ptr;
    }
    if (size < need) { udata.u_error = EFAULT; return 0; }
    return size;
}
usize_t valaddr_r(const uint8_t *p, usize_t size, usize_t need) { return valaddr(p,size,need,0); }
usize_t valaddr_w(const uint8_t *p, usize_t size, usize_t need) { return valaddr(p,size,need,1); }
int _uget(const uint8_t *a, uint8_t *b, usize_t n) { memcpy(b,a,n); return 0; }
int _uput(const uint8_t *a, uint8_t *b, usize_t n) { memcpy(b,a,n); return 0; }
int _uzero(uint8_t *a, usize_t n) { memset(a,0,n); return 0; }
int16_t _ugetc(const uint8_t *p) { return *p; }
uint16_t _ugetw(const uint16_t *p) { return *p; }
uint32_t _ugetl(const uint32_t *p) { return *p; }
int _uputc(uint16_t v, uint8_t *p) { *p=v; return 0; }
int _uputw(uint16_t v, uint16_t *p) { *p=v; return 0; }
int _uputl(uint32_t v, uint32_t *p) { *p=v; return 0; }
uint32_t ripple_ugetp(void *p) { return ugetl(p); }
void *ripple_buffer_copy(void *to, const void *from, usize_t n) { return memcpy(to,from,n); }
void ripple_buffer_zero(void *p) { memset(p,0,BLKSIZE); }
static unsigned long dense(unsigned long ptr) { return (ptr >> 16) * 64000UL + (unsigned)ptr; }
arg_t brk_extend(uaddr_t ptr) {
    return (ptr >> 16) < 8 || (ptr >> 16) > 23 || (unsigned)ptr >= 64000 || ptr < PROGBASE ? ENOMEM : 0;
}
arg_t _brk(void) {
    unsigned long next = udata.u_argn, old = udata.u_break;
    if ((udata.u_error=brk_extend(next))) return -1;
    if (next > old) memset((void *)old,0,dense(next)-dense(old));
    udata.u_break=next;
    return 0;
}
arg_t _sbrk(void) {
    unsigned long old=udata.u_break, at=dense(old), next;
    long change=udata.u_argn;
    next=at+change;
    if ((change>0 && next<at) || (change<0 && next>at)) { udata.u_error=ENOMEM; return -1; }
    udata.u_argn=((next/64000UL)<<16)|(next%64000UL);
    if (_brk()) return -1;
    return old;
}
