/* Portable native lowering must release analysis and generated text after
 * each function; the companion allocator-budget check measures compilation.
 * Ordinary rct execution is the semantic control, not the memory regression. */
#include <stdio.h>
typedef unsigned long (*operation)(unsigned long);
static unsigned long bump(unsigned long x) { return x*1664525UL+1013904223UL; }
static operation volatile apply=bump;
#define STEP x=apply(x);
#define S2 STEP STEP
#define S4 S2 S2
#define S8 S4 S4
#define S16 S8 S8
#define S32 S16 S16
#define S64 S32 S32
#define S128 S64 S64
#define FUNCTION(n) static unsigned long f##n(unsigned long x) {S128 return x;}
FUNCTION(0)
FUNCTION(1)
FUNCTION(2)
FUNCTION(3)
FUNCTION(4)
FUNCTION(5)
FUNCTION(6)
FUNCTION(7)
FUNCTION(8)
FUNCTION(9)
FUNCTION(10)
FUNCTION(11)
FUNCTION(12)
FUNCTION(13)
FUNCTION(14)
FUNCTION(15)
FUNCTION(16)
FUNCTION(17)
FUNCTION(18)
FUNCTION(19)
FUNCTION(20)
FUNCTION(21)
FUNCTION(22)
FUNCTION(23)
FUNCTION(24)
FUNCTION(25)
FUNCTION(26)
FUNCTION(27)
FUNCTION(28)
FUNCTION(29)
FUNCTION(30)
FUNCTION(31)
int main(void) {
 unsigned long x=123456789UL;
 x=f0(x);
 x=f1(x);
 x=f2(x);
 x=f3(x);
 x=f4(x);
 x=f5(x);
 x=f6(x);
 x=f7(x);
 x=f8(x);
 x=f9(x);
 x=f10(x);
 x=f11(x);
 x=f12(x);
 x=f13(x);
 x=f14(x);
 x=f15(x);
 x=f16(x);
 x=f17(x);
 x=f18(x);
 x=f19(x);
 x=f20(x);
 x=f21(x);
 x=f22(x);
 x=f23(x);
 x=f24(x);
 x=f25(x);
 x=f26(x);
 x=f27(x);
 x=f28(x);
 x=f29(x);
 x=f30(x);
 x=f31(x);
 if(x==265772309UL)putchar('Y');else putchar('N');
 if(apply(0)==1013904223UL)putchar('Y');else putchar('N');
 if(sizeof(unsigned long)==2)putchar('Y');else putchar('N');
 putchar('\n');return 0;
}
