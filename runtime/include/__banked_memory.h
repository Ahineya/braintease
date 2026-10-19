#ifndef RCC_BANKED_MEMORY_H
#define RCC_BANKED_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/* Internal C16 routines shared by the standalone and guest libraries. Pointers
 * are (bank << 16) | offset, not linear integers. Keep the bank bits invariant
 * inside each span and use native-word offsets/counts there. Only span changes
 * update the bank and the 32-bit remaining length. Ordinary C dereferences still
 * go through each backend's checked memory access (including MMIO and COW).
 * A C byte is a full 16-bit cell: never mask these transfers to eight bits.
 */
#ifndef BANK_SIZE
#define BANK_SIZE 64000
#endif
#if BANK_SIZE < 1 || BANK_SIZE > 65535
#error Invalid C16 bank size
#endif

/* Decimal bank sizes above INT_MAX would otherwise widen loop comparisons. */
#define __RCC_BANK_CELLS ((unsigned)BANK_SIZE)

static void *__rcc_banked_copy(void *dst, const void *src, size_t n) {
    uintptr_t d = (uintptr_t)dst, s = (uintptr_t)src;
    uintptr_t db = d & 0xffff0000UL, sb = s & 0xffff0000UL;
    unsigned da = (unsigned)d, sa = (unsigned)s, count, i;
    while (n != 0) {
        count = __RCC_BANK_CELLS - da;
        if (count > __RCC_BANK_CELLS - sa) count = __RCC_BANK_CELLS - sa;
        if (n < count) count = (unsigned)n;
        for (i = 0; i < count; i++) {
            *(unsigned char *)(db | da) = *(const unsigned char *)(sb | sa);
            da++;
            sa++;
        }
        n -= count;
        if (da == __RCC_BANK_CELLS) { da = 0; db += 65536UL; }
        if (sa == __RCC_BANK_CELLS) { sa = 0; sb += 65536UL; }
    }
    return dst;
}

static void *__rcc_banked_set(void *dst, int c, size_t n) {
    uintptr_t d = (uintptr_t)dst, db = d & 0xffff0000UL;
    unsigned da = (unsigned)d, count, i;
    while (n != 0) {
        count = __RCC_BANK_CELLS - da;
        if (n < count) count = (unsigned)n;
        for (i = 0; i < count; i++) {
            *(unsigned char *)(db | da) = (unsigned char)c;
            da++;
        }
        n -= count;
        da = 0;
        db += 65536UL;
    }
    return dst;
}

static size_t __rcc_banked_length(const char *str) {
    uintptr_t s = (uintptr_t)str, sb = s & 0xffff0000UL;
    unsigned sa = (unsigned)s, i;
    size_t length = 0;
    while (1) {
        for (i = sa; i < __RCC_BANK_CELLS; i++) {
            if (*(const unsigned char *)(sb | i) == 0)
                return length + (unsigned)(i - sa);
        }
        length += (unsigned)(__RCC_BANK_CELLS - sa);
        sa = 0;
        sb += 65536UL;
    }
}

static int __rcc_banked_compare(const char *a, const char *b) {
    uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
    uintptr_t ab = ap & 0xffff0000UL, bb = bp & 0xffff0000UL;
    unsigned aa = (unsigned)ap, ba = (unsigned)bp, count, i, av, bv;
    while (1) {
        count = __RCC_BANK_CELLS - aa;
        if (count > __RCC_BANK_CELLS - ba) count = __RCC_BANK_CELLS - ba;
        for (i = 0; i < count; i++) {
            av = *(const unsigned char *)(ab | aa);
            bv = *(const unsigned char *)(bb | ba);
            if (av != bv) return av < bv ? -1 : 1;
            if (av == 0) return 0;
            aa++;
            ba++;
        }
        if (aa == __RCC_BANK_CELLS) { aa = 0; ab += 65536UL; }
        if (ba == __RCC_BANK_CELLS) { ba = 0; bb += 65536UL; }
    }
}
#endif
