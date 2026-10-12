#include <stdio.h>

static volatile unsigned short effects;

static int delay(int value) {
    for (int i = 0; i < 5000; ++i) {}
    for (unsigned short i = 65534; i < 65535; ++i) {}
    for (int i = -32768; i < 32767; ++i) {}
    for (int i = 7; i < 0; ++i) {}
    return value + 7;
}

static unsigned short live_counter(void) {
    unsigned short i;
    for (i = 0; i < 5; ++i) {}
    return i;
}

static void observable(void) {
    for (unsigned short i = 0; i < 7; ++i) ++effects;
    for (volatile unsigned short i = 0; i < 3; ++i) effects += 2;
}

int main(void) {
    if (delay(5) == 12) putchar('Y'); else putchar('N');
    if (live_counter() == 5) putchar('Y'); else putchar('N');
    effects = 0;
    observable();
    if (effects == 13) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
