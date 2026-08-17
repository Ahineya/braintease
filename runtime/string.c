#include <stddef.h>
#include <string.h>

char *strcpy(char *dst, char *src) {
    size_t i;
    for (i = 0; ; i++) {
        dst[i] = src[i];
        if (!src[i]) break;
    }
    return dst;
}

char *strncpy(char *dst, char *src, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (src[i]) {
            dst[i] = src[i];
        } else {
            for (; i < n; i++) {
                dst[i] = 0;
            }
            break;
        }
    }
    return dst;
}

size_t strlen(char *str) {
    size_t len;
    for (len = 0; str[len]; len++) {
    }
    return len;
}

int strcmp(char *s1, char *s2) {
    size_t i;
    for (i = 0; ; i++) {
        if (!s1[i] || !s2[i]) break;
        if (s1[i] != s2[i]) {
            return s1[i] - s2[i];
        }
    }
    return s1[i] - s2[i];
}

int strncmp(char *s1, char *s2, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (!s1[i] || !s2[i]) {
            return s1[i] - s2[i];
        }
        if (s1[i] != s2[i]) {
            return s1[i] - s2[i];
        }
    }
    return 0;
}

char *strcat(char *dst, char *src) {
    size_t dst_len = strlen(dst);
    size_t i;
    for (i = 0; ; i++) {
        dst[dst_len + i] = src[i];
        if (!src[i]) break;
    }
    return dst;
}

char *strncat(char *dst, char *src, size_t n) {
    size_t dst_len = strlen(dst);
    size_t i;
    for (i = 0; i < n; i++) {
        if (!src[i]) break;
        dst[dst_len + i] = src[i];
    }
    dst[dst_len + i] = 0;
    return dst;
}

char *strchr(char *str, int c) {
    size_t i;
    for (i = 0; str[i]; i++) {
        if (str[i] == c) {
            return (char *)&str[i];
        }
    }
    if (c == 0) {
        return (char *)&str[i];
    }
    return 0;
}

char *strrchr(char *str, int c) {
    char *last = 0;
    size_t i;
    for (i = 0; str[i]; i++) {
        if (str[i] == c) {
            last = (char *)&str[i];
        }
    }
    if (c == 0) {
        return (char *)&str[i];
    }
    return last;
}

void *memcpy(void *dst, void *src, size_t n) {
    char *d = (char *)dst;
    char *s = (char *)src;
    size_t i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memmove(void *dst, void *src, size_t n) {
    char *d = (char *)dst;
    char *s = (char *)src;
    size_t i;

    if (d < s || d >= s + n) {
        for (i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        i = n;
        while (i) {
            i = i - 1;
            d[i] = s[i];
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    char *d = (char *)dst;
    size_t i;
    for (i = 0; i < n; i++) {
        d[i] = (char)c;
    }
    return dst;
}

int memcmp(void *s1, void *s2, size_t n) {
    char *p1 = (char *)s1;
    char *p2 = (char *)s2;
    size_t i;
    for (i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}
