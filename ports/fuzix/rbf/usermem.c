/* SPDX-License-Identifier: GPL-2.0-only */
#include <kernel.h>
#include <kdata.h>
extern void *rbf_userptr(const void *);
usize_t valaddr(const uint8_t *p, usize_t size, usize_t need, uint_fast8_t write) {
    unsigned long address = (unsigned long)rbf_userptr(p);
    unsigned bank = address >> 16, off = (unsigned)address;
    used(write);
    /* Each allocation occupies one physical bank; never carry a user request
     * through the unused tail of a C16 architectural address. */
    if ((bank != 3 && bank != 4) || off >= BANK_SIZE) size = 0;
    else if (size > BANK_SIZE - off) size = BANK_SIZE - off;
    if (size < need) { udata.u_error = EFAULT; return 0; }
    return size;
}
usize_t valaddr_r(const uint8_t *p, usize_t n, usize_t need) { return valaddr(p,n,need,0); }
usize_t valaddr_w(const uint8_t *p, usize_t n, usize_t need) { return valaddr(p,n,need,1); }
int _uget(const uint8_t *p, uint8_t *d, usize_t n) { ripple_buffer_copy(d,rbf_userptr(p),n); return 0; }
int _uput(const uint8_t *s, uint8_t *p, usize_t n) { ripple_buffer_copy(rbf_userptr(p),s,n); return 0; }
int _uzero(uint8_t *p, usize_t n) { memset(rbf_userptr(p),0,n); return 0; }
int16_t _ugetc(const uint8_t *p) { return *(uint8_t *)rbf_userptr(p); }
uint16_t _ugetw(const uint16_t *p) { return *(uint16_t *)rbf_userptr(p); }
uint32_t _ugetl(const uint32_t *p) { return *(uint32_t *)rbf_userptr(p); }
int _uputc(uint16_t v,uint8_t *p) { *(uint8_t *)rbf_userptr(p)=v; return 0; }
int _uputw(uint16_t v,uint16_t *p) { *(uint16_t *)rbf_userptr(p)=v; return 0; }
int _uputl(uint32_t v,uint32_t *p) { *(uint32_t *)rbf_userptr(p)=v; return 0; }
uint32_t ripple_ugetp(void *p) { uint32_t value=ugetl(p); return value?(uint32_t)rbf_userptr((void *)value):0; }
