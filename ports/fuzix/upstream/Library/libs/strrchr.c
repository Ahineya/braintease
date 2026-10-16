#include <string.h>

char *strrchr(register const char *s, int c)
{
	register char ch = c;
	register const char *p = NULL;
	do {
		if (*s == ch)
			p = s;
	} while(*s++);
	return p;
}

