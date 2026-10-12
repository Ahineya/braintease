#include <stdio.h>
static long triangle[] = {(long)(-.867 * 65536L), (long)(-.5 * 65536L), (long)(.7 * 65536L)};
static long initial_scale = (long)(.2 * 65536L);
static long single_round = (long)(-16777216.0f + 16777217L);
static unsigned long high = (unsigned long)(2147483648.0 + 256.0);
static long negative = (long)-2147483648.0;
static int truncated = (int)(-7.75 / 2.0);
static float gain = 1.0f + .5f;
static double offset = -3.5;
int main(void)
{
    union { float value; unsigned long bits; } single;
    union { double value; unsigned long long bits; } double_precision;
    putchar(triangle[0] == -56819L && triangle[1] == -32768L && triangle[2] == 45875L ? 'Y' : 'N');
    putchar(initial_scale == 13107L && single_round == 0 ? 'Y' : 'N');
    putchar(high == 2147483904UL && negative == (-2147483647L - 1) ? 'Y' : 'N');
    putchar(truncated == -3 ? 'Y' : 'N');
    single.value = gain;
    double_precision.value = offset;
    putchar(single.bits == 0x3fc00000UL && double_precision.bits == 0xc00c000000000000ULL ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
