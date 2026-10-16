#include <string.h>

char *strcpy(char *dp, register const char *s)
{
	register char *d = dp;
	while(*d++ = *s++);
	return dp;
}
