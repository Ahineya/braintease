#include "api.h"
/* Same names, different private definitions in another translation unit. */
static unsigned long private_counter = 31UL;
static unsigned long helper(unsigned long n) { return n*3UL+private_counter; }
unsigned long peer(unsigned long n) { return helper(n); }
unsigned long odd(unsigned long n) { return n ? even(n-1UL) : 0UL; }
