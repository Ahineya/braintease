#include <stddef.h>
#include <string.h>

/* Pointer stepping keeps each iteration in native 16-bit arithmetic. The
 * compiler normalizes a bank crossing; n/len remain 32-bit C-byte counts. */
char *strcpy(char *dst, char *src) {
    char *d = dst;
    char *s = src;
    while (1) {
        *d = *s;
        if (*s == 0) break;
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
    size_t len = 0;
    while (*str != 0) {
        len = len + 1;
        str = str + 1;
    }
    return len;
}

int strcmp(char *s1, char *s2) {
    unsigned char *a = (unsigned char *)s1;
    unsigned char *b = (unsigned char *)s2;
    while (*a == *b) {
        if (*a == 0) return 0;
        a = a + 1;
        b = b + 1;
    }
    return *a < *b ? -1 : 1;
}

int strncmp(char *s1, char *s2, size_t n) {
    unsigned char *a = (unsigned char *)s1;
    unsigned char *b = (unsigned char *)s2;
    while (n != 0) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (*a == 0) return 0;
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
    unsigned char *d = (unsigned char *)dst;
    unsigned char *s = (unsigned char *)src;
    while (n != 0) {
        *d = *s;
        d = d + 1;
        s = s + 1;
        n = n - 1;
    }
    return dst;
}

void *memmove(void *dst, void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char *s = (unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) {
        return memcpy(dst, src, n);
    }
    d = d + n;
    s = s + n;
    while (n != 0) {
        d = d - 1;
        s = s - 1;
        *d = *s;
        n = n - 1;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    while (n != 0) {
        *d = (unsigned char)c;
        d = d + 1;
        n = n - 1;
    }
    return dst;
}

int memcmp(void *s1, void *s2, size_t n) {
    unsigned char *a = (unsigned char *)s1;
    unsigned char *b = (unsigned char *)s2;
    while (n != 0) {
        if (*a != *b) return *a < *b ? -1 : 1;
        a = a + 1;
        b = b + 1;
        n = n - 1;
    }
    return 0;
}
