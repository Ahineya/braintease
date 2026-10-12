#ifndef DOOM_PORT_STDLIB_H
#define DOOM_PORT_STDLIB_H
#include "../../../runtime/include/stdlib.h"
int atoi(const char *text);
long atol(const char *text);
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);
char *getenv(const char *name);
#endif
