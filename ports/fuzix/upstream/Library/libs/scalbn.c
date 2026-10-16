/* From MUSL */

#include <math.h>
#include "libm.h"

double scalbn(double x, int n)
{
	double scale;

	if (n > 1023) {
		x *= 8.98846567431158e+307;
		n -= 1023;
		if (n > 1023) {
			x *= 8.98846567431158e+307;
			n -= 1023;
			if (n > 1023) {
				x = (double)(x * 8.98846567431158e+307);
				return x;
			}
		}
	} else if (n < -1022) {
		x *= 2.2250738585072014e-308;
		n += 1022;
		if (n < -1022) {
			x *= 2.2250738585072014e-308;
			n += 1022;
			if (n < -1022) {
				x =(double)(x * 2.2250738585072014e-308);
				return x;
			}
		}
	}
	INSERT_WORDS(scale, (uint32_t)(0x3ff+n)<<20, 0);
	x = (double)(x * scale);
	return x;
}
