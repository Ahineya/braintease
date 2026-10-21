#include "api.h"
static unsigned long private_counter = 100UL;
static unsigned long helper(unsigned long n) { return n+private_counter; }
double gain = 2.0;
double square(double x) { return x*x+gain; }
unsigned long counter = 11UL;
unsigned long samples[3] = {5UL,7UL,9UL};
unsigned long *cursor = &samples[1];
Operation saved = helper;
struct Pair config = {42UL,1234U,&samples[2]};
#ifndef NATIVE_MODULE_GREETING
#define NATIVE_MODULE_GREETING "h\x1234"
#endif
char greeting[] = NATIVE_MODULE_GREETING;
unsigned long combine(Operation op, unsigned long n) { counter += n; return op(n)+helper(n); }
Operation callback(void) { return helper; }
unsigned long even(unsigned long n) { return n ? odd(n-1UL) : 1UL; }
