#include <stdio.h>
#include <stdlib.h>
static unsigned values[70003];
static unsigned long wide[33003];
static unsigned *middle = &values[64000L];
static unsigned *last = &values[70002L];
static unsigned long *wide_last = &wide[33002L];
static void check(int value) { putchar(value ? 'Y' : 'N'); }
int main(void) {
    unsigned *p = values + 63998L;
    check(values[0] == 0 && *last == 0 && *wide_last == 0);
    values[0] = 123;
    p[0] = 40000U; p[1] = 40001U; p[2] = 40002U; p[3] = 40003U;
    *last = 65535U;
    *wide_last = 0x1234abcdUL;
    check(values[0] == 123 && p[0] == 40000U && p[3] == 40003U);
    check(*middle == 40002U && values[70002L] == 65535U);
    check(wide[33002L] == 0x1234abcdUL);
    check(last - values == 70002L && middle - p == 2);
    p = malloc(3000);
    check(p != NULL);
    if (p) { p[0] = 789; check(p[0] == 789 && *last == 65535U); free(p); }
    else check(0);
    putchar('\n');
    return 0;
}
