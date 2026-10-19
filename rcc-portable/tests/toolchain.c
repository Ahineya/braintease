#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
struct Pair {int a;long b;};
struct Pair make(int a,long b) {struct Pair p={a,b};return p;}
unsigned short storage[64010];
long sum(int count,...) {
    va_list ap;long total=0;int i;va_start(ap,count);
    for(i=0;i<count;i++)total+=va_arg(ap,long);
    va_end(ap);return total;
}
int main(void) {
    struct Pair p=make(3,70000L);unsigned short *cross=storage+63999;
    unsigned long long large=0xfedcba9876543210ULL;
    union {double number;uint64_t bits;} floating;
    cross[0]=17;cross[1]=23;
    if(p.a!=3||p.b!=70000L||cross[0]+cross[1]!=40) return 1;
    if(sum(3,1L,2L,70000L)!=70003L||large/256ULL!=0xfedcba98765432ULL) return 2;
    floating.number=0x1p63;
    if(floating.bits!=0x43e0000000000000ULL || -0X1.8p+2!=-6.0 || 0x.8p-1!=0.25) return 3;
    puts("Compiled and linked entirely in the guest!");
    return 0;
}
