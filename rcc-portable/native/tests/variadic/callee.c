#include "api.h"
static int consume(va_list ap)
{
    int small = va_arg(ap, int);
    unsigned long word = va_arg(ap, unsigned long);
    unsigned long long wide = va_arg(ap, unsigned long long);
    double number = va_arg(ap, double);
    char *text = va_arg(ap, char *);
    Callback callback = va_arg(ap, Callback);
    unsigned last = va_arg(ap, unsigned);
    unsigned i;
    /* Force loop checkpoints while the list and incoming vararg area live. */
    for (i = 0; i < 17; i++) word += i;
    return small == -7 && word == 0x12345678UL + 136UL &&
           wide == 0xFEDCBA9876543210ULL && number == 1.5 &&
           text[0] == 'o' && text[1] == 'k' && callback(9UL) == 28UL && last == 511U;
}
int mixed(int tag, ...)
{
    va_list a, b;
    int first, second;
    va_start(a, tag);
    va_copy(b, a);
    first = consume(a);
    second = consume(b);
    va_end(b);
    va_end(a);
    return tag == 31 && first && second;
}
unsigned long recursive(unsigned depth, ...)
{
    va_list ap;
    unsigned long first, rest, last;
    va_start(ap, depth);
    first = va_arg(ap, unsigned long);
    rest = depth ? recursive(depth - 1, first + 1UL, first + 2UL) : 0UL;
    last = va_arg(ap, unsigned long);
    va_end(ap);
    return first + rest + last;
}
unsigned long many(unsigned n, ...)
{
    va_list ap;
    unsigned long total = 0;
    va_start(ap, n);
    while (n--) total += va_arg(ap, int);
    va_end(ap);
    return total;
}
int empty(unsigned tag, ...)
{
    va_list ap;
    va_start(ap, tag);
    va_end(ap);
    return tag == 23;
}
