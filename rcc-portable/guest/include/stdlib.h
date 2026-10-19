#ifndef RCC_PORTABLE_GUEST_STDLIB_H
#define RCC_PORTABLE_GUEST_STDLIB_H
/* Keep bootstrap additions local while the common guest SDK evolves. */
#include "../../../ports/fuzix/upstream/Library/include/stdlib.h"
#define strtoull rcc_c_strtoull
unsigned long long strtoull(const char *, char **, int);
#define strtod rcc_c_strtod
double strtod(const char *, char **);
#endif
