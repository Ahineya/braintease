/* English bitmap-font locale, until guest gettext catalogs are available. */
#include <stdio.h>
#include "localize.h"
int locale_load_domain(const char *domain, const char *translations)
{ (void)domain; (void)translations; return 1; }
void locale_init(const char *lang) { (void)lang; }
void locale_write_ordinal_number(char *buf, unsigned n, int number)
{
    int last=number%100;
    const char *suffix="th";
    if (last<11 || last>13) {
        if (number%10==1) suffix="st";
        else if (number%10==2) suffix="nd";
        else if (number%10==3) suffix="rd";
    }
    snprintf(buf,n,"%d%s",number,suffix);
}
