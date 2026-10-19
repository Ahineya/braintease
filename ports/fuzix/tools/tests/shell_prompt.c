/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Deterministic host-side checks of the allocation-free prompt formatter. */
#include "shell-prompt.h"
#include <assert.h>
#include <errno.h>
#include <pwd.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utsname.h>
static char path[512]="/home/tester/work";
static uid_t user_id=501, effective_id=501;
static int found_user=1;
uid_t getuid(void) {return user_id;}
uid_t geteuid(void) {return effective_id;}
struct passwd *getpwuid(uid_t uid) {
    static struct passwd entry;
    (void)uid;entry.pw_name="tester";errno=EIO;
    return found_user ? &entry : 0;
}
char *getcwd(char *buffer,size_t size) {
    if(strlen(path)>=size) {errno=ERANGE;return 0;}
    strcpy(buffer,path);return buffer;
}
int uname(struct utsname *host) {strcpy(host->nodename,"ripple.test");return 0;}
time_t time(time_t *t) {if(t)*t=0;return 0;}
struct tm *localtime_r(const time_t *now,struct tm *clock) {
    (void)now;memset(clock,0,sizeof(*clock));clock->tm_hour=13;clock->tm_min=5;
    clock->tm_sec=9;clock->tm_wday=4;clock->tm_mday=1;return clock;
}
int main(void) {
    const char *s;
    char large[3072];
    errno=123;
    assert(!strcmp(shell_prompt_expand("\\u@\\h|\\H|\\s|\\$",0),"tester@ripple|ripple.test|sh|$"));
    assert(errno==123);
    effective_id=0;assert(!strcmp(shell_prompt_expand("\\$",0),"#"));
    found_user=0;user_id=77;assert(!strcmp(shell_prompt_expand("\\u",0),"77"));found_user=1;
    assert(!strcmp(shell_prompt_expand("\\w|\\W","/home/tester"),"~/work|work"));
    assert(!strcmp(shell_prompt_expand("\\w|\\W","/home/tester/"),"~/work|work"));
    strcpy(path,"/home/tester");assert(!strcmp(shell_prompt_expand("\\w|\\W",path),"~|~"));
    strcpy(path,"/home/tester2");assert(!strcmp(shell_prompt_expand("\\w","/home/tester"),path));
    strcpy(path,"/");assert(!strcmp(shell_prompt_expand("\\w|\\W",0),"/|/"));
    assert(!strcmp(shell_prompt_expand("\\w|\\W","/"),"~|~"));
    strcpy(path,"/`touch SHOULD_NOT_RUN`");assert(!strcmp(shell_prompt_expand("\\w",0),path));
    assert(!strcmp(shell_prompt_expand("\\t|\\T|\\@|\\A|\\d",0),"13:05:09|01:05:09|01:05 PM|13:05|Thu Jan 01"));
    assert(!strcmp(shell_prompt_expand("\\101\\033[0m\\\\\\q\\",0),"A\033[0m\\\\q\\"));
    s=shell_prompt_expand("first\\n\\[\\e[32m\\]abc\\[\\e[0m\\]> ",0);
    assert(!strcmp(s,"first\n\033[32mabc\033[0m> "));
    assert(shell_prompt_columns(s)==5);
    assert(!strcmp(shell_prompt_tail(s),"\033[32mabc\033[0m> "));
    s=shell_prompt_expand("a\\[hidden\\]b",0);assert(shell_prompt_columns(s)==2);
    s=shell_prompt_expand("\\e]0;title\\a> ",0);assert(shell_prompt_columns(s)==2);
    s=shell_prompt_expand("\\e]0;title\\e\\\\> ",0);assert(shell_prompt_columns(s)==2);
    assert(shell_prompt_columns("a\t> ")==10);
    assert(shell_prompt_columns("long\r> ")==2);
    assert(shell_prompt_columns("abc\b")==2);
    assert(shell_prompt_columns("Жé中🙂> ")==8);
    s=shell_prompt_expand("first\\n\\[\\e[32m\\]Жé中🙂\\[\\e[0m\\]> ",0);
    assert(shell_prompt_columns(s)==8);
    strcpy(path,"/home/tester/Ж中");
    s=shell_prompt_expand("\\w> ","/home/tester");assert(shell_prompt_columns(s)==7);
    memset(large,'x',sizeof(large));large[sizeof(large)-1]=0;
    s=shell_prompt_expand(large,0);assert(strlen(s)==1023);assert(shell_prompt_columns(s)==1023);
    assert(!strcmp(shell_prompt_expand(0,0),""));
    return 0;
}
