#include <stdio.h>
static unsigned short width = 7;
static unsigned short height = 5;
static volatile unsigned short observed = 0;
static unsigned short changing = 0;

static unsigned short paint(unsigned short x, unsigned short y) {
    if (x >= width || y >= height) return 99;
    return width * height + y * width + x;
}
static unsigned short grid(void) {
    unsigned short sum = 0;
    for (unsigned short y = 0; y < height; ++y)
        for (unsigned short x = 0; x < width; ++x)
            sum += paint(x, y);
    return sum;
}
static unsigned short mutate_through_pointer(unsigned short *value) {
    unsigned short sum = 0;
    for (unsigned short i = 0; i < 3; ++i) {
        sum += changing;
        *value += 1;
    }
    return sum;
}
static void barrier(void) {
    for (unsigned short i = 0; i < 2; ++i) changing += 1;
}
static unsigned short calls(void) {
    unsigned short sum = 0;
    for (unsigned short i = 0; i < 3; ++i) {
        sum += changing;
        barrier();
    }
    return sum;
}
int main(void) {
    if (grid() == 1820) putchar('Y'); else putchar('N');
    width = 0;
    if (grid() == 0) putchar('Y'); else putchar('N');
    width = 3; height = 2;
    if (grid() == 51) putchar('Y'); else putchar('N');
    if (paint(3, 0) == 99 && paint(0, 2) == 99) putchar('Y'); else putchar('N');
    for (unsigned short i = 0; i < 4; ++i) observed = observed + 1;
    if (observed == 4) putchar('Y'); else putchar('N');
    if (mutate_through_pointer(&changing) == 3 && changing == 3) putchar('Y'); else putchar('N');
    changing = 0;
    if (calls() == 6 && changing == 6) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
