/* C16 string operations compare whole cells, including values above 255. */
typedef unsigned long size_t;
typedef unsigned short cell;

size_t strlen(const cell *s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}
int strcmp(const cell *a, const cell *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a < *b ? -1 : *a > *b ? 1 : 0;
}
int strncmp(const cell *a, const cell *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        if (!a[i]) break;
    }
    return 0;
}
cell *strcpy(cell *d, const cell *s) {
    cell *out = d;
    while ((*d++ = *s++)) {}
    return out;
}
cell *strncpy(cell *d, const cell *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) { d[i] = s[i]; ++i; }
    while (i < n) d[i++] = 0;
    return d;
}
cell *strcat(cell *d, const cell *s) {
    strcpy(d + strlen(d), s);
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const cell *x = a, *y = b;
    size_t i;
    for (i = 0; i < n; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
void *memchr(const void *s, int c, size_t n) {
    const cell *p = s;
    size_t i;
    for (i = 0; i < n; i++) if (p[i] == (cell)c) return (void *)(p + i);
    return 0;
}
cell *strchr(const cell *s, int c) {
    while (*s != (cell)c) { if (!*s) return 0; ++s; }
    return (cell *)s;
}
cell *strrchr(const cell *s, int c) {
    const cell *last = 0;
    do { if (*s == (cell)c) last = s; } while (*s++);
    return (cell *)last;
}
int abs(int n) { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }
long long llabs(long long n) { return n < 0 ? -n : n; }
