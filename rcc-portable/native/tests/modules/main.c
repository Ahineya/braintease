#include "api.h"
static unsigned long helper(unsigned long n) { return n+1UL; }
int main(void)
{
    Operation operation = callback();
    if (counter != 11UL || *cursor != 7UL || samples[2] != 9UL) return 1;
    if (saved(2UL) != 102UL || operation(3UL) != 103UL) return 2;
    if (combine(helper,4UL) != 109UL || counter != 15UL) return 3;
    if (peer(2UL) != 37UL || helper(2UL) != 3UL) return 4;
    if (!even(8UL) || odd(8UL) || !odd(9UL)) return 5;
    if (config.value != 42UL || config.flag != 1234U || *config.pointer != 9UL) return 6;
    if (greeting[0] != 'h' || greeting[1] != 0x1234 || greeting[2]) return 7;
    *cursor = 19UL;
    if (samples[1] != 19UL) return 8;
    if (square(2.5) != 8.25 || gain != 2.0) return 9;
    return 0;
}
