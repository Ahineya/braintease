/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AWK-only binary64 decimal formatting. Expand the exact binary rational in
 * decimal integers, then round to nearest, ties to even. No host FP shortcuts. */
#include <math.h>
#include <stdint.h>
static size_t float_text(char *out, double value, int precision, int conversion, int alternate) {
    union { double value; uint64_t bits; } u;
    uint64_t mantissa;
    char integer[800], digits[800], *p = out, *end;
    int exponent, power, point, n = 0, i, j, carry, total, count, keep, up, use_exp;
    u.value = value;
    exponent = (int)((u.bits >> 52) & 2047ULL);
    mantissa = u.bits & 0xfffffffffffffULL;
    if (exponent == 2047) {
        strcpy(out, mantissa ? "nan" : "inf");
        if (conversion == 'E' || conversion == 'G' || conversion == 'F')
            for (i = 0; out[i]; i++) out[i] -= 'a' - 'A';
        return 3;
    }
    if (exponent) { mantissa |= 1ULL << 52; power = exponent - 1075; }
    else power = -1074;
    if (!mantissa) { digits[0] = '0'; n = 1; point = 1; }
    else {
        do { integer[n++] = mantissa % 10ULL; mantissa /= 10ULL; } while (mantissa);
        count = power < 0 ? -power : power;
        for (i = 0; i < count; i++) {
            carry = 0;
            for (j = 0; j < n; j++) {
                total = integer[j] * (power < 0 ? 5 : 2) + carry;
                integer[j] = total % 10;
                carry = total / 10;
            }
            if (carry) integer[n++] = carry;
        }
        point = n + (power < 0 ? power : 0);
        for (i = 0; i < n; i++) digits[i] = integer[n-i-1] + '0';
        while (n > 1 && digits[n-1] == '0') n--;
    }
    if (precision < 0) precision = 6;
    if (precision > 30) precision = 30;
    if (conversion == 'g' || conversion == 'G') {
        if (!precision) precision = 1;
        keep = precision;
    } else if (conversion == 'e' || conversion == 'E') keep = precision + 1;
    else keep = point + precision;
    if (keep < n) {
        up = 0;
        if (keep >= 0) {
            up = digits[keep] > '5';
            if (digits[keep] == '5') {
                for (i = keep + 1; i < n; i++) if (digits[i] != '0') up = 1;
                if (keep && ((digits[keep-1] - '0') & 1)) up = 1;
            }
        }
        if (keep <= 0) {
            digits[0] = up ? '1' : '0'; n = 1;
            point = up ? 1 - precision : 1;
        } else {
            n = keep;
            if (up) {
                i = n - 1;
                while (i >= 0 && digits[i] == '9') digits[i--] = '0';
                if (i >= 0) digits[i]++;
                else { digits[0] = '1'; point++; }
            }
        }
    }
    if (conversion == 'g' || conversion == 'G') {
        use_exp = point > precision || point <= -4;
        count = precision;
    } else if (conversion == 'e' || conversion == 'E') {
        count = precision + 1; use_exp = 1;
    } else { count = n; use_exp = 0; }
    if (use_exp) {
        *p++ = digits[0];
        if (count > 1 || alternate) *p++ = '.';
        for (i = 1; i < count; i++) *p++ = i < n ? digits[i] : '0';
    } else {
        for (i = 0; i < point; i++) *p++ = i < n ? digits[i] : '0';
        if (point <= 0) *p++ = '0';
        count = conversion == 'g' || conversion == 'G' ? precision - point : precision;
        if (count > 0 || alternate) *p++ = '.';
        for (i = 0; i < count; i++) {
            int at = point + i;
            *p++ = at >= 0 && at < n ? digits[at] : '0';
        }
    }
    if ((conversion == 'g' || conversion == 'G') && !alternate) {
        *p = 0;
        if (strchr(out, '.')) {
            while (p > out && p[-1] == '0') p--;
            if (p > out && p[-1] == '.') p--;
        }
    }
    if (use_exp) {
        *p++ = conversion == 'E' || conversion == 'G' ? 'E' : 'e';
        exponent = point - 1;
        *p++ = exponent < 0 ? '-' : '+';
        if (exponent < 0) exponent = -exponent;
        end = p;
        do { *p++ = exponent % 10 + '0'; exponent /= 10; } while (exponent);
        if (p - end == 1) *p++ = '0';
        for (i = 0; i < (p - end) / 2; i++) {
            char c = end[i]; end[i] = p[-i - 1]; p[-i - 1] = c;
        }
    }
    *p = 0;
    return p - out;
}
