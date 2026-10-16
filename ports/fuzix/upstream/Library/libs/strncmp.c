#include	<string.h>

/* ANSIfied from dLibs 1.2 */

int strncmp(register const char *str1, register const char *str2, register size_t limit)
{
	/* strncmp is not defined for a zero length but having our defined
	   behaviour as 0 is compliant and makes some of our internal users
	   compliant with their own definitions */
	if (!limit)
		return 0;
	for(; ((--limit) && (*str1 == *str2)); ++str1, ++str2)
		if (*str1 == '\0')
			return 0;
	return (unsigned char)*str1 - (unsigned char)*str2;
}
