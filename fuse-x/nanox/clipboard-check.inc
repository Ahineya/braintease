/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nx_clipboard.h"
static int paste_check(const char *path,unsigned long count,int bracketed) {
    struct termios raw;char buf[97],ready[128];int fd,n,marker;unsigned long got=0;
    if(tcgetattr(0,&raw))return 1;
    raw.c_lflag&=~(ICANON|ECHO|ISIG);raw.c_iflag=0;raw.c_cc[VMIN]=1;raw.c_cc[VTIME]=0;
    if(tcsetattr(0,TCSANOW,&raw))return 1;
    fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return 1;
    puts("Clipboard Ж 🙂");if(bracketed)fputs("\033[?2004h",stdout);fflush(stdout);
    snprintf(ready,sizeof(ready),"%s.ready",path);marker=open(ready,O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(marker<0)return 1;write(marker,"ready",5);close(marker);
    while(got<count) {
        n=read(0,buf,count-got<sizeof(buf)?count-got:sizeof(buf));
        if(n<0||!n)return 1;
        if(write(fd,buf,n)!=n)return 1;got+=n;usleep(1000);
    }
    close(fd);fd=open("/tmp/paste-done",O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(fd<0)return 1;write(fd,"done",4);close(fd);
    /* Leave the displayed text and terminal alive for selection/close tests. */
    for(;;)usleep(10000);
}
static int clipboard_check(int argc,char **argv) {
    GR_WINDOW_ID w;size_t length=0;char *text;int fd,result=1;
    if(argc<4)return 2;
    w=GrNewWindow(GR_ROOT_WINDOW_ID,0,0,8,16,0,0,0);if(!w)return 1;
    if(!strcmp(argv[2],"get")) {
        text=nx_clipboard_get(w,&length);
        if(text){fd=open(argv[3],O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd>=0){result=write(fd,text,length)!=length;close(fd);}free(text);}
    } else if(!strcmp(argv[2],"set"))result=nx_clipboard_set(w,argv[3],strlen(argv[3]));
    else if(!strcmp(argv[2],"foreign")) {
        GR_WINDOW_ID other=strtoul(argv[3],NULL,10);
        result=nx_clipboard_set(other,"wrong-owner",11)==0;
    }
    GrDestroyWindow(w);GrClose();printf("CLIPBOARD_CHECK %d\n",result);return result;
}

static int file_check(const char *path) {
    char buf[257];int fd,n;unsigned i;unsigned long length=0,hash=2166136261UL;
    fd=open(path,O_RDONLY);if(fd<0)return 1;
    while((n=read(fd,buf,sizeof(buf)))>0)for(i=0;i<(unsigned)n;i++){hash=(hash^(unsigned char)buf[i])*16777619UL;length++;}
    close(fd);if(n<0)return 1;printf("CLIPBOARD_FILE %lu %08lx\n",length,hash);return 0;
}
