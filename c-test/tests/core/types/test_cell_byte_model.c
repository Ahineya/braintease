#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Record {
    char tag;
    int value;
    long wide;
};

struct LargeObject {
    char data[70000];
};

enum Color { RED, GREEN };

int main(void) {
    int source[2] = { 4660, 22136 };
    int target[2] = { 0, 99 };
    struct Record original = { 65, -2345, 305419896L };
    struct Record copy;
    unsigned char high = 65535U;
    unsigned char low = 1;
    signed char negative = -32767 - 1;
    unsigned char overlap[4] = { 50000U, 60000U, 1234, 4321 };
    size_t big = sizeof(struct LargeObject);
    char *bytes = (char *)source;

    if (CHAR_BIT == 16 && sizeof(int) == 1 && sizeof(long) == 2 &&
        sizeof(void *) == 2 && sizeof(enum Color) == 1) putchar('Y'); else putchar('N');
    if (sizeof(original) == 4 && sizeof(source) == 2 &&
        sizeof(sizeof(int)) == sizeof(size_t) && big == 70000UL) putchar('Y'); else putchar('N');
    if ((int *)(bytes + sizeof(int)) == source + 1) putchar('Y'); else putchar('N');

    memcpy(target, source, sizeof(int));
    if (target[0] == 4660 && target[1] == 99) putchar('Y'); else putchar('N');
    memcpy(&copy, &original, sizeof(original));
    if (copy.tag == 65 && copy.value == -2345 && copy.wide == 305419896L) putchar('Y'); else putchar('N');
    memset(target, 65535U, sizeof(int));
    if (target[0] == -1 && target[1] == 99) putchar('Y'); else putchar('N');
    if (memcmp(&high, &low, 1) > 0 && memcmp(&low, &high, 1) < 0) putchar('Y'); else putchar('N');
    memmove(overlap + 1, overlap, 3);
    if (overlap[0] == 50000U && overlap[1] == 50000U &&
        overlap[2] == 60000U && overlap[3] == 1234) putchar('Y'); else putchar('N');

    if (high > 0 && high / 3 == 21845U && negative < 0 &&
        (unsigned char)305419896UL == 22136U) putchar('Y'); else putchar('N');
    if (UCHAR_MAX == 65535U && UINT_LEAST8_MAX == 65535U &&
        SIZE_MAX == 4294967295UL && PTRDIFF_MAX == 2147483647L) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
