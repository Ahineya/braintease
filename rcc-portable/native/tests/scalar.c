/* C16 wraparound, private logical memory, checked GEP, nested resumable calls
 * and a loop large enough to request timer checkpoints under x64 TCG. */
#include "probe.h"
static unsigned long sum(unsigned long n)
{
    if (!n) return PROBE_BASE;
    return sum(n-1UL)+n;
}
static unsigned long twice(unsigned long n) { return PROBE_DOUBLE(n); }
int main(void)
{
    volatile unsigned long values[2];
    unsigned long i, total=0;
    unsigned int wrap=65535U;
    values[0]=3UL; values[1]=9UL;
    for (i=0;i<2UL;i++) total+=twice(values[i]);
    if (total!=24UL || sum(8UL)!=43UL) return 1;
    if ((unsigned int)(wrap+1U)!=0U) return 2;
    for (i=0;i<20000UL;i++) total+=i&3UL;
    return total!=30024UL;
}
