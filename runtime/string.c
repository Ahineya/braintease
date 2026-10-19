#include <stddef.h>
#include <string.h>
#include <__banked_memory.h>

/* Bank-sized spans avoid pointer normalization and wide length arithmetic
 * per cell in the bulk operations. Other routines retain pointer stepping;
 * strcpy and counted comparisons cache each loaded character. */
char *strcpy(char *dst, char *src) {
    char *d = dst;
    char *s = src;
    unsigned char c;
    while (1) {
        c = (unsigned char)*s;
        *d = c;
        if (c == 0) break;
        d = d + 1;
        s = s + 1;
    }
    return dst;
}

char *strncpy(char *dst, char *src, size_t n) {
    char *d = dst;
    char *s = src;
    while (n != 0 && *s != 0) {
        *d = *s;
        d = d + 1;
        s = s + 1;
        n = n - 1;
    }
    while (n != 0) {
        *d = 0;
        d = d + 1;
        n = n - 1;
    }
    return dst;
}

size_t strlen(char *str) {
    return __rcc_banked_length(str);
}

int strcmp(char *s1, char *s2) {
    return __rcc_banked_compare(s1, s2);
}

int strncmp(char *s1, char *s2, size_t n) {
    unsigned char *a = (unsigned char *)s1;
    unsigned char *b = (unsigned char *)s2;
    unsigned av, bv;
    while (n != 0) {
        av = *a;
        bv = *b;
        if (av != bv) return av < bv ? -1 : 1;
        if (av == 0) return 0;
        a = a + 1;
        b = b + 1;
        n = n - 1;
    }
    return 0;
}

char *strcat(char *dst, char *src) {
    char *end = dst;
    while (*end != 0) end = end + 1;
    strcpy(end, src);
    return dst;
}

char *strncat(char *dst, char *src, size_t n) {
    char *end = dst;
    while (*end != 0) end = end + 1;
    while (n != 0 && *src != 0) {
        *end = *src;
        end = end + 1;
        src = src + 1;
        n = n - 1;
    }
    *end = 0;
    return dst;
}

char *strchr(char *str, int c) {
    while (1) {
        if (*str == (char)c) return str;
        if (*str == 0) return NULL;
        str = str + 1;
    }
}

char *strrchr(char *str, int c) {
    char *last = NULL;
    while (1) {
        if (*str == (char)c) last = str;
        if (*str == 0) return last;
        str = str + 1;
    }
}

void *memcpy(void *dst, void *src, size_t n) {
    return __rcc_banked_copy(dst, src, n);
}

void *memmove(void *dst, void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char *s = (unsigned char *)src;
    uintptr_t dp, sp, db, sb;
    unsigned da, sa, count, i;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) return memcpy(dst, src, n);
    dp = (uintptr_t)(d + n);
    sp = (uintptr_t)(s + n);
    db = dp & 0xffff0000UL;
    sb = sp & 0xffff0000UL;
    da = (unsigned)dp;
    sa = (unsigned)sp;
    while (n != 0) {
        // End pointers may be exactly at the start of the following bank.
        if (da == 0) { da = BANK_SIZE; db -= 65536UL; }
        if (sa == 0) { sa = BANK_SIZE; sb -= 65536UL; }
        count = da;
        if (count > sa) count = sa;
        if (n < count) count = (unsigned)n;
        for (i = 0; i < count; i++) {
            da--;
            sa--;
            *(unsigned char *)(db | da) = *(unsigned char *)(sb | sa);
        }
        n -= count;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    return __rcc_banked_set(dst, c, n);
}

int memcmp(void *s1, void *s2, size_t n) {
    unsigned char *a = (unsigned char *)s1;
    unsigned char *b = (unsigned char *)s2;
    unsigned av, bv;
    while (n != 0) {
        av = *a;
        bv = *b;
        if (av != bv) return av < bv ? -1 : 1;
        a = a + 1;
        b = b + 1;
        n = n - 1;
    }
    return 0;
}
