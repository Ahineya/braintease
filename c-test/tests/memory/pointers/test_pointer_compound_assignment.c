#include <stdint.h>
#include <stdio.h>

#ifndef BANK_SIZE
#define BANK_SIZE 64000
#endif

static char *saved;
static int calls;
static char **once(void) { ++calls; return &saved; }
static char *at(unsigned int bank, unsigned int offset) {
    return (char *)(((uintptr_t)bank << 16) | offset);
}
static char *advance(char *pointer, long delta) {
    pointer += delta;
    return pointer;
}

int main(void) {
    char *pointer = at(5, BANK_SIZE - 1);
    long far = 140000L;
    unsigned int large = 56000U;
    int negative = -1;
    int rows[3][3];
    int (*row)[3] = rows;
    struct { char *position; } cursor;

    if ((pointer += 1) != at(6, 0) || pointer != at(6, 0)) return 1;
    if ((pointer -= 1) != at(5, BANK_SIZE - 1)) return 2;
    pointer += negative;
    if (pointer != at(5, BANK_SIZE - 2)) return 3;
    pointer -= negative;
    if (pointer != at(5, BANK_SIZE - 1)) return 4;
    pointer = at(5, 0);
    pointer += far;
    if (pointer != at(5 + far / BANK_SIZE, far % BANK_SIZE)) return 5;
    pointer -= far;
    if (pointer != at(5, 0)) return 6;
    pointer += large;
    if (pointer != at(5 + large / BANK_SIZE, large % BANK_SIZE)) return 7;
    pointer -= large;
    if (pointer != at(5, 0)) return 8;
    if (advance(at(6, 0), -1L) != at(5, BANK_SIZE - 1)) return 9;
    row += 2;
    if (row != &rows[2]) return 10;
    row -= 1;
    if (row != &rows[1]) return 11;
    saved = at(5, BANK_SIZE - 1);
    if ((*once() += 2) != at(6, 1) || saved != at(6, 1) || calls != 1) return 12;
    if ((*once() -= 2) != at(5, BANK_SIZE - 1) || calls != 2) return 13;
    cursor.position = at(6, 0);
    cursor.position -= 1;
    if (cursor.position != at(5, BANK_SIZE - 1)) return 14;
    puts("pointer compound assignment ok");
    return 0;
}
