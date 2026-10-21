/* SPDX-License-Identifier: MIT */
#include "../service.h"
#include <termios.h>
static volatile int stopped;
static void stop(int n){stopped=n;}
static void event(const char *path,const char *kind,const char *label){
    FILE *f=fopen(path,"a");if(!f)exit(20);fprintf(f,"%s %s\n",kind,label);fclose(f);
}
int main(int argc,char **argv){
    char *fd=getenv("SVC_READY_FD");unsigned i;
    if(argc<4)return 21;
    if(!strcmp(argv[1],"fail"))return 7;
    event(argv[2],"start",argv[3]);
    for(i=4;i<(unsigned)argc;i++)printf("argument %s\n",argv[i]);fflush(stdout);
    if(!strcmp(argv[1],"console")){
        struct termios raw;
        if(!isatty(0)||tcgetpgrp(0)!=getpgrp()||tcgetattr(0,&raw))return 22;
        raw.c_lflag&=~(ICANON|ECHO);tcsetattr(0,TCSANOW,&raw);
        fputs("\033[?1049h\033[?1003hCONSOLE_RAW_EXIT\n",stdout);fflush(stdout);kill(getpid(),SIGKILL);return 23;
    }
    if(!strcmp(argv[1],"one"))return 0;
    signal(SIGTERM,!strcmp(argv[1],"ignore")?SIG_IGN:stop);
    if(fd&&strcmp(argv[1],"no-ready")){int channel=atoi(fd);write(channel,"R",1);close(channel);}
    while(!stopped)pause();
    event(argv[2],"stop",argv[3]);return 0;
}
