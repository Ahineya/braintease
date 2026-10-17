/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "../awk/awk.h"
int awk_getc(FILE *stream) {
    int c = fgetc(stream);
    if ((c == EOF && !feof(stream) && !ferror(stream)) ||
        (c != EOF && (unsigned)c > 255))
        FATAL("input contains a non-octet character");
    if (ferror(stream)) FATAL("input read error");
    return c;
}

int awk_octet(int value) {
    if ((unsigned)value > 255) FATAL("regex input contains a non-octet character");
    return value;
}
