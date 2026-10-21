/* SPDX-License-Identifier: MIT */
/* The collection launcher is a small guest Nano-X client, like the games. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "nano-X.h"
#include "puzzle_catalog.h"

#define BG GR_RGB(26,33,39)
#define PANEL GR_RGB(35,44,53)
#define FG GR_RGB(226,233,235)
#define MUTED GR_RGB(151,170,180)
#define GOLD GR_RGB(235,193,110)
#define SELECTED GR_RGB(51,72,84)
#define LIST_TOP 92
#define ROW 32
static GR_WINDOW_ID win;
static GR_GC_ID gc;
static int w=720,h=620,dirty=1,choice,first,count,matching[PUZZLE_COUNT];
static int wakefd[2],server;
static volatile int stopping;
static pid_t children[PUZZLE_COUNT];
static char query[64],directory[512],status[256];

static void caught(int sig)
{
    char c=1;int saved=errno;signal(sig,caught);
    if(sig!=SIGCHLD)stopping=1;
    write(wakefd[1],&c,1);errno=saved;
}
static int lower(int c) { return c>='A'&&c<='Z'?c+'a'-'A':c; }
static int contains(const char *text,const char *needle)
{
    int i,j;
    for(i=0;text[i]||!needle[0];i++){
        for(j=0;needle[j]&&text[i+j]&&lower(text[i+j])==lower(needle[j]);j++){}
        if(!needle[j])return 1;
    }
    return 0;
}
static int match_rank(const char *text,const char *needle)
{
    int i;for(i=0;needle[i]&&text[i]&&lower(text[i])==lower(needle[i]);i++){}
    if(!needle[i])return text[i]?1:0;
    return contains(text,needle)?2:3;
}
static void filter(void)
{
    int i,rank,n,a,b;count=0;choice=first=0;
    /* "net" should select Net before Magnets, which also contains "net". */
    for(rank=0;rank<3;rank++)for(i=0;i<(int)PUZZLE_COUNT;i++){
        a=match_rank(puzzle_catalog[i].name,query);b=match_rank(puzzle_catalog[i].id,query);n=a<b?a:b;
        if(n==rank)matching[count++]=i;
    }
    dirty=1;
}
static int rows(void) { int n=(h-LIST_TOP-148)/ROW;return n>0?n:1; }
static void box(int x,int y,int width,int height,GR_COLOR colour)
{
    GrSetGCForeground(gc,colour);GrFillRect(win,gc,x,y,width,height);
}
static void text(int x,int y,const char *s,int limit,GR_COLOR colour)
{
    int n=strlen(s);if(n>limit)n=limit;
    if(n>0){GrSetGCForeground(gc,colour);GrText(win,gc,x,y,s,n,GR_TFASCII|GR_TFTOP);}
}
static void paint(void)
{
    int i,n,x,y,columns=2,cell=(w-32)/columns,shown=rows()*columns;
    char buf[100];const char *s;
    if(!dirty)return;
    if(choice<first)first=(choice/columns)*columns;
    if(choice>=first+shown)first=(choice/columns-rows()+1)*columns;
    box(0,0,w,h,BG);box(0,0,w,80,PANEL);
    text(16,14,"PORTABLE PUZZLES",32,GOLD);
    snprintf(buf,sizeof(buf),"%d games / Simon Tatham",(int)PUZZLE_COUNT);text(300,14,buf,(w-312)/8,MUTED);
    text(16,50,"Find",4,MUTED);box(64,43,w-80,28,SELECTED);
    snprintf(buf,sizeof(buf),"%s_",query);text(74,49,buf,(w-96)/8,FG);
    for(i=0;i<shown&&i+first<count;i++){
        n=i+first;x=16+(i%columns)*cell;y=LIST_TOP+(i/columns)*ROW;
        box(x,y,cell-8,ROW-4,n==choice?SELECTED:BG);
        snprintf(buf,sizeof(buf),"%s%s",puzzle_catalog[matching[n]].name,children[matching[n]]?"  *":"");
        text(x+10,y+7,buf,(cell-28)/8,n==choice?GOLD:FG);
    }
    if(!count)text(20,LIST_TOP+16,"No matching games. Backspace edits; Escape clears.",(w-40)/8,MUTED);
    box(0,h-136,w,136,PANEL);
    if(count){
        text(16,h-124,puzzle_catalog[matching[choice]].name,(w-150)/8,GOLD);
        box(w-120,h-130,104,30,SELECTED);text(w-100,h-123,"Play",8,GOLD);
        s=puzzle_catalog[matching[choice]].objective;y=h-88;
        while(*s&&y<h-38){
            n=strlen(s);if(n>(w-32)/8){n=(w-32)/8;while(n>0&&s[n]!=' ')n--;if(!n)n=(w-32)/8;}
            text(16,y,s,n,FG);s+=n;while(*s==' ')s++;y+=18;
        }
    }
    text(16,h-24,status[0]?status:"Type to find / Arrows: select / Enter: play / *: open",(w-32)/8,MUTED);
    GrFlush();dirty=0;
}
static void reap(void)
{
    pid_t pid;int result,i;
    while((pid=waitpid(-1,&result,WNOHANG))>0){
        for(i=0;i<(int)PUZZLE_COUNT;i++)if(children[i]==pid){
            children[i]=0;
            if(!WIFEXITED(result)||WEXITSTATUS(result))snprintf(status,sizeof(status),"%s closed with an error; see terminal output.",puzzle_catalog[i].name);
            dirty=1;
        }
    }
}
static void launch(void)
{
    int index,fd;pid_t pid;char path[600];
    if(!count)return;index=matching[choice];
    if(children[index]){snprintf(status,sizeof(status),"%s is already open.",puzzle_catalog[index].name);dirty=1;return;}
    if(snprintf(path,sizeof(path),"%s/%s",directory,puzzle_catalog[index].id)>=(int)sizeof(path)-5){
        snprintf(status,sizeof(status),"Game path is too long.");dirty=1;return;
    }
    if(access(path,X_OK)<0)strcat(path,".fxe");
    if(access(path,X_OK)<0){snprintf(status,sizeof(status),"Cannot open %s: %s",puzzle_catalog[index].name,strerror(errno));dirty=1;return;}
    GrFlush();pid=fork();
    if(pid==0){
        char *arguments[2];arguments[0]=path;arguments[1]=NULL;
        /* Close inherited descriptors without sending GrClose on the
         * launcher's shared Nano-X connection. The child opens its own. */
        fd=server;close(fd);close(wakefd[0]);close(wakefd[1]);
        signal(SIGCHLD,SIG_DFL);signal(SIGTERM,SIG_DFL);signal(SIGHUP,SIG_DFL);signal(SIGINT,SIG_DFL);
        execv(path,arguments);perror(path);_exit(127);
    }
    if(pid<0)snprintf(status,sizeof(status),"Cannot start game: %s",strerror(errno));
    else {children[index]=pid;snprintf(status,sizeof(status),"Opened %s. Each game saves its own progress.",puzzle_catalog[index].name);}
    dirty=1;
}
static void key(int ch,unsigned mods)
{
    int n=strlen(query),step=0;
    if((mods&MWKMOD_CTRL)&&(ch=='q'||ch=='Q'||ch==17)){stopping=1;return;}
    if(ch==MWKEY_ESCAPE||ch==27){if(query[0]){query[0]=0;filter();}else stopping=1;return;}
    if(ch=='\r'||ch=='\n'){launch();return;}
    if(ch==MWKEY_UP)step=-2;if(ch==MWKEY_DOWN)step=2;
    if(ch==MWKEY_LEFT)step=-1;if(ch==MWKEY_RIGHT)step=1;
    if(ch==MWKEY_PAGEUP)step=-2*rows();if(ch==MWKEY_PAGEDOWN)step=2*rows();
    if(ch==MWKEY_HOME)choice=0;else if(ch==MWKEY_END&&count)choice=count-1;
    else if(step){choice+=step;if(choice<0)choice=0;if(choice>=count)choice=count?count-1:0;}
    else if((mods&MWKMOD_CTRL)&&(ch=='u'||ch=='U'||ch==21)){query[0]=0;filter();}
    else if((ch==8||ch==127||ch==MWKEY_BACKSPACE)&&n){query[n-1]=0;filter();}
    else if(ch>=32&&ch<127&&!(mods&MWKMOD_CTRL)&&n<(int)sizeof(query)-1){query[n]=ch;query[n+1]=0;filter();}
    status[0]=0;dirty=1;
}
int main(int argc,char **argv)
{
    GR_EVENT e;GR_SCREEN_INFO screen;struct stat st;char *slash,candidate[550];int n;
    if(argc==2&&!strcmp(argv[1],"--help")){puts("usage: puzzles [--games DIRECTORY]\nFind a game, then Enter or click Play. Click a selected game to play.\nGames save progress independently. Closing the launcher leaves games open.");return 0;}
    if(argc==3&&!strcmp(argv[1],"--games")){
        if(strlen(argv[2])>=sizeof(directory)){fputs("puzzles: game directory too long\n",stderr);return 2;}
        strcpy(directory,argv[2]);
    }else if(argc!=1){fputs("usage: puzzles [--games DIRECTORY]\n",stderr);return 2;}
    else{
        if(strlen(argv[0])>=sizeof(directory)){fputs("puzzles: launcher path too long\n",stderr);return 2;}
        strcpy(directory,argv[0]);slash=strrchr(directory,'/');
        if(slash)*slash=0;else strcpy(directory,"/bin");
        snprintf(candidate,sizeof(candidate),"%s/puzzles",directory);
        if(stat(candidate,&st)==0&&S_ISDIR(st.st_mode)){
            if(strlen(candidate)>=sizeof(directory)){fputs("puzzles: game directory too long\n",stderr);return 2;}
            strcpy(directory,candidate);
        }
    }
    server=GrOpen();if(server<0){fputs("puzzles: start Nano-X with startx first\n",stderr);return 1;}
    GrGetScreenInfo(&screen);if(w>screen.cols-24)w=screen.cols-24;if(h>screen.rows-64)h=screen.rows-64;
    if(w<480)w=480;if(h<400)h=400;
    win=GrNewWindowEx(GR_WM_PROPS_APPWINDOW,"Portable Puzzles",GR_ROOT_WINDOW_ID,16,12,w,h,BG);gc=GrNewGC();
    if(!win||!gc){fputs("puzzles: cannot create launcher window\n",stderr);GrClose();return 1;}
    if(pipe(wakefd)<0||fcntl(wakefd[0],F_SETFL,O_NONBLOCK)<0||fcntl(wakefd[1],F_SETFL,O_NONBLOCK)<0){perror("puzzles: signal pipe");GrClose();return 1;}
    GrSetGCUseBackground(gc,GR_FALSE);GrSelectEvents(win,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_KEY_DOWN|GR_EVENT_MASK_BUTTON_DOWN|GR_EVENT_MASK_UPDATE|GR_EVENT_MASK_CLOSE_REQ);
    GrRegisterInput(wakefd[0]);signal(SIGCHLD,caught);signal(SIGTERM,caught);signal(SIGHUP,caught);signal(SIGINT,caught);
    filter();GrMapWindow(win);GrSetFocus(win);paint();
    if(getenv("PUZZLES_TRACE"))fprintf(stderr,"PUZZLES_LAUNCHER_READY %d\n",(int)PUZZLE_COUNT);
    while(!stopping){
        GrGetNextEvent(&e);
        if(e.type==GR_EVENT_TYPE_CLOSE_REQ)stopping=1;
        else if(e.type==GR_EVENT_TYPE_EXPOSURE)dirty=1;
        else if(e.type==GR_EVENT_TYPE_FDINPUT){char buf[32];while(read(wakefd[0],buf,sizeof(buf))>0){}reap();}
        else if(e.type==GR_EVENT_TYPE_KEY_DOWN)key(e.keystroke.ch,e.keystroke.modifiers);
        else if(e.type==GR_EVENT_TYPE_UPDATE&&e.update.utype==GR_UPDATE_SIZE&&e.update.wid==win){
            w=e.update.width<480?480:e.update.width;h=e.update.height<400?400:e.update.height;
            if(w!=e.update.width||h!=e.update.height)GrResizeWindow(win,w,h);dirty=1;
        }else if(e.type==GR_EVENT_TYPE_BUTTON_DOWN){
            if(e.button.changebuttons&GR_BUTTON_SCROLLUP)key(MWKEY_UP,0);
            else if(e.button.changebuttons&GR_BUTTON_SCROLLDN)key(MWKEY_DOWN,0);
            else if(e.button.changebuttons&GR_BUTTON_L){
                if(e.button.y>=h-130&&e.button.y<h-100&&e.button.x>=w-120)launch();
                else if(e.button.y>=LIST_TOP&&e.button.y<LIST_TOP+rows()*ROW&&e.button.x>=16&&e.button.x<w-16){
                    n=first+((e.button.y-LIST_TOP)/ROW)*2+(e.button.x-16)/((w-32)/2);
                    if(n<count){if(n==choice)launch();else {choice=n;status[0]=0;dirty=1;}}
                }
            }
        }
        if(!stopping)paint();
    }
    reap();GrUnregisterInput(wakefd[0]);close(wakefd[0]);close(wakefd[1]);
    GrDestroyWindow(win);GrDestroyGC(gc);GrClose();return 0;
}
