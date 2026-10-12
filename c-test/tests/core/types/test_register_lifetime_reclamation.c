#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static uint32_t triple(uint32_t value)
{
    return value * 3UL + 7UL;
}

static uint16_t shared_scalar(uint16_t a, uint16_t b)
{
    return (a + b) ^ (a - b) ^ (~a) ^ (-b);
}

static uint32_t shared_low_word(uint32_t value)
{
    uint16_t low = (uint16_t)value;
    return value ^ (uint32_t)low;
}

/* The left operands and their companion words must survive the nested call. */
static uint32_t words(uint32_t a, uint32_t b,
                           uint32_t c, uint32_t d)
{
    return (a ^ b) + triple(c) + (a | d);
}

static uint64_t wide(uint64_t a, uint64_t b)
{
    return (a ^ 0x1111222233334444ULL) + triple((uint32_t)b) + (b >> 17);
}

static char global_text[] = "global";

static char *pick(char *left, char *right, int which)
{
    char *chosen;
    if (which) chosen = left;
    else chosen = right;
    /* Pointer bank companions must survive both branch arms and the loop. */
    while (*chosen == '!') ++chosen;
    return chosen;
}

static uint32_t checksum(char *text, uint32_t seed)
{
    while (*text) {
        seed = (seed << 3) ^ (uint32_t)*text;
        ++text;
    }
    return seed;
}

int main(void)
{
    char local_text[] = "!!local";
    char *heap_text = malloc(8);
    int i;
    uint32_t (*callback)(uint32_t) = triple;
    if (!heap_text) return 1;
    for (i = 0; i < 8; ++i) heap_text[i] = local_text[i];
    putchar(words(0x12345678UL, 0x87654321UL, 0xfedcba98UL, 0xabcdef01UL)
            == 0x4de544a1UL ? 'Y' : 'N');
    putchar(wide(0x123456789abcdef0ULL, 0x8877665544332211ULL)
            == 0x0325b8972953a307ULL ? 'Y' : 'N');
    putchar(pick(global_text, local_text, 1) == global_text ? 'Y' : 'N');
    putchar(pick(global_text, local_text, 0) == local_text + 2 ? 'Y' : 'N');
    putchar(pick(global_text, heap_text, 0) == heap_text + 2 ? 'Y' : 'N');
    putchar(checksum(pick(heap_text, local_text, 1), callback(0x12345678UL))
            == checksum(local_text + 2, triple(0x12345678UL)) ? 'Y' : 'N');
    putchar(shared_scalar(0x1234U, 0x5678U) == 0x9753U ? 'Y' : 'N');
    putchar(shared_low_word(0xabcd5678UL) == 0xabcd0000UL ? 'Y' : 'N');
    free(heap_text);
    putchar('\n');
    return 0;
}
