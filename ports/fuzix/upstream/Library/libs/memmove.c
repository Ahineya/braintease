/* From dLibs 1.20 but ANSIfied */

#include <types.h>

void *memmove(void *dest, const void *src, size_t len)
{
	register uint8_t *dp = dest;
	register const uint8_t *sp = src;

	if (sp < dp) {
		dp += len;
		sp += len;
		while(len--)
			*--dp = *--sp;
	} else {
		while(len--)
			*dp++ = *sp++;
	}
	return dest;
}
