/* SPDX-License-Identifier: MIT */
/* A terminal session policy, deliberately separate from background services. */
#include "service.h"
#include "ini.h"
#include <termios.h>
static int load(const char *path,struct service *s,char *error,unsigned capacity){
    size_t length;char *text=sv_read_config(path,&length,error,capacity);IniReader *r;
    int next,seen=0,ok=0;unsigned index;char *end;unsigned long number;
    if(!text)return -1;
    r=ini_reader_with_limits(text,length,SV_DEFINITIONS,SV_DEFINITIONS,16);free(text);
    if(!r){snprintf(error,capacity,"out of memory");return -1;}
    while((next=ini_next(r))>0){
        const char *key=ini_key(r),*value=ini_value(r);
        if(!strcmp(ini_section(r),"console")&&!strcmp(key,"program")){
            if(seen++||(*value&&*value!='/')||strlen(value)>=sizeof(s->program))goto done;
            strcpy(s->program,value);
        }else if(!strcmp(ini_section(r),"arguments")){
            errno=0;number=strtoul(key,&end,10);
            if(errno||!*key||*end||*key=='-'||number<1||number>SV_ARGS)goto done;
            index=number-1;
            if(s->arg_seen&(1U<<index)||strlen(value)>=sizeof(s->args[0]))goto done;
            strcpy(s->args[index],value);s->arg_seen|=1U<<index;if(index+1>s->argc)s->argc=index+1;
        }else goto done;
    }
    if(next<0||!seen||s->arg_seen!=(1U<<s->argc)-1||!*s->program&&s->argc)goto done;
    ok=1;
done:
    if(!ok)snprintf(error,capacity,"invalid console policy at line %lu",ini_line(r));
    ini_free(r);return ok?0:-1;
}
int main(int argc,char **argv){
    struct service *s;struct termios saved;char error[512],*arguments[SV_ARGS+2],*shell[]={"sh",NULL};
    unsigned i;int check=argc==4&&!strcmp(argv[3],"--check"),pid,status,have_terminal;
    if((argc!=3&&!check)||strcmp(argv[1],"--config")){fputs("usage: console-session --config FILE [--check]\n",stderr);return 2;}
    s=calloc(1,sizeof(*s));
    if(!s)strcpy(error,"out of memory");
    if(!s||load(argv[2],s,error,sizeof(error))){
        fprintf(stderr,"console-session: %s; using shell\n",error);free(s);if(check)return 1;goto shell;
    }
    if(check){free(s);return 0;}
    if(!*s->program){free(s);goto shell;}
    have_terminal=tcgetattr(0,&saved)==0;
    if(!have_terminal){fputs("console-session: no terminal; using shell\n",stderr);free(s);goto shell;}
    /* The launcher stays in the foreground group established by init. Its
     * interactive child shares that group; the fallback shell replaces it. */
    signal(SIGINT,SIG_IGN);signal(SIGQUIT,SIG_IGN);signal(SIGTSTP,SIG_IGN);signal(SIGTTOU,SIG_IGN);
    arguments[0]=s->program;for(i=0;i<s->argc;i++)arguments[i+1]=s->args[i];arguments[s->argc+1]=NULL;
    fflush(NULL);pid=fork();
    if(!pid){signal(SIGINT,SIG_DFL);signal(SIGQUIT,SIG_DFL);execv(s->program,arguments);perror(s->program);_exit(127);}
    if(pid<0)perror("console-session fork");
    else while(waitpid(pid,&status,0)<0){if(errno==EINTR)continue;perror("console-session wait");break;}
    tcsetpgrp(0,getpgrp());tcsetattr(0,TCSANOW,&saved);
    /* A killed TUI cannot run its own terminal cleanup handlers. */
    fputs("\033[?1003l\033[?1002l\033[?1000l\033[?1006l\033[?2004l\033[?2026l\033[0m\033(B\033[r\033[?7h\033[?1l\033[?25h\033[0 q\033[?1049l",stdout);fflush(stdout);
    signal(SIGINT,SIG_DFL);signal(SIGQUIT,SIG_DFL);signal(SIGTSTP,SIG_DFL);signal(SIGTTOU,SIG_DFL);
    free(s);
shell:
    execv("/bin/sh",shell);perror("console-session /bin/sh");return 127;
}
