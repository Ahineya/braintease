/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Small, database-free TZ implementation shared by Ripple and Fuse-X.
 * TZ overrides /etc/TZ. Rules repeat annually; historical IANA zone files are
 * deliberately outside this format. Invalid settings safely select UTC. */
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

struct tz_rule { int kind, day, month, week, weekday; long seconds; };
struct tz_config {
    char names[2][32];
    long west[2];
    int dst;
    struct tz_rule start, end;
};
static struct tz_config zone;
static char standard_name[32]="UTC", summer_name[32]="UTC";
char *tzname[2]={standard_name,summer_name};
long timezone;
int daylight;

static int tz_number(const char **text,int low,int high,int *out) {
    const char *p=*text;int n=0;
    if(*p<'0'||*p>'9')return 0;
    do {
        n=n*10+*p++-'0';
        if(n>high)return 0;
    }while(*p>='0'&&*p<='9');
    if(n<low)return 0;
    *out=n;*text=p;return 1;
}
static int tz_name(const char **text,char *out) {
    const char *p=*text;unsigned n=0;int quoted=*p=='<';
    if(quoted)p++;
    while((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||
          (quoted&&((*p>='0'&&*p<='9')||*p=='+'||*p=='-'))) {
        if(n==31)return 0;
        out[n++]=*p++;
    }
    if(n<3||(quoted&&*p++!='>'))return 0;
    out[n]=0;*text=p;return 1;
}
static int tz_offset(const char **text,int maximum,long *out) {
    const char *p=*text;int sign=1,h,m=0,s=0;
    if(*p=='-'||*p=='+'){if(*p=='-')sign=-1;p++;}
    if(!tz_number(&p,0,maximum,&h))return 0;
    if(*p==':'){
        p++;if(!tz_number(&p,0,59,&m))return 0;
        if(*p==':'){p++;if(!tz_number(&p,0,59,&s))return 0;}
    }
    *out=sign*(h*3600L+m*60L+s);*text=p;return 1;
}
static int tz_rule(const char **text,struct tz_rule *r) {
    const char *p=*text;
    r->seconds=7200;
    if(*p=='M'){
        r->kind='M';p++;
        if(!tz_number(&p,1,12,&r->month)||*p++!='.'||
           !tz_number(&p,1,5,&r->week)||*p++!='.'||!tz_number(&p,0,6,&r->weekday))return 0;
    }else{
        r->kind=*p=='J'?'J':'N';if(r->kind=='J')p++;
        if(!tz_number(&p,r->kind=='J'?1:0,365,&r->day))return 0;
    }
    if(*p=='/'){p++;if(!tz_offset(&p,167,&r->seconds))return 0;}
    *text=p;return 1;
}
static int tz_parse(const char *p,struct tz_config *z) {
    memset(z,0,sizeof(*z));
    if(!*p){strcpy(z->names[0],"UTC");strcpy(z->names[1],"UTC");return 1;}
    if(!tz_name(&p,z->names[0]))return 0;
    /* Bare UTC and GMT are useful, unambiguous aliases for offset zero. */
    if(!*p&&(!strcmp(z->names[0],"UTC")||!strcmp(z->names[0],"GMT"))){
        strcpy(z->names[1],z->names[0]);return 1;
    }
    if(!tz_offset(&p,24,&z->west[0]))return 0;
    strcpy(z->names[1],z->names[0]);
    if(!*p)return 1;
    if(!tz_name(&p,z->names[1]))return 0;
    z->west[1]=z->west[0]-3600;
    if(*p!=','&&!tz_offset(&p,24,&z->west[1]))return 0;
    /* No guessed DST rules: callers must give both annual transitions. */
    if(*p++!=','||!tz_rule(&p,&z->start)||*p++!=','||!tz_rule(&p,&z->end)||*p)return 0;
    z->dst=1;return 1;
}
void tzset(void) {
    int saved_errno=errno;const char *text=getenv("TZ");
    char file_text[256];struct tz_config parsed;FILE *f;
    if(!text){
        text="";f=fopen("/etc/TZ","r");
        if(f){
            if(fgets(file_text,sizeof(file_text),f)){
                unsigned n=strlen(file_text);
                /* Reject a truncated rule rather than accepting its prefix. */
                if(n&&file_text[n-1]!='\n'&&!feof(f)){
                    int c=fgetc(f);if(c!=EOF)file_text[0]=0;
                }
                file_text[strcspn(file_text,"\r\n")]=0;text=file_text;
            }
            fclose(f);
        }
    }
    if(!tz_parse(text,&parsed))tz_parse("",&parsed);
    zone=parsed;timezone=zone.west[0];daylight=zone.dst;
    strcpy(standard_name,zone.names[0]);strcpy(summer_name,zone.names[1]);
    errno=saved_errno;
}
static int tz_month_days(int year,int month) {
    static const unsigned char days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month-1]+(month==2&&__isleap(year));
}
static long tz_transition(struct tz_rule *r,struct tm *standard,long west) {
    int year=standard->tm_year+1900,day=r->day,month,first,jan;
    if(r->kind=='J'){
        day--;if(__isleap(year)&&r->day>=60)day++;
    }else if(r->kind=='M'){
        day=0;for(month=1;month<r->month;month++)day+=tz_month_days(year,month);
        jan=(standard->tm_wday-standard->tm_yday%7+7)%7;
        first=(jan+day)%7;
        first=(r->weekday-first+7)%7+1+(r->week-1)*7;
        if(first>tz_month_days(year,r->month))first-=7;
        day+=first-1;
    }
    /* Seconds from this year's UTC Jan 1, without overflowing a signed epoch. */
    return day*86400L+r->seconds+west;
}
static void tz_convert(time_t value,long west,struct tm *result) {
    long days=value/86400UL,rem=(long)(value%86400UL)-west;
    while(rem<0){rem+=86400;days--;}
    while(rem>=86400){rem-=86400;days++;}
    __compute_tm(result,days,rem);result->tm_isdst=0;
}
struct tm *localtime_r(const time_t *value,struct tm *result) {
    long now,start,end;int dst=0;
    tzset();tz_convert(*value,zone.west[0],result);
    if(zone.dst){
        now=result->tm_yday*86400L+result->tm_hour*3600L+result->tm_min*60L+result->tm_sec+zone.west[0];
        start=tz_transition(&zone.start,result,zone.west[0]);
        end=tz_transition(&zone.end,result,zone.west[1]);
        dst=start<end?(now>=start&&now<end):(now>=start||now<end);
        if(dst)tz_convert(*value,zone.west[1],result);
    }
    result->tm_isdst=dst;return result;
}
struct tm *localtime(const time_t *value) {
    static struct tm result;
    return localtime_r(value,&result);
}
