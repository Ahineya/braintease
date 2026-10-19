#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <__banked_memory.h>

/* Exercise the shared C routines on native as well: standalone native string
 * services would otherwise bypass the guest library under test. */
#define memcpy __rcc_banked_copy
#define memset __rcc_banked_set
#define strlen __rcc_banked_length
#define strcmp __rcc_banked_compare

/* Buffers start at different offsets just before bank boundaries. Counts pass
 * 65535, so truncating size_t or assuming matching source/destination spans fails.
 */
static char *edge(char *p, unsigned before) {
    return p + (64000U - (unsigned)(uintptr_t)p) - before;
}
int main(void) {
    char *one = malloc(140000UL), *two = malloc(140000UL), *a, *b;
    unsigned long n = 65550UL, i;
    int good;
    if (!one || !two) return 1;
    a = edge(one, 7);
    b = edge(two, 19);
    a[-1] = 123;
    a[n] = 234;
    if (memset(a, 60000U, n) == a && a[-1] == 123 && a[n] == 234) putchar('Y'); else putchar('N');
    b[-1] = 345;
    b[n] = 456;
    if (memcpy(b, a, n) == b && b[-1] == 345 && b[n] == 456) putchar('Y'); else putchar('N');
    good = 1;
    for (i = 0; i < n; i++) if ((unsigned char)b[i] != 60000U) good = 0;
    if (good && memcmp(a, b, n) == 0) putchar('Y'); else putchar('N');
    a[n - 1] = 0;
    b[n - 1] = 0;
    if (strlen(a) == n - 1 && strlen(b) == n - 1 && strcmp(a, b) == 0) putchar('Y'); else putchar('N');
    b[n - 2] = 50000U;
    if (strcmp(a, b) > 0 && strcmp(b, a) < 0) putchar('Y'); else putchar('N');
    a[0] = 50000U;
    a[1] = 1234;
    a[n - 2] = 4321;
    memmove(a + 1, a, n - 1);
    if ((unsigned char)a[1] == 50000U && a[2] == 1234 && a[n - 1] == 4321 && a[n] == 234) putchar('Y'); else putchar('N');
    memmove(a, a + 1, n - 1);
    if ((unsigned char)a[0] == 50000U && a[1] == 1234 && a[n - 2] == 4321 && a[-1] == 123) putchar('Y'); else putchar('N');
    /* A zero count must leave the one-past pointer untouched. */
    a = one + 140000UL;
    if (memcpy(a, a, 0) == a && memset(a, 0, 0) == a && memmove(a, a, 0) == a && memcmp(a, a, 0) == 0) putchar('Y'); else putchar('N');
    free(one);
    free(two);
    putchar('\n');
    return 0;
}
