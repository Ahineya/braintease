#include <stdint.h>
#include <stdio.h>

/* Numeric pointer banks remain known through same-bank indexing and casts.
   The adjacent boundary and signed cases must still use the general path. */
static volatile unsigned int index_input = 7;
static volatile long negative_input = -1;
static volatile unsigned long far_input = 65536UL;
static volatile unsigned int *saved;

static uintptr_t pointer_value(volatile unsigned int *p) { return (uintptr_t)p; }

int main(void) {
    volatile unsigned int *base = (volatile unsigned int *)256UL;
    unsigned int i;
    unsigned int correct = 1;
    for (i = 0; i < 8; ++i) base[i] = 200U + i;
    for (i = 0; i < 8; ++i) if (base[i] != 200U + i) correct = 0;
    putchar(correct ? 'Y' : 'N');
    saved = (volatile unsigned int *)(void *)&base[3];
    putchar(*saved == 203 ? 'Y' : 'N');
    putchar(pointer_value(saved) == 259UL ? 'Y' : 'N');
    putchar((uintptr_t)(void *)base == 256UL ? 'Y' : 'N');
    putchar((void *)0 == (void *)(uintptr_t)0 ? 'Y' : 'N');
    {
        unsigned int index = index_input;
        volatile unsigned int *near = (volatile unsigned int *)0x0003f9fcUL;
        putchar(index < 8 && (uintptr_t)&near[index] == 0x00040003UL ? 'Y' : 'N');
        putchar((uintptr_t)(near + negative_input) == 0x0003f9fbUL ? 'Y' : 'N');
        putchar((uintptr_t)(near + far_input) == 0x000505fcUL ? 'Y' : 'N');
        putchar(pointer_value((volatile unsigned int *)0x00030000UL) == 0x00030000UL ? 'Y' : 'N');
    }
    putchar((unsigned long long)65535L == 65535ULL ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
