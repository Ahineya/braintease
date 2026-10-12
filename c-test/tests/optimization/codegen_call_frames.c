#include <stdio.h>
#include <stdarg.h>

int scalar_leaf(int a, int b) { return a + b; }
long wide_leaf(long a, long b) { return (a ^ b) + 3L; }
int stack_leaf(int a,int b,int c,int d,int e,int f) { return a+b+c+d+e+f; }
int recursive_frame(int n) { if(n == 0) return 7; return n + recursive_frame(n-1); }
int variadic_frame(int count, ...) {
    va_list ap; int sum=0;
    va_start(ap,count);
    for(int i=0;i<count;i++) sum += va_arg(ap,int);
    va_end(ap);
    return sum;
}
int (*volatile scalar_call)(int,int) = scalar_leaf;
long (*volatile wide_call)(long,long) = wide_leaf;
int (*volatile stack_call)(int,int,int,int,int,int) = stack_leaf;

int main(void) {
    if(scalar_call(30000,1234)==31234) putchar('Y'); else putchar('N');
    if(wide_call(0x12345678L,0x76543210L)==0x6460646bL) putchar('Y'); else putchar('N');
    if(stack_call(1,2,3,4,5,6)==21) putchar('Y'); else putchar('N');
    if(recursive_frame(5)==22) putchar('Y'); else putchar('N');
    if(variadic_frame(6,10,20,30,40,50,60)==210) putchar('Y'); else putchar('N');
    putchar('\n'); return 0;
}
