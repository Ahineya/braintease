#ifndef DOOM_PORT_STDLIB_H
#define DOOM_PORT_STDLIB_H
#include "../../../runtime/include/stdlib.h"
#if defined(RIPPLE) && defined(DOOM_FAST_MEMORY)
#include "alloc_fast.h"
#define calloc Doom_Calloc
#endif
int atoi(const char *text);
long atol(const char *text);
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);
char *getenv(const char *name);
#endif
