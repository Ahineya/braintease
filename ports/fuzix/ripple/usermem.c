/* SPDX-License-Identifier: GPL-2.0-only */
#include "platform.h"
#include <kdata.h>
usize_t valaddr(const uint8_t *base, usize_t size, usize_t need, uint_fast8_t is_write) {
    unsigned long v = (unsigned long)ripple_userptr(base);
    unsigned bank = (unsigned)(v >> 16), off = (unsigned)v;
    used(is_write);
    if ((bank != USER_GP && bank != USER_SB) || off >= BANK_CELLS)
        size = 0;
    else if (size > BANK_CELLS - off)
        size = BANK_CELLS - off;
    if (size < need) {
        udata.u_error = EFAULT;
        return 0;
    }
    return size;
}
usize_t valaddr_r(const uint8_t *b, usize_t s, usize_t n) { return valaddr(b, s, n, 0); }
usize_t valaddr_w(const uint8_t *b, usize_t s, usize_t n) { return valaddr(b, s, n, 1); }
int _uget(const uint8_t *a, uint8_t *b, usize_t n) {
    memcpy(b, ripple_userptr(a), n);
    return 0;
}
int _uput(const uint8_t *a, uint8_t *b, usize_t n) {
    memcpy(ripple_userptr(b), a, n);
    return 0;
}
int _uzero(uint8_t *a, usize_t n) {
    memset(ripple_userptr(a), 0, n);
    return 0;
}
int16_t _ugetc(const uint8_t *a) { return *(uint8_t *)ripple_userptr(a); }
uint16_t _ugetw(const uint16_t *a) { return *(uint16_t *)ripple_userptr(a); }
uint32_t _ugetl(const uint32_t *a) { return *(uint32_t *)ripple_userptr(a); }
int _uputc(uint16_t v, uint8_t *a) {
    *(uint8_t *)ripple_userptr(a) = v;
    return 0;
}
int _uputw(uint16_t v, uint16_t *a) {
    *(uint16_t *)ripple_userptr(a) = v;
    return 0;
}
int _uputl(uint32_t v, uint32_t *a) {
    *(uint32_t *)ripple_userptr(a) = v;
    return 0;
}
uint32_t ripple_ugetp(void *a) { return (uint32_t)ripple_userptr((void *)ugetl(a)); }
