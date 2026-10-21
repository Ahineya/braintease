/* SPDX-License-Identifier: MIT */
#include "calc.h"
#include <stdlib.h>
#include <float.h>
#include <ctype.h>
#include <string.h>
struct parser {
    const char *p;
    const char *error;
    unsigned depth;
};
static double sum(struct parser *p);
static void spaces(struct parser *p) {
    while (*p->p == ' ' || *p->p == '\t')
        p->p++;
}
static double checked(struct parser *p, double v) {
    if (v != v || v > DBL_MAX || v < -DBL_MAX)
        p->error = "Number out of range";
    return v;
}
static double atom(struct parser *p) {
    double v = 0;
    char *end;
    int negative = 0;
    spaces(p);
    while (*p->p == '+' || *p->p == '-') {
        if (*p->p == '-')
            negative = !negative;
        p->p++;
        spaces(p);
    }
    if (*p->p == '(') {
        if (++p->depth > 24) {
            p->error = "Too many parentheses";
            return 0;
        }
        p->p++;
        v = sum(p);
        spaces(p);
        if (*p->p != ')')
            p->error = "Missing closing parenthesis";
        else
            p->p++;
        p->depth--;
    } else {
        if (!isdigit((unsigned char)*p->p) && *p->p != '.') {
            p->error = "Expected a number";
            return 0;
        }
        v = strtod(p->p, &end);
        if (end == p->p) {
            p->error = "Expected a number";
            return 0;
        }
        p->p = end;
    }
    return checked(p, negative ? -v : v);
}
static double product(struct parser *p) {
    double v = atom(p), r;
    char op;
    while (!p->error) {
        spaces(p);
        op = *p->p;
        if (op != '*' && op != '/')
            break;
        p->p++;
        r = atom(p);
        if (p->error)
            break;
        if (op == '/' && r == 0) {
            p->error = "Cannot divide by zero";
            break;
        }
        v = checked(p, op == '*' ? v * r : v / r);
    }
    return v;
}
static double sum(struct parser *p) {
    double v = product(p), r;
    char op;
    while (!p->error) {
        spaces(p);
        op = *p->p;
        if (op != '+' && op != '-')
            break;
        p->p++;
        r = product(p);
        if (p->error)
            break;
        v = checked(p, op == '+' ? v + r : v - r);
    }
    return v;
}
const char *calc_evaluate(const char *expression, double *value) {
    struct parser p;
    if (strlen(expression) > 128)
        return "Expression is too long";
    p.p = expression;
    p.error = NULL;
    p.depth = 0;
    *value = sum(&p);
    spaces(&p);
    if (!p.error && *p.p)
        p.error = "Unexpected character";
    return p.error;
}
