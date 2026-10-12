#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifndef BANK_SIZE
#define BANK_SIZE 64000
#endif

static int values[2] = {4, 7};
static char *saved_pointer;

static char *at(unsigned int bank, unsigned int offset) {
    return (char *)(((uintptr_t)bank << 16) | offset);
}

int main(void) {
    char *end = at(5, BANK_SIZE - 1);
    char *next = at(6, 0);
    int negative = -1;
    unsigned int large = 56000U;
    unsigned long far = 140000UL;
    int *p = &values[1];
    int rows[2][3];
    int (*row)[3] = rows;

    if (at(5, 1) == at(6, 1) || !next || next == 0) return 1;
    if ((uintptr_t)at(5, 0x1234U) != 0x00051234UL) return 2;
    if (end + 1 != next || next - 1 != end) return 3;
    if (next + negative != end || next - end != 1L || end - next != -1L) return 4;
    if (at(5, 0) + large != at(5 + large / BANK_SIZE, large % BANK_SIZE)) return 5;
    if (at(5, 0) + far != at(5 + far / BANK_SIZE, far % BANK_SIZE)) return 6;
    if (*p != 7 || !p || p != &values[1] || p - &values[0] != 1L) return 7;
    if ((void *)(uintptr_t)(void *)p != (void *)p) return 8;
    end++;
    next--;
    if (end != at(6, 0) || next != at(5, BANK_SIZE - 1)) return 9;
    if (row + 1 != &rows[1] || (row + 1) - row != 1L) return 10;

    /* Address-of and assignment use the lvalue indexing path. */
    if (&p[negative] != &values[0]) return 11;
    p[negative] = 37;
    if (values[0] != 37) return 12;
    {
        char *q = at(6, 0);
        char *boundary = q;
        if (q-- != boundary || q != boundary - 1) return 13;
        if (++q != boundary || q != boundary) return 14;
        if (--q != boundary - 1 || q != boundary - 1) return 15;
        if (q++ != boundary - 1 || q != boundary) return 16;
        saved_pointer = boundary;
        if (--saved_pointer != boundary - 1) return 17;
        if (saved_pointer++ != boundary - 1 || saved_pointer != boundary) return 18;
    }

    puts("banked pointer semantics ok");
    return 0;
}
