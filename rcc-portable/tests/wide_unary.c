#include <stddef.h>
volatile long input32 = 65537L;
volatile long long input64 = 0x100000001LL;
long neg32(long value) {return -value;}
long long neg64(long long value) {return -value;}
unsigned long complement32(unsigned long value) {return ~value;}
unsigned long long complement64(unsigned long long value) {return ~value;}
struct Container {long long prefix; int member;};
struct Container object = {123456789LL, 17};
struct Container *container(int *member) {
    return (struct Container *)((char *)member - offsetof(struct Container, member));
}
int main(void) {
    if (neg32(input32) != -65537L || neg32(-input32) != 65537L) return 1;
    if (neg64(input64) != -0x100000001LL || neg64(-input64) != 0x100000001LL) return 2;
    if (complement32(input32) != 0xfffefffeUL) return 3;
    if (complement64(input64) != 0xfffffffefffffffeULL) return 4;
    if (container(&object.member) != &object || container(&object.member)->prefix != 123456789LL) return 5;
    return 0;
}
