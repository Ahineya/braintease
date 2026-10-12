#include <stdio.h>

static void dirty_stack(void) {
    volatile unsigned short words[256];
    unsigned short i;
    for (i = 0; i < 256; ++i) words[i] = 0xabcdU;
}

static unsigned long join_words(unsigned short lo, unsigned short hi) {
    return (unsigned long)lo | ((unsigned long)hi << 16);
}

/* The left operand's high cell must survive the shift's internal branches.
 * Dirty stack storage prevents skipped spills from accidentally reading zero. */
static unsigned long left(unsigned long keep, unsigned long value, unsigned short count) {
    return keep | (value << count);
}

static unsigned long right(unsigned long keep, unsigned long value, unsigned short count) {
    return keep | (value >> count);
}

static unsigned long signed_right(unsigned long keep, long value, unsigned short count) {
    return keep ^ (unsigned long)(value >> count);
}

static int fail(int code) {
    printf("long shift failure %d\n", code);
    return 1;
}

int main(void) {
    dirty_stack();
    if (join_words(0, 0) != 0UL) return fail(1);
    dirty_stack();
    if (join_words(0x5678U, 0x1234U) != 0x12345678UL) return fail(2);
    dirty_stack();
    if (left(0x12340000UL, 1UL, 16) != 0x12350000UL) return fail(3);
    dirty_stack();
    if (left(0x12340000UL, 1UL, 17) != 0x12360000UL) return fail(4);
    dirty_stack();
    if (left(0x12340000UL, 1UL, 31) != 0x92340000UL) return fail(5);
    dirty_stack();
    if (right(0xdead0000UL, 0x89abcdefUL, 16) != 0xdead89abUL) return fail(6);
    dirty_stack();
    if (right(0xdead0000UL, 0x89abcdefUL, 17) != 0xdead44d5UL) return fail(7);
    dirty_stack();
    if (right(0xdead0000UL, 0x89abcdefUL, 31) != 0xdead0001UL) return fail(8);
    dirty_stack();
    if (signed_right(0x12340000UL, -2147483647L, 16) != 0xedcb8000UL) return fail(9);
    dirty_stack();
    if (signed_right(0x12340000UL, -2147483647L, 17) != 0xedcbc000UL) return fail(10);
    dirty_stack();
    if (signed_right(0x12340000UL, -2147483647L, 31) != 0xedcbffffUL) return fail(11);
    dirty_stack();
    if (signed_right(0x12340000UL, 0x12345678L, 16) != 0x12341234UL) return fail(12);
    dirty_stack();
    if (left(0UL, 0x12345678UL, 0) != 0x12345678UL) return fail(13);
    dirty_stack();
    if (right(0UL, 0x12345678UL, 15) != 0x00002468UL) return fail(14);
    dirty_stack();
    if (signed_right(0UL, -2147483647L, 1) != 0xc0000000UL) return fail(15);
    puts("long shifts ok");
    return 0;
}
