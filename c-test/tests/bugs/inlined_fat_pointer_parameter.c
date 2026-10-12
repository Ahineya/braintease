#include <stdint.h>
#include <stdio.h>

static volatile unsigned int data[2] = {41, 73};
static volatile unsigned int *saved;

static uintptr_t bits(volatile unsigned int *p) { return (uintptr_t)p; }
static volatile unsigned int *identity(volatile unsigned int *p) { return p; }

int main(void) {
    saved = &data[1];
    if (bits(saved) == (uintptr_t)&data[1]) putchar('Y'); else putchar('N');
    if (*identity(saved) == 73) putchar('Y'); else putchar('N');
    if (bits((volatile unsigned int *)0x00030000UL) == 0x00030000UL) putchar('Y'); else putchar('N');
    if (bits((volatile unsigned int *)0) == 0UL) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
