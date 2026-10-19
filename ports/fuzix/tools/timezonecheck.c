/* Deterministic timezone vectors, independent of the guest wall clock. */
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int check(char *setting,time_t value,int year,int month,int day,int hour,int minute,int second,int dst,char *name) {
    struct tm t;int saved_errno=EDOM;
    if(putenv(setting))return 1;
    errno=saved_errno;localtime_r(&value,&t);
    if(errno!=saved_errno||t.tm_year!=year-1900||t.tm_mon!=month-1||t.tm_mday!=day||t.tm_hour!=hour||
       t.tm_min!=minute||t.tm_sec!=second||t.tm_isdst!=dst||strcmp(tzname[dst],name)){
        printf("FAIL %s %lu: %d-%d-%d %d:%d:%d dst=%d %s\n",setting,(unsigned long)value,
            t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec,t.tm_isdst,tzname[t.tm_isdst]);return 1;
    }
    t.tm_isdst=1;gmtime_r(&value,&t);if(t.tm_isdst)return 1;
    return 0;
}
int main(int argc,char **argv) {
    time_t now;struct tm t;int failed=0;
    if(argc==2&&!strcmp(argv[1],"--now")){
        time(&now);localtime_r(&now,&t);printf("LOCAL_CLOCK %02d:%02d %s\n",t.tm_hour,t.tm_min,tzname[t.tm_isdst]);return 0;
    }
    failed+=check("TZ=CET-1CEST,M3.5.0/2,M10.5.0/3",1767226500UL,2026,1,1,1,15,0,0,"CET");
    failed+=check("TZ=CET-1CEST,M3.5.0/2,M10.5.0/3",1782907200UL,2026,7,1,14,0,0,1,"CEST");
    failed+=check("TZ=CET-1CEST,M3.5.0/2,M10.5.0/3",1774745999UL,2026,3,29,1,59,59,0,"CET");
    failed+=check("TZ=CET-1CEST,M3.5.0/2,M10.5.0/3",1774746000UL,2026,3,29,3,0,0,1,"CEST");
    failed+=check("TZ=CET-1CEST,M3.5.0/2,M10.5.0/3",1792889999UL,2026,10,25,2,59,59,1,"CEST");
    failed+=check("TZ=CET-1CEST,M3.5.0/2,M10.5.0/3",1792890000UL,2026,10,25,2,0,0,0,"CET");
    failed+=check("TZ=CET-1CEST,M3.5.0/2,M10.5.0/3",1772953199UL,2026,3,8,7,59,59,0,"CET");
    failed+=check("TZ=CET-1CEST,M3.5.0/2,M10.5.0/3",1772953200UL,2026,3,8,8,0,0,0,"CET");
    failed+=check("TZ=CET-1CEST,M3.5.0/2,M10.5.0/3",1793512799UL,2026,11,1,6,59,59,0,"CET");
    failed+=check("TZ=CET-1CEST,M3.5.0/2,M10.5.0/3",1793512800UL,2026,11,1,7,0,0,0,"CET");
    failed+=check("TZ=CET-1CEST,M3.5.0/2,M10.5.0/3",1775318399UL,2026,4,4,17,59,59,1,"CEST");
    failed+=check("TZ=CET-1CEST,M3.5.0/2,M10.5.0/3",1775318400UL,2026,4,4,18,0,0,1,"CEST");
    failed+=check("TZ=CET-1CEST,M3.5.0/2,M10.5.0/3",1791043199UL,2026,10,3,17,59,59,1,"CEST");
    failed+=check("TZ=CET-1CEST,M3.5.0/2,M10.5.0/3",1791043200UL,2026,10,3,18,0,0,1,"CEST");
    failed+=check("TZ=EST5EDT,M3.2.0,M11.1.0",1767226500UL,2025,12,31,19,15,0,0,"EST");
    failed+=check("TZ=EST5EDT,M3.2.0,M11.1.0",1782907200UL,2026,7,1,8,0,0,1,"EDT");
    failed+=check("TZ=EST5EDT,M3.2.0,M11.1.0",1774745999UL,2026,3,28,20,59,59,1,"EDT");
    failed+=check("TZ=EST5EDT,M3.2.0,M11.1.0",1774746000UL,2026,3,28,21,0,0,1,"EDT");
    failed+=check("TZ=EST5EDT,M3.2.0,M11.1.0",1792889999UL,2026,10,24,20,59,59,1,"EDT");
    failed+=check("TZ=EST5EDT,M3.2.0,M11.1.0",1792890000UL,2026,10,24,21,0,0,1,"EDT");
    failed+=check("TZ=EST5EDT,M3.2.0,M11.1.0",1772953199UL,2026,3,8,1,59,59,0,"EST");
    failed+=check("TZ=EST5EDT,M3.2.0,M11.1.0",1772953200UL,2026,3,8,3,0,0,1,"EDT");
    failed+=check("TZ=EST5EDT,M3.2.0,M11.1.0",1793512799UL,2026,11,1,1,59,59,1,"EDT");
    failed+=check("TZ=EST5EDT,M3.2.0,M11.1.0",1793512800UL,2026,11,1,1,0,0,0,"EST");
    failed+=check("TZ=EST5EDT,M3.2.0,M11.1.0",1775318399UL,2026,4,4,11,59,59,1,"EDT");
    failed+=check("TZ=EST5EDT,M3.2.0,M11.1.0",1775318400UL,2026,4,4,12,0,0,1,"EDT");
    failed+=check("TZ=EST5EDT,M3.2.0,M11.1.0",1791043199UL,2026,10,3,11,59,59,1,"EDT");
    failed+=check("TZ=EST5EDT,M3.2.0,M11.1.0",1791043200UL,2026,10,3,12,0,0,1,"EDT");
    failed+=check("TZ=AEST-10AEDT,M10.1.0,M4.1.0/3",1767226500UL,2026,1,1,11,15,0,1,"AEDT");
    failed+=check("TZ=AEST-10AEDT,M10.1.0,M4.1.0/3",1782907200UL,2026,7,1,22,0,0,0,"AEST");
    failed+=check("TZ=AEST-10AEDT,M10.1.0,M4.1.0/3",1774745999UL,2026,3,29,11,59,59,1,"AEDT");
    failed+=check("TZ=AEST-10AEDT,M10.1.0,M4.1.0/3",1774746000UL,2026,3,29,12,0,0,1,"AEDT");
    failed+=check("TZ=AEST-10AEDT,M10.1.0,M4.1.0/3",1792889999UL,2026,10,25,11,59,59,1,"AEDT");
    failed+=check("TZ=AEST-10AEDT,M10.1.0,M4.1.0/3",1792890000UL,2026,10,25,12,0,0,1,"AEDT");
    failed+=check("TZ=AEST-10AEDT,M10.1.0,M4.1.0/3",1772953199UL,2026,3,8,17,59,59,1,"AEDT");
    failed+=check("TZ=AEST-10AEDT,M10.1.0,M4.1.0/3",1772953200UL,2026,3,8,18,0,0,1,"AEDT");
    failed+=check("TZ=AEST-10AEDT,M10.1.0,M4.1.0/3",1793512799UL,2026,11,1,16,59,59,1,"AEDT");
    failed+=check("TZ=AEST-10AEDT,M10.1.0,M4.1.0/3",1793512800UL,2026,11,1,17,0,0,1,"AEDT");
    failed+=check("TZ=AEST-10AEDT,M10.1.0,M4.1.0/3",1775318399UL,2026,4,5,2,59,59,1,"AEDT");
    failed+=check("TZ=AEST-10AEDT,M10.1.0,M4.1.0/3",1775318400UL,2026,4,5,2,0,0,0,"AEST");
    failed+=check("TZ=AEST-10AEDT,M10.1.0,M4.1.0/3",1791043199UL,2026,10,4,1,59,59,0,"AEST");
    failed+=check("TZ=AEST-10AEDT,M10.1.0,M4.1.0/3",1791043200UL,2026,10,4,3,0,0,1,"AEDT");
    failed+=check("TZ=NPT-5:45",1798750800UL,2027,1,1,2,45,0,0,"NPT");
    failed+=check("TZ=<+0545>-5:45",1767225600UL,2026,1,1,5,45,0,0,"+0545");
    failed+=check("TZ=XST3:30:15",1767232800UL,2025,12,31,22,29,45,0,"XST");
    failed+=check("TZ=EST5",0UL,1969,12,31,19,0,0,0,"EST");
    failed+=check("TZ=CET-1",4107542400UL,2100,3,1,1,0,0,0,"CET");
    failed+=check("TZ=CET-1",4294967294UL,2106,2,7,7,28,14,0,"CET");
    failed+=check("TZ=STD0DST,J60/0,J300/0",1709208000UL,2024,2,29,12,0,0,0,"STD");
    failed+=check("TZ=STD0DST,J60/0,J300/0",1709251200UL,2024,3,1,1,0,0,1,"DST");
    failed+=check("TZ=STD0DST,59/0,300/0",1709164800UL,2024,2,29,1,0,0,1,"DST");
    failed+=check("TZ=STD0DST,59/0,300/0",1740744000UL,2025,2,28,12,0,0,0,"STD");
    failed+=check("TZ=STD0DST,59/0,300/0",1740787200UL,2025,3,1,1,0,0,1,"DST");
    failed+=check("TZ=UTC",1782907200UL,2026,7,1,12,0,0,0,"UTC");
    failed+=check("TZ=",1782907200UL,2026,7,1,12,0,0,0,"UTC");
    failed+=check("TZ=Europe/Tirane",1782907200UL,2026,7,1,12,0,0,0,"UTC");
    failed+=check("TZ=CET-1CEST",1782907200UL,2026,7,1,12,0,0,0,"UTC");
    failed+=check("TZ=CET999999999999999",1782907200UL,2026,7,1,12,0,0,0,"UTC");
    failed+=check("TZ=CET-1CEST,M13.5.0,M10.5.0",1782907200UL,2026,7,1,12,0,0,0,"UTC");
    failed+=check("TZ=CET-1junk!",1782907200UL,2026,7,1,12,0,0,0,"UTC");
    if(failed){printf("TIMEZONE_FAILED %d\n",failed);return 1;}
    puts("TIMEZONE_OK 60 vectors");return 0;
}
