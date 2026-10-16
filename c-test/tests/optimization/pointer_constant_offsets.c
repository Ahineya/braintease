#include <stdio.h>
#include <stdint.h>
#ifndef BANK_SIZE
#define BANK_SIZE 64000
#endif
static volatile uintptr_t location;
static unsigned short at_zero(unsigned short *p) { return p[0]; }
static unsigned short at_next(unsigned short *p) { return p[1]; }
static unsigned short at_previous(unsigned short *p) { return p[-1]; }
static void check(int good) { putchar(good ? 'Y' : 'N'); }
int main(void) {
    /* Initialize absolute cells without using any pointer-offset expression. */
    *(volatile unsigned short *)((3UL<<16)|(BANK_SIZE-1))=1234;
    *(volatile unsigned short *)0x00040000UL=5678;
    *(volatile unsigned short *)0x00040001UL=9012;
    location=(3UL<<16)|(BANK_SIZE-1);
    check(at_zero((unsigned short *)location)==1234);
    check(at_next((unsigned short *)location)==5678);
    location=0x00040000UL;
    check(at_previous((unsigned short *)location)==1234);
    check(at_zero((unsigned short *)location)==5678);
    check(at_next((unsigned short *)location)==9012);
    location=0x00040001UL;
    check(at_previous((unsigned short *)location)==5678);
    putchar('\n'); return 0;
}
