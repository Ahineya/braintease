#include <stdint.h>
#include <stdio.h>

static volatile uint32_t unsigned_control;
static volatile int32_t signed_control;
static volatile uint64_t widest_control;

static int classify_unsigned(void)
{
    switch (unsigned_control) {
        case UINT32_C(0x616d6500): return 1;
        case UINT32_C(0x616d7800): return 2;
        case UINT32_C(0x00006500): return 3;
        case UINT32_C(0xffff6500): return 4;
        default: return 5;
    }
}

static int classify_signed(void)
{
    switch (signed_control) {
        case -70000L: return 1;
        case 70000L: return 2;
        case -1L: return 3;
        case INT32_MIN: return 4;
        default: return 5;
    }
}

static int classify_widest(void)
{
    switch (widest_control) {
        case UINT64_C(0x1234567887654321): return 1;
        case UINT64_C(0x0000000087654321): return 2;
        case UINT64_C(0xffffffff87654321): return 3;
        case UINT64_MAX: return 4;
        default: return 5;
    }
}

int main(void)
{
    unsigned_control = UINT32_C(0x616d6500);
    putchar(classify_unsigned() == 1 ? 'Y' : 'N');
    unsigned_control = UINT32_C(0x616d7800);
    putchar(classify_unsigned() == 2 ? 'Y' : 'N');
    unsigned_control = UINT32_C(0x00006500);
    putchar(classify_unsigned() == 3 ? 'Y' : 'N');
    unsigned_control = UINT32_C(0xffff6500);
    putchar(classify_unsigned() == 4 ? 'Y' : 'N');
    unsigned_control = UINT32_C(0x11116500);
    putchar(classify_unsigned() == 5 ? 'Y' : 'N');
    signed_control = -70000L;
    putchar(classify_signed() == 1 ? 'Y' : 'N');
    signed_control = 70000L;
    putchar(classify_signed() == 2 ? 'Y' : 'N');
    signed_control = -1L;
    putchar(classify_signed() == 3 ? 'Y' : 'N');
    signed_control = INT32_MIN;
    putchar(classify_signed() == 4 ? 'Y' : 'N');
    signed_control = 123L;
    putchar(classify_signed() == 5 ? 'Y' : 'N');
    widest_control = UINT64_C(0x1234567887654321);
    putchar(classify_widest() == 1 ? 'Y' : 'N');
    widest_control = UINT64_C(0x0000000087654321);
    putchar(classify_widest() == 2 ? 'Y' : 'N');
    widest_control = UINT64_C(0xffffffff87654321);
    putchar(classify_widest() == 3 ? 'Y' : 'N');
    widest_control = UINT64_MAX;
    putchar(classify_widest() == 4 ? 'Y' : 'N');
    widest_control = UINT64_C(0x1234567800000000);
    putchar(classify_widest() == 5 ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
