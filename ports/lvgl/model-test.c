/* SPDX-License-Identifier: MIT */
#include "calc.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    const char *good[] = {"2+3*4", "(2+3)*4", "-(-3.5)/2", "1e3+0.25", "1-2-3",
                          "8/2/2", "--2",     "2*-3",      "0.1+0.2"};
    double expected[] = {14, 20, 1.75, 1000.25, -4, 2, 2, -6, 0.3};
    const char *bad[] = {"", ".", "2+", "(1+2", "1/0", "1e999", "1e308*10", "2foo", "2 3", "1)"};
    unsigned i;
    double v;
    const char *error;
    char nested[100];
    for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        error = calc_evaluate(good[i], &v);
        if (error || v - expected[i] > 0.0000001 || expected[i] - v > 0.0000001) {
            printf("BAD %s\n", good[i]);
            return 1;
        }
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (!calc_evaluate(bad[i], &v)) {
            printf("BAD accepted %s\n", bad[i]);
            return 1;
        }
    memset(nested, '(', 30);
    nested[30] = '1';
    memset(nested + 31, ')', 30);
    nested[61] = 0;
    if (!calc_evaluate(nested, &v))
        return 1;
    puts(
        "CALC_MODEL passed precedence, decimal, exponents, unary signs, errors and nesting bounds");
    return 0;
}
