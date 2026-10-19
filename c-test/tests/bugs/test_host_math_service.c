#include <stdio.h>
extern unsigned long long __rcc_host_math(unsigned, unsigned long long, unsigned long long);
int main(void) {
    unsigned long long value = __rcc_host_math(0,0,0);
    if (value==7) putchar('Y'); else putchar('N');
    value=__rcc_host_math(0x311,0x3ff0000000000000ULL,0);
    if (value==0) putchar('Y'); else putchar('N');
    value=__rcc_host_math(0x319,0x4000000000000000ULL,0x4024000000000000ULL);
    if (value==0x4090000000000000ULL) putchar('Y'); else putchar('N');
    putchar('\n'); return 0;
}
