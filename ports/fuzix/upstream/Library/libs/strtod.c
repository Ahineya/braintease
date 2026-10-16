#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

/*
 *	Sign
 */

const char *get_sign(register const char *p, unsigned *sign)
{
	*sign = 0;	/* +ve */
	if (*p == '+')
		return p + 1;
	if (*p != '-')
		return p;
	*sign = 1;
	return p + 1;
}

/*
 *	Signed hex value
 */

double hexnumber(const char **ptr, unsigned *err)
{
	register const char *p = *ptr;
	unsigned sign;
	unsigned prev = 0;
	double sum = 0;
	uint_fast8_t c;

	p = get_sign(p, &sign);
	if (!isdigit(*p))
		*err = 1;
	while(isxdigit(c = *p)) {
		sum *= 16;
		if (c >= '0' && c <= '9')
			sum += *p - '0';
		else if (c >= 'A' && c <= 'F')
			sum += *p - 'A' + 10;
		else if (c >= 'a' && c <= 'f')
			sum += *p - 'a' + 10;
		p++;
	}
	*ptr = p;
	/* Much faster than a multiply in most processors */
	if (sign)
		return -sum;
	return sum;
}

signed inumber(const char **ptr, unsigned *err)
{
	register const char *p = *ptr;
	unsigned sign;
	unsigned sum = 0;

	p = get_sign(p, &sign);
	if (!isdigit(*p))
		*err = 1;
	while(isdigit(*p)) {
		sum *= 10;
		sum += *p - '0';
		/* Exponent range is way below this in all cases */
		if (sum > 255)
			*err = 1;
		p++;
	}
	*ptr = p;
	/* Much faster than a multiply in most processors */
	if (sign)
		return -sum;
	return sum;
}

static double number(const char **ptr)
{
	register const char *p = *ptr;
	double working = 0.0;
	double scale = 10;

	while(isdigit(*p)) {
		working *= 10.0;
		working += *p - '0';
		p++;
	}
	*ptr = p;
	return working;
}

static double fraction(const char **ptr)
{
	register const char *p = *ptr;
	double working = 0.0;
	double scale = 0.1;

	while(isdigit(*p)) {
		working += (*p - '0') * scale;
		scale *= 0.1;
		p++;
	}
	*ptr = p;
	return working;
}

/* Ripple: C-locale decimal conversion, including .5 and exponents past 255.
 * Retain 17 significant digits so long mantissas cannot overflow before scaling. */
double __strtod(const char *text, char **endptr, unsigned *err)
{
    const char *p = text, *exponent_start;
    double value = 0.0, factor;
    int negative, digits = 0, kept = 0, adjust = 0, power = 0, exponent_negative;
    unsigned magnitude;
    while (isspace(*p)) p++;
    negative = *p == '-';
    if (*p == '+' || *p == '-') p++;
    if (!strncasecmp(p, "inf", 3)) {
        p += 3;
        if (!strncasecmp(p, "inity", 5)) p += 5;
        value = 1.0 / 0.0;
        goto done;
    }
    if (!strncasecmp(p, "nan", 3)) { p += 3; value = 0.0 / 0.0; goto done; }
    while (isdigit(*p)) {
        digits++;
        if (kept < 17) {
            value = value * 10.0 + (*p - '0');
            if (value != 0.0 || kept) kept++;
        } else adjust++;
        p++;
    }
    if (*p == '.') {
        p++;
        while (isdigit(*p)) {
            digits++;
            if (kept < 17) {
                value = value * 10.0 + (*p - '0');
                adjust--;
                if (value != 0.0 || kept) kept++;
            }
            p++;
        }
    }
    if (!digits) { if (endptr) *endptr = (char *)text; return 0.0; }
    if (*p == 'e' || *p == 'E') {
        exponent_start = p++;
        exponent_negative = *p == '-';
        if (*p == '+' || *p == '-') p++;
        if (!isdigit(*p)) p = exponent_start;
        else {
            while (isdigit(*p)) {
                if (power < 5000) power = power >= 500 ? 5000 : power * 10 + *p - '0';
                if (power > 5000) power = 5000;
                p++;
            }
            adjust += exponent_negative ? -power : power;
        }
    }
    magnitude = adjust < 0 ? -adjust : adjust;
    factor = 10.0;
    if (value != 0.0) {
        while (magnitude) {
            if (magnitude & 1) value = adjust < 0 ? value / factor : value * factor;
            magnitude >>= 1;
            if (magnitude) factor *= factor;
        }
        if (!isfinite(value) || value == 0.0) *err = 1;
    }
done:
    if (endptr) *endptr = (char *)p;
    return negative ? -value : value;
}

double strtod(const char *nptr, char **endptr)
{
	unsigned err = 0;
	double r = __strtod(nptr, endptr, &err);
	if (err)
		errno = ERANGE;
	return r;
}

/* Yes atof returns doubles, the C standard was apparently drunk that
   afternoon */

double atof(const char *nptr)
{
	unsigned err = 0;
	return __strtod(nptr, NULL, &err);
}

#ifdef TEST

#include <stdio.h>

int main(int argc, char *argv[])
{
	char buf[256];
	double d;

	while(fgets(buf, 255, stdin)) {
		errno = 0;
		d = strtod(buf, NULL);
		if (errno)
			printf("!");
		printf("%16.16e\n", d);
	}
	return 0;
}
#endif
