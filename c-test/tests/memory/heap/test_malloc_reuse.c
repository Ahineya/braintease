#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static void check(int condition) { putchar(condition ? 'Y' : 'N'); }

int main(void) {
    unsigned char *a, *b, *c, *guard, *p;
    unsigned int i;
    int ok;
    free(NULL);
    check(malloc(0) == NULL && calloc(0, 4) == NULL && calloc(4, 0) == NULL);
    check(calloc(SIZE_MAX / 2 + 1, 2) == NULL && malloc(SIZE_MAX) == NULL);

    /* Merge both neighbors, then split and merge that result again. */
    a = malloc(60); b = malloc(80); c = malloc(100); guard = malloc(17);
    if (!a || !b || !c || !guard) return 1;
    guard[0] = 91; guard[16] = 92;
    free(a); free(c); free(b);
    p = malloc(220);
    check(p == a && guard[0] == 91 && guard[16] == 92);
    free(p); free(guard);

    /* Adjacent growth and shrink preserve the address and old contents. */
    a = malloc(64); b = malloc(128); guard = malloc(17);
    if (!a || !b || !guard) return 2;
    for (i = 0; i < 64; i++) a[i] = i + 1000;
    free(b);
    p = realloc(a, 180);
    ok = p == a;
    for (i = 0; i < 64; i++) if (p[i] != i + 1000) ok = 0;
    check(ok);
    a = realloc(p, 8);
    p = realloc(a, 190);
    check(p == a && p[0] == 1000 && p[7] == 1007);
    /* Failure leaves the original allocation alive. */
    check(realloc(p, SIZE_MAX) == NULL && p[7] == 1007);
    free(p); free(guard);

    a = malloc(20); guard = malloc(20);
    if (!a || !guard) return 3;
    for (i = 0; i < 20; i++) a[i] = i + 2000;
    p = realloc(a, 1000);
    if (!p) return 4;
    ok = p != a;
    for (i = 0; i < 20; i++) if (p[i] != i + 2000) ok = 0;
    check(ok);
    free(p); free(guard);

    /* A failed grow may absorb a too-small free neighbor. The original
     * bytes must survive and the absorbed node must leave its free list.
     */
    a = malloc(32); b = malloc(20);
    guard = malloc(251UL * 64000UL - 33 - 32 - 20);
    if (!a || !b || !guard) return 6;
    for (i = 0; i < 32; i++) a[i] = i + 3000;
    free(b);
    p = realloc(a, 100);
    check(p == NULL && malloc(1) == NULL && a[0] == 3000 && a[31] == 3031);
    free(guard); free(a);

    p = realloc(NULL, 12);
    check(p != NULL && realloc(p, 0) == NULL);
    p = calloc(513, sizeof(unsigned char));
    if (!p) return 5;
    ok = 1;
    for (i = 0; i < 513; i++) if (p[i] != 0) ok = 0;
    check(ok);
    free(p);
    putchar('\n');
    return 0;
}
