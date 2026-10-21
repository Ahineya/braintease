/* SPDX-License-Identifier: MIT */
#ifndef LVCALC_MODEL_H
#define LVCALC_MODEL_H
/* Bounded arithmetic expression: decimal/exponent literals, unary signs,
 * parentheses and the four basic operations. Returns NULL on success. */
const char *calc_evaluate(const char *expression, double *value);
#endif
