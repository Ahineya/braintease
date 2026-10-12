#include <stdio.h>
static volatile unsigned short observer;
static unsigned short repeat_add(unsigned short seed, unsigned short count, unsigned short step) {
    for (unsigned short i = 0; i < count; ++i) seed += step;
    return seed;
}
static unsigned short nested(unsigned short rows, unsigned short columns) {
    unsigned short sum = 0;
    for (unsigned short y = 0; y < rows; ++y)
        for (unsigned short x = 0; x < columns; ++x) sum += y * 10 + x;
    return sum;
}
static unsigned short early(unsigned short count) {
    unsigned short sum = 0;
    for (unsigned short i = 0; i < count; ++i) {
        sum += i;
        if (i == 5) return sum;
    }
    return sum;
}
static unsigned long wide(unsigned short count) {
    unsigned long sum = 70000L;
    for (unsigned short i = 0; i < count; ++i) sum += 65537L;
    return sum;
}
static unsigned short recurse(unsigned short count) {
    if (count == 0) return 5;
    unsigned short keep = repeat_add(100, count, count + 1);
    return keep + recurse(count - 1);
}
static void touch(volatile unsigned short *p) {
    for (unsigned short i = 0; i < 2; ++i) *p += 1;
}
static unsigned short calls(unsigned short count) {
    unsigned short sum = 0;
    for (unsigned short i = 0; i < count; ++i) { touch(&observer); sum += observer; }
    return sum;
}
static unsigned short escaped(void) {
    volatile unsigned short local = 7;
    for (unsigned short i = 0; i < 3; ++i) touch(&local);
    return local;
}
int main(void) {
    if (repeat_add(123, 0, 7) == 123) putchar('Y'); else putchar('N');
    if (repeat_add(65530, 10, 3) == 24) putchar('Y'); else putchar('N');
    if (nested(3, 4) == 138 && nested(7, 0) == 0) putchar('Y'); else putchar('N');
    if (early(0) == 0 && early(3) == 3 && early(19) == 15) putchar('Y'); else putchar('N');
    if (wide(7) == 528759L) putchar('Y'); else putchar('N');
    if (recurse(3) == 325) putchar('Y'); else putchar('N');
    observer = 0;
    if (calls(4) == 20 && observer == 8) putchar('Y'); else putchar('N');
    if (escaped() == 13) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
