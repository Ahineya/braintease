/* Direct helpers must preserve effects, wide values and function-pointer ABI. */
#include <stdio.h>
static volatile unsigned long calls;
static unsigned long leaf(unsigned long n) { ++calls; return n * 65537UL + 7; }
static unsigned long nested(unsigned long n) { return leaf(n) ^ 0x12345678UL; }
static unsigned long (*volatile callback)(unsigned long) = nested;
static int choose(int n) { if(n < 0) return -7; return n + 13; }
static unsigned long invoke(unsigned long (*f)(unsigned long), unsigned long n) { return f(n); }
static void check(int ok) { putchar(ok ? 'Y' : 'N'); }
int main(void) {
    check(nested(3) == ((3UL * 65537UL + 7) ^ 0x12345678UL));
    check(calls == 1);
    check(callback(5) == ((5UL * 65537UL + 7) ^ 0x12345678UL));
    check(invoke(callback, 9) == ((9UL * 65537UL + 7) ^ 0x12345678UL));
    check(calls == 3);
    check(choose(-1) == -7 && choose(2) == 15);
    putchar('\n'); return 0;
}
