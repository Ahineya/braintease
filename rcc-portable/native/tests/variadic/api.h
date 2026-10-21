#ifndef NATIVE_VARIADIC_PROBE_H
#define NATIVE_VARIADIC_PROBE_H
#include <stdarg.h>
typedef unsigned long (*Callback)(unsigned long);
int mixed(int, ...);
unsigned long recursive(unsigned, ...);
unsigned long many(unsigned, ...);
int empty(unsigned, ...);
#endif
