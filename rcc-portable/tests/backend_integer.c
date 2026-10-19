#include <stdint.h>
volatile long long minimum=(-9223372036854775807LL-1LL), negative=-9LL;
volatile unsigned long long top=0xffffffffffffffffULL, divisor=0x8000000000000001ULL;
volatile long low=0x12345678L;
volatile long long shift=4;
long long constant_extension(void) {return 32768L;}
int main(void) {
    long long a=negative,b=4;
    unsigned long long u=top,d=divisor;
    if(a/b!=-2 || a%b!=-1) return 1;
    if(minimum/-1LL!=minimum || minimum%-1LL!=0) return 2;
    if(u/d!=1 || u%d!=0x7ffffffffffffffeULL) return 3;
    if((minimum>>63)!=-1 || (u>>63)!=1 || (u<<63)!=0x8000000000000000ULL) return 4;
    if((low>>shift)!=0x1234567L || (low<<shift)!=0x23456780L) return 5;
    if(constant_extension()!=32768LL) return 6;
    if((u+1)!=0 || (u*3)!=0xfffffffffffffffdULL || -minimum!=minimum) return 7;
    if((a&7)!=7 || (a|7)!=-9 || (a^7)!=-16) return 8;
    return 0;
}
