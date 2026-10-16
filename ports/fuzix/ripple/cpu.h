/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_FUZIX_CPU_H
#define RIPPLE_FUZIX_CPU_H
#define uputp uputl
#define ugetp(x) ripple_ugetp(x)
extern uint32_t ripple_ugetp(void *);
#define uputi uputw
#define ugeti(x) ugetw(x)
typedef uint32_t clock_t;
typedef union {
    clock_t full;
    struct {
        uint16_t low, high;
    } h;
} ticks_t;
typedef uint32_t paddr_t;
typedef uint16_t page_t;
#define CPU_MID 0x5250
#define staticfast
#define used(x) ((void)(x))
#define __packed
#define __fastcall
#define NORETURN
#define barrier() ((void)0)
#define brk_limit() (PROGTOP - 1024)
#define STACKALIGN(x) (x)
#define ntohs(x) ripple_swab16(x)
#define ntohl(x) ripple_swab32(x)
#define cpu_to_le16(x) (x)
#define le16_to_cpu(x) (x)
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define POINTER32
extern uint16_t ripple_swab16(uint16_t);
extern uint32_t ripple_swab32(uint32_t);
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern int memcmp(const void *, const void *, size_t);
extern size_t strlen(const char *);
extern int strcmp(const char *, const char *);
/* Cache-buffer operations cannot report a recoverable copy failure. */
extern void *ripple_buffer_copy(void *, const void *, usize_t);
extern void ripple_buffer_zero(void *);
#endif
