/*
 *	BSD has an API for this so we might as well provide a libc API
 *	using the same interface. Pity the API varies between systems on
 *	the type used for loadavg!
 */

#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <syscalls.h>

int getloadavg(unsigned int loadavg[], int nelem)
{
	struct _uzisysinfoblk uts;
	uint8_t i;
	int bytes = _uname(&uts, sizeof(uts));

	if (nelem < 0) { errno = EINVAL; return -1; }
	if (bytes < sizeof(uts))
		return -1;
	if (nelem > 3)
		nelem = 3;
	for (i = 0; i < nelem; i++)
		loadavg[i] = ((unsigned long)uts.loadavg[i] * 100UL) >> 8;
	return nelem;
}
