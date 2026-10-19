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
static int user_span(const void *p) {
    unsigned bank=(unsigned)((unsigned long)p>>16);
    return bank>=USER_GP&&bank<=USER_SB;
}
static int checked_span(const void *p,usize_t count,unsigned write) {
    if(user_span(p)&&ripple_mmu_validate((unsigned long)p,count,write)!=count) {
        if(!udata.u_error)udata.u_error=EFAULT;
        return -1;
    }
    return 0;
}
static int try_copy(void *dest, const void *source, usize_t count) {
#if RIPPLE_BULK_IO
    uint8_t *d=dest;const uint8_t *s=source;
    if(count<BULK_THRESHOLD)return 1;
    /* Resolve COW before handing physical destinations to a privileged device.
     * Validate the complete range before the first transfer. */
    if(checked_span(d,count,1)||checked_span(s,count,0))return -1;
    while(count) {
        unsigned n=count>VM_BULK_MAX_CELLS?VM_BULK_MAX_CELLS:(unsigned)count;
        unsigned long from=(unsigned long)s,to=(unsigned long)d;
        unsigned sb=(unsigned)(from>>16),so=(unsigned)from,db=(unsigned)(to>>16),doff=(unsigned)to;
        unsigned status;irqflags_t flags;
        unsigned run;
        if(user_span(s)){if(ripple_mmu_span(from,0,&sb,&so,&run))return -1;if(n>run)n=run;}
        if(user_span(d)){if(ripple_mmu_span(to,1,&db,&doff,&run))return -1;if(n>run)n=run;}
        flags=di();status=vm_bulk_copy(sb,so,db,doff,n,VM_BULK_WORDS);irqrestore(flags);
        if(status==VM_DEVICE_UNKNOWN_COMMAND||status==VM_DEVICE_UNSUPPORTED)return 1;
        if(status!=VM_DEVICE_OK){udata.u_error=EIO;return -1;}
        s+=n;d+=n;count-=n;
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

int ripple_fill(void *dest,unsigned value,usize_t count) {
#if RIPPLE_BULK_IO
    uint8_t *d=dest;usize_t remaining=count;
    if(count>=BULK_THRESHOLD) {
        if(checked_span(dest,count,1))return -1;
        while(remaining) {
            unsigned n=remaining>VM_BULK_MAX_CELLS?VM_BULK_MAX_CELLS:(unsigned)remaining;
            unsigned long ptr=(unsigned long)d;
            unsigned bank=(unsigned)(ptr>>16),off=(unsigned)ptr,status;
            irqflags_t flags;
            unsigned run;
            if(user_span(d)){if(ripple_mmu_span(ptr,1,&bank,&off,&run))return -1;if(n>run)n=run;}
            flags=di();status=vm_bulk_fill(bank,off,n,value);irqrestore(flags);
            if(status==VM_DEVICE_UNKNOWN_COMMAND||status==VM_DEVICE_UNSUPPORTED)break;
            if(status!=VM_DEVICE_OK){udata.u_error=EIO;return -1;}
            d+=n;remaining-=n;
        }
        if(!remaining)return 0;
    }
#endif
    memset(dest,value,count);return 0;
}

int ripple_zero(void *dest,usize_t count){return ripple_fill(dest,0,count);}

void *ripple_buffer_copy(void *dest, const void *source, usize_t count) {
    if (ripple_copy(dest, source, count)) panic("kernel buffer transfer");
    return dest;
}
void ripple_buffer_zero(void *dest) {
    if (ripple_zero(dest, BLKSIZE)) panic("kernel buffer clear");
}

usize_t valaddr(const uint8_t *base, usize_t size, usize_t need, uint_fast8_t is_write) {
    unsigned long v = (unsigned long)ripple_userptr(base);
    unsigned saved_error = udata.u_error;
    udata.u_error = 0;
    size = ripple_mmu_validate(v,size,is_write);
    if (size < need) {
        if (!udata.u_error)
            udata.u_error = EFAULT;
        return 0;
    }
    udata.u_error = saved_error;
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
