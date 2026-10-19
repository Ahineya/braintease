/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Prompt escapes shared by the Ripple and hosted V7 shells. */
#include "shell-prompt.h"
#include <unistd.h>
#include <string.h>
#include <pwd.h>
#include <utsname.h>
#include <time.h>
#include <errno.h>
#include "../../../runtime/include/ripple_unicode.h"

#define PROMPT_SIZE 1024
static char rendered[PROMPT_SIZE], invisible[PROMPT_SIZE], cwd[512];
static unsigned length, hidden;

static void character(unsigned c) {
    /* A NUL escape cannot occur inside a C prompt string. Ignore it. */
    if(c && length+1<PROMPT_SIZE) {
        rendered[length]=c;
        invisible[length++]=hidden;
    }
}
static void string(const char *s) {
    if(s) while(*s) character((unsigned char)*s++);
}
static void decimal(unsigned n) {
    char digits[8];
    unsigned count=0;
    do {digits[count++]='0'+n%10;n/=10;} while(n);
    while(count) character(digits[--count]);
}
static void two(unsigned n) {character('0'+n/10);character('0'+n%10);}
static void directory(const char *home, int basename) {
    const char *path=cwd, *last;
    unsigned n=home ? strlen(home) : 0;
    int inside;
    while(n>1 && home[n-1]=='/')n--;
    inside=n && !strncmp(path,home,n) && (!path[n] || path[n]=='/');
    if(basename) {
        if(inside && !path[n])string("~");
        else {last=strrchr(path,'/');string(last && last[1] ? last+1 : path);}
    } else if(inside) {
        character('~');string(path+n);
    } else {
        string(path);
    }
}

const char *shell_prompt_expand(const char *specification, const char *home) {
    int saved_errno=errno, have_cwd=0, have_host=0, have_time=0;
    struct utsname host;
    struct tm clock;
    time_t now;
    const char *p=specification ? specification : "";
    length=hidden=0;
    while(*p && length+1<PROMPT_SIZE) {
        unsigned c=(unsigned char)*p++;
        if(c!='\\' || !*p) {character(c);continue;}
        c=(unsigned char)*p++;
        switch(c) {
        case '[': hidden=1;break;
        case ']': hidden=0;break;
        case '\\': character('\\');break;
        case 'a': character(7);break;
        case 'e': case 'E': character(27);break;
        case 'n': character('\n');break;
        case 'r': character('\r');break;
        case '$': character(geteuid()==0 ? '#' : '$');break;
        case 's': string("sh");break;
        case 'u': {
            unsigned uid=getuid();
            struct passwd *user=getpwuid(uid);
            if(user && user->pw_name) string(user->pw_name);
            else decimal(uid);
            break;
        }
        case 'h': case 'H': {
            const char *name;
            if(!have_host) {
                memset(&host,0,sizeof(host));
                if(uname(&host)<0) strcpy(host.nodename,"?");
                host.nodename[sizeof(host.nodename)-1]=0;
                have_host=1;
            }
            name=host.nodename;
            while(*name && (c=='H' || *name!='.')) character((unsigned char)*name++);
            break;
        }
        case 'w': case 'W':
            if(!have_cwd) {
                if(!getcwd(cwd,sizeof(cwd))) strcpy(cwd,"?");
                have_cwd=1;
            }
            directory(home,c=='W');break;
        case 'd': case 't': case 'T': case '@': case 'A':
            if(!have_time) {
                memset(&clock,0,sizeof(clock));
                if(time(&now)!=(time_t)-1) localtime_r(&now,&clock);
                have_time=1;
            }
            if(c=='d') {
                const char *week="SunMonTueWedThuFriSat", *month="JanFebMarAprMayJunJulAugSepOctNovDec";
                unsigned i;
                for(i=0;i<3;i++) character(week[(clock.tm_wday%7)*3+i]);
                character(' ');
                for(i=0;i<3;i++) character(month[(clock.tm_mon%12)*3+i]);
                character(' ');two(clock.tm_mday);
            } else {
                unsigned hour=clock.tm_hour;
                if(c=='T' || c=='@') {hour%=12;if(!hour)hour=12;}
                two(hour);character(':');two(clock.tm_min);
                if(c=='t' || c=='T') {character(':');two(clock.tm_sec);}
                if(c=='@') string(clock.tm_hour<12 ? " AM" : " PM");
            }
            break;
        default:
            if(c>='0' && c<='7') {
                unsigned value=c-'0',count=1;
                while(count<3 && *p>='0' && *p<='7') {value=value*8+*p++-'0';count++;}
                character(value&255);
            } else {character('\\');character(c);}
        }
    }
    rendered[length]=0;
    errno=saved_errno;
    return rendered;
}

/* Measure the final logical prompt line. ANSI styling and OSC titles do not
 * occupy cells; explicit \[...\] also handles other terminal sequences. */
static unsigned geometry(const char *text, const char **tail) {
    unsigned i,column=0,state=0,formatted=text==rendered;
    *tail=text;
    for(i=0;text[i];i++) {
        unsigned c=(unsigned char)text[i];
        if(formatted && invisible[i]) continue;
        if(state==1) {
            state=c=='[' ? 2 : c==']' ? 3 : (c=='(' || c==')') ? 5 : 0;
        } else if(state==2) {
            if(c>='@' && c<='~') state=0;
        } else if(state==3) {
            if(c==7)state=0;
            else if(c==27)state=4;
        } else if(state==4) {
            state=c=='\\' ? 0 : c==27 ? 4 : 3;
        } else if(state==5) state=0;
        else if(c==27)state=1;
        else if(c=='\n' || c=='\r') {column=0;*tail=text+i+1;}
        else if(c=='\t')column+=8-column%8;
        else if(c=='\b') {if(column)column--;}
        else if(c>=32 && c!=127) {
            unsigned start=i;
            do {i++;c=(unsigned char)text[i];}
            while(c>=32 && c!=127 && !(formatted && invisible[i]));
            column+=ru_columns((const unsigned char *)text+start,i-start);
            i--;
        }
    }
    return column;
}
unsigned shell_prompt_columns(const char *text) {
    const char *tail;
    return geometry(text,&tail);
}
const char *shell_prompt_tail(const char *text) {
    const char *tail;
    geometry(text,&tail);
    return tail;
}
