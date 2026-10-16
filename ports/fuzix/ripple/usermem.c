/* SPDX-License-Identifier: GPL-2.0-only */
#include "platform.h"
#include <kdata.h>
#include <vm_bulk.h>

/* Small syscall arguments cost less to copy directly than to submit a device
 * descriptor. Bulk helpers never run inside the assembly trap-frame shuffle. */
#define BULK_THRESHOLD 256U
extern void ripple_bank_copy_slow(unsigned, unsigned);

/* 0 = copied, 1 = unsupported/small, -1 = device failure. A fallback may copy
 * the whole original range again: all callers use distinct source/dest ranges.
 * In particular a fork may copy the current kernel stack into another bank;
 * the mailbox descriptor lives on the source stack, never the destination. */
static int try_copy(void *dest, const void *source, usize_t count) {
#if RIPPLE_BULK_IO
    uint8_t *d = dest;
    const uint8_t *s = source;
#if RIPPLE_MMU
    if ((((unsigned long)d >> 16) >= USER_GP && ((unsigned long)d >> 16) <= USER_SB) ||
        (((unsigned long)s >> 16) >= USER_GP && ((unsigned long)s >> 16) <= USER_SB)) return 1;
#endif
    if (count < BULK_THRESHOLD) return 1;
    while (count) {
        unsigned n = count > VM_BULK_MAX_CELLS ? VM_BULK_MAX_CELLS : (unsigned)count;
        unsigned long from = (unsigned long)s, to = (unsigned long)d;
        irqflags_t flags = di();
        unsigned status = vm_bulk_copy((unsigned)(from >> 16), (unsigned)from,
            (unsigned)(to >> 16), (unsigned)to, n, VM_BULK_WORDS);
        irqrestore(flags);
        if (status == VM_DEVICE_UNKNOWN_COMMAND || status == VM_DEVICE_UNSUPPORTED) return 1;
        if (status != VM_DEVICE_OK) { udata.u_error = EIO; return -1; }
        s += n; d += n; count -= n;
    }
    return 0;
#else
    return 1;
#endif
}

void ripple_bank_copy(unsigned source, unsigned dest) {
    int result = try_copy((void *)((unsigned long)dest << 16),
                          (void *)((unsigned long)source << 16), BANK_CELLS);
    if (result < 0) panic("task bank transfer");
    if (result) ripple_bank_copy_slow(source, dest);
}

int ripple_copy(void *dest, const void *source, usize_t count) {
    int result = try_copy(dest, source, count);
    if (result < 0) return -1;
    if (result) memcpy(dest, (void *)source, count);
    return 0;
}

int ripple_zero(void *dest, usize_t count) {
#if RIPPLE_BULK_IO
    uint8_t *d = dest;
    usize_t remaining = count;
    if (count >= BULK_THRESHOLD
#if RIPPLE_MMU
        && !(((unsigned long)d >> 16) >= USER_GP && ((unsigned long)d >> 16) <= USER_SB)
#endif
       ) {
        while (remaining) {
            unsigned n = remaining > VM_BULK_MAX_CELLS ? VM_BULK_MAX_CELLS : (unsigned)remaining;
            unsigned long ptr = (unsigned long)d;
            irqflags_t flags = di();
            unsigned status = vm_bulk_fill((unsigned)(ptr >> 16), (unsigned)ptr, n, 0);
            irqrestore(flags);
            if (status == VM_DEVICE_UNKNOWN_COMMAND || status == VM_DEVICE_UNSUPPORTED) break;
            if (status != VM_DEVICE_OK) { udata.u_error = EIO; return -1; }
            d += n; remaining -= n;
        }
        if (!remaining) return 0;
    }
#endif
    memset(dest, 0, count);
    return 0;
}

void *ripple_buffer_copy(void *dest, const void *source, usize_t count) {
    if (ripple_copy(dest, source, count)) panic("kernel buffer transfer");
    return dest;
}
void ripple_buffer_zero(void *dest) {
    if (ripple_zero(dest, BLKSIZE)) panic("kernel buffer clear");
}

usize_t valaddr(const uint8_t *base, usize_t size, usize_t need, uint_fast8_t is_write) {
    unsigned long v = (unsigned long)ripple_userptr(base);
    unsigned bank = (unsigned)(v >> 16), off = (unsigned)v;
#if RIPPLE_MMU
    unsigned saved_error = udata.u_error;
    udata.u_error = 0;
    size = ripple_mmu_validate(v,size,is_write);
    used(bank); used(off);
#else
    used(is_write);
    if ((bank != USER_GP && bank != USER_SB) || off >= BANK_CELLS)
        size = 0;
    else if (size > BANK_CELLS - off)
        size = BANK_CELLS - off;
    #endif
    if (size < need) {
#if RIPPLE_MMU
        if (!udata.u_error)
#endif
            udata.u_error = EFAULT;
        return 0;
    }
#if RIPPLE_MMU
    udata.u_error = saved_error;
#endif
    return size;
}
usize_t valaddr_r(const uint8_t *b, usize_t s, usize_t n) { return valaddr(b, s, n, 0); }
usize_t valaddr_w(const uint8_t *b, usize_t s, usize_t n) { return valaddr(b, s, n, 1); }
int _uget(const uint8_t *a, uint8_t *b, usize_t n) {
    return ripple_copy(b, ripple_userptr(a), n);
}
int _uput(const uint8_t *a, uint8_t *b, usize_t n) {
    return ripple_copy(ripple_userptr(b), a, n);
}
int _uzero(uint8_t *a, usize_t n) {
    return ripple_zero(ripple_userptr(a), n);
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
