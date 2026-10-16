#include <stdio.h>
static volatile unsigned long source32;
static volatile unsigned long long source64;
static volatile unsigned long observed;
static unsigned long pattern32(unsigned long x) { return (x * 37UL + 11UL) & 255UL; }
static unsigned long long pattern64(unsigned long long x) { return (x * 37ULL + 11ULL) & 65535ULL; }
static unsigned long (*volatile call32)(unsigned long)=pattern32;
static unsigned long long (*volatile call64)(unsigned long long)=pattern64;
static unsigned long keep_wide(unsigned long x) { unsigned long y=x*37UL+11UL; observed=y; return y&255UL; }
static void check(int good) { putchar(good ? 'Y' : 'N'); }
int main(void) {
    source32=0xffffffffUL; check(call32(source32)==230UL);
    source32=0x12345678UL; check(pattern32(source32)==99UL);
    source32=0x80000000UL; check(pattern32(source32)==11UL);
    source32=0x10000UL; check(pattern32(source32)==11UL);
    source64=0xffffffffffffffffULL; check(call64(source64)==65510ULL);
    source64=0x123456789abcdef0ULL; check(pattern64(source64)==14523ULL);
    source32=0x12345678UL; check(keep_wide(source32)==99UL && observed==0xa1907f63UL);
    check(((source32/37UL+11UL)&65535UL)==(unsigned short)(source32/37UL+11UL));
    check(((source32>>17)+11UL)%256UL==37UL);
    check(((source32<<17)+11UL)%256UL==11UL);
    check(((source32-1234567UL)^0xa5a5a5a5UL)%65536UL==55892UL);
    putchar('\n'); return 0;
}
