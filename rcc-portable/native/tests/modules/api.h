#ifndef NATIVE_MODULE_TEST_H
#define NATIVE_MODULE_TEST_H
typedef unsigned long (*Operation)(unsigned long);
struct Pair { unsigned long value; unsigned flag; unsigned long *pointer; };
extern unsigned long counter, samples[3];
extern unsigned long *cursor;
extern Operation saved;
extern struct Pair config;
extern char greeting[];
unsigned long combine(Operation, unsigned long);
Operation callback(void);
unsigned long peer(unsigned long);
unsigned long even(unsigned long);
unsigned long odd(unsigned long);
double square(double);
extern double gain;
#endif
