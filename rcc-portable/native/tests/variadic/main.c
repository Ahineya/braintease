#include "api.h"
static unsigned long callback(unsigned long n) { return n * 3UL + 1UL; }
int main(void)
{
    unsigned char padding[63990];
    int (*indirect)(int, ...) = mixed;
    signed char small = -7;
    unsigned char last = 511U;
    float number = 1.5f;
    unsigned long word = 0x12345678UL;
    unsigned long long wide = 0xFEDCBA9876543210ULL;
    padding[0] = 1; padding[63989] = 2;
    if (!indirect(31, small, word, wide, number, "ok", callback, last)) return 1;
    if (!mixed(31, -7, 0x12345678UL, 0xFEDCBA9876543210ULL, 1.5, "ok", callback, 511U)) return 2;
    if (recursive(3, 10UL, 20UL) != 105UL) return 3;
    if (!empty(23)) return 4;
    if (many(70,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,
             21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
             41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,
             61,62,63,64,65,66,67,68,69,70) != 2485UL) return 5;
    if (padding[0] != 1 || padding[63989] != 2) return 6;
    return 0;
}
