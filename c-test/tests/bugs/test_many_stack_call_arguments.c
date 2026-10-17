#include <stdio.h>
#include <stdarg.h>
int sum(int count, ...) {
    va_list args;
    int i, total = 0;
    va_start(args, count);
    for (i = 0; i < count; i++) total += va_arg(args, int);
    va_end(args);
    return total;
}
int (*indirect)(int, ...) = sum;
unsigned long mixed(int count, ...) {
    va_list args;
    int i;
    unsigned long total = 0;
    va_start(args, count);
    for (i = 0; i < count; i++) {
        unsigned long value = va_arg(args, unsigned long);
        char *text = va_arg(args, char *);
        total += value + *text;
    }
    va_end(args);
    return total;
}
int main(void) {
    if (sum(20,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20)==210)
        putchar('Y'); else putchar('N');
    if (indirect(20,20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1)==210)
        putchar('Y'); else putchar('N');
    if (mixed(4,70000UL,"a",80000UL,"b",90000UL,"c",100000UL,"d")==340394UL)
        putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
