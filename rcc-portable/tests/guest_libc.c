#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    char buffer[160],small[4],*end;
    const char *bad="  +bad";
    unsigned long long value;
    errno=0;value=strtoull("18446744073709551615!",&end,10);
    if(value!=ULLONG_MAX || *end!='!' || errno)return 1;
    errno=0;value=strtoull("18446744073709551616x",&end,10);
    if(value!=ULLONG_MAX || *end!='x' || errno!=ERANGE)return 2;
    if(strtoull(" -1",&end,0)!=ULLONG_MAX || *end)return 3;
    if(strtoull("0Xabcdef",&end,0)!=0xabcdefULL || *end)return 4;
    if(strtoull("0777",&end,0)!=511ULL || *end)return 5;
    if(strtoull("z!",&end,36)!=35ULL || *end!='!')return 6;
    if(strtoull(bad,&end,10)!=0 || end!=bad)return 7;
    if(strtoull("0x",&end,0)!=0 || *end!='x')return 8;
    errno=0;if(strtoull("10",&end,1)!=0 || errno!=EINVAL)return 9;
    snprintf(buffer,sizeof(buffer),"%lld %llu %llx %lld %zu %g",
             LLONG_MIN,ULLONG_MAX,0xabcdefULL,17LL,sizeof("abc"),1.5);
    if(strcmp(buffer,"-9223372036854775808 18446744073709551615 abcdef 17 4 1.5"))return 10;
    if(snprintf(small,sizeof(small),"%llu",123456ULL)!=6 || strcmp(small,"123"))return 11;
    if(strtod("0x1p63!",&end)!=9223372036854775808.0 || *end!='!')return 12;
    if(strtod(" -0X1.8p+2f",&end)!=-6.0 || *end!='f')return 13;
    if(strtod("0x.8p-1",&end)!=0.25 || *end)return 14;
    if(strtod("0x1p+",&end)!=1.0 || *end!='p')return 15;
    if(strtod("1.25!",&end)!=1.25 || *end!='!')return 16;
    errno=0;if(strtod("0x1p-9999",&end)!=0.0 || errno!=ERANGE)return 17;
    {
        FILE *file=tmpfile(); int j, argument=0;
        if(!file)return 18;
        for(j=0;j<1025;j++)if(putc(argument++,file)!=j)return 19;
        if(argument!=1025 || fflush(file) || fseek(file,0,SEEK_SET))return 20;
        for(j=0;j<1025;j++){
            if(getc(file)!=j)return 21;
            if(j==255 && (ungetc(777,file)!=777 || getc(file)!=777))return 22;
        }
        if(getc(file)!=EOF || !feof(file))return 23;
        if(fseek(file,0,SEEK_SET) || getc(file)!=0)return 24;
        if(fseek(file,0,SEEK_SET) || setvbuf(file,NULL,_IOLBF,BUFSIZ))return 25;
        if(putc('X',file)!='X' || putc('\n',file)!='\n' || fseek(file,0,SEEK_SET))return 26;
        if(getc(file)!='X' || getc(file)!='\n' || fclose(file))return 27;
    }
    puts("portable guest libc checks passed");return 0;
}
