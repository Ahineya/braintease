#include <stdio.h>
extern int rcc_process_getpid(void);
static volatile int use_process;
static unsigned short read_version(unsigned short address) {
    unsigned short value;
    __asm__("LOAD %0, R0, %1" : "=r"(value) : "r"(address));
    return value;
}
int main(void) {
    if(use_process)rcc_process_getpid();
    unsigned short version=read_version(31);
    putchar(version==1?'Y':'N');putchar('\n');return 0;
}
