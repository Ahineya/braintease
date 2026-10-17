#include <stdio.h>
static volatile int input;
static int yes_calls, no_calls;
static void yes(void) { ++yes_calls; }
static void no(void) { ++no_calls; }
static void choose(int condition) { condition ? yes() : no(); }
int main(void) {
    input = 1;
    choose(input);
    if (yes_calls == 1 && no_calls == 0) putchar('Y'); else putchar('N');
    input = 0;
    choose(input);
    if (yes_calls == 1 && no_calls == 1) putchar('Y'); else putchar('N');
    input ? (void)++yes_calls : (void)++no_calls;
    if (yes_calls == 1 && no_calls == 2) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
