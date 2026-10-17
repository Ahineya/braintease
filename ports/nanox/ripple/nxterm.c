/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The shared terminal/PTY model, rendered through the real Nano-X client API. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <time.h>
#include <syscalls.h>
#include "nano-X.h"
#include "pty-limits.h"
#include "../../../ports/graphics/terminal.h"
#include "../../../ports/graphics/terminal.c"
static struct vt terminal;
static GR_WINDOW_ID window;
static GR_GC_ID gc;
static int master=-1,child,connected;
static volatile int stopping;
static char pending[512];static unsigned pending_count;
static const GR_COLOR palette[16]={
    GR_RGB(0,0,0),GR_RGB(168,0,0),GR_RGB(0,168,0),GR_RGB(168,168,0),
    GR_RGB(0,0,168),GR_RGB(168,0,168),GR_RGB(0,168,168),GR_RGB(168,168,168),
    GR_RGB(84,84,84),GR_RGB(255,84,84),GR_RGB(84,255,84),GR_RGB(255,255,84),
    GR_RGB(84,84,255),GR_RGB(255,84,255),GR_RGB(84,255,255),GR_RGB(255,255,255)};
static void caught(int s){stopping=s;signal(s,caught);}
static void queue(const char *s){unsigned n=strlen(s);if(n<=sizeof(pending)-pending_count){memcpy(pending+pending_count,s,n);pending_count+=n;}}
static void reply(struct vt *t,const char *s){queue(s);}
static void invalidate(void){unsigned i;for(i=0;i<terminal.rows;i++)terminal.dirty[i]=1;}
static void draw(void){
    unsigned y,x,end,colour,dirty=0;char text[VT_MAX_COLS];struct vt_cell *cells=vt_cells(&terminal);
    for(y=0;y<terminal.rows;y++)dirty|=terminal.dirty[y];
    if(!dirty)return;
    for(y=0;y<terminal.rows;y++)if(terminal.dirty[y]){
        x=0;while(x<terminal.cols){
            colour=cells[y*terminal.cols+x].colour;end=x;
            while(end<terminal.cols&&cells[y*terminal.cols+end].colour==colour){text[end-x]=cells[y*terminal.cols+end].ch&255;end++;}
            GrSetGCForeground(gc,palette[colour&15]);GrSetGCBackground(gc,palette[(colour>>4)&15]);
            GrText(window,gc,x*8,y*16,text,end-x,GR_TFASCII|GR_TFTOP);x=end;
        }
        terminal.dirty[y]=0;
    }
    if(terminal.visible){GrSetGCForeground(gc,palette[cells[terminal.y*terminal.cols+terminal.x].colour&15]);GrLine(window,gc,terminal.x*8,terminal.y*16+15,terminal.x*8+7,terminal.y*16+15);}
    GrFlush();
}
static void key(GR_EVENT_KEYSTROKE *e){
    unsigned k=e->ch;char s[2]={0,0};
    if(getenv("NANOX_TRACE_WIRE"))fprintf(stderr,"NXKEY %d %u\n",getpid(),k);
    switch(k){
        case MWKEY_UP:queue(terminal.appcursor?"\033OA":"\033[A");return;
        case MWKEY_DOWN:queue(terminal.appcursor?"\033OB":"\033[B");return;
        case MWKEY_RIGHT:queue(terminal.appcursor?"\033OC":"\033[C");return;
        case MWKEY_LEFT:queue(terminal.appcursor?"\033OD":"\033[D");return;
        case MWKEY_HOME:queue("\033[H");return;case MWKEY_END:queue("\033[F");return;
        case MWKEY_INSERT:queue("\033[2~");return;case MWKEY_DELETE:queue("\033[3~");return;
        case MWKEY_PAGEUP:queue("\033[5~");return;case MWKEY_PAGEDOWN:queue("\033[6~");return;
        case MWKEY_F1:queue("\033OP");return;case MWKEY_F2:queue("\033OQ");return;
        case MWKEY_F3:queue("\033OR");return;case MWKEY_F4:queue("\033OS");return;
        case MWKEY_F5:queue("\033[15~");return;case MWKEY_F6:queue("\033[17~");return;
        case MWKEY_F7:queue("\033[18~");return;case MWKEY_F8:queue("\033[19~");return;
        case MWKEY_F9:queue("\033[20~");return;case MWKEY_F10:queue("\033[21~");return;
        case MWKEY_F11:queue("\033[23~");return;case MWKEY_F12:queue("\033[24~");return;
        case MWKEY_BACKSPACE:k=127;break;
    }
    if(k>255)return;
    if(e->modifiers&MWKMOD_CTRL){if((k>='a'&&k<='z')||(k>='@'&&k<='_'))k&=31;else if(k==' ')k=0;}
    if(e->modifiers&MWKMOD_ALT)queue("\033");
    if(k){s[0]=k;queue(s);}else if(pending_count<sizeof(pending))pending[pending_count++]=0;
}
static void event(GR_EVENT *e){
    struct winsize size;unsigned cols,rows;
    if(e->type==GR_EVENT_TYPE_EXPOSURE)invalidate();
    else if(e->type==GR_EVENT_TYPE_KEY_DOWN)key(&e->keystroke);
    else if(e->type==GR_EVENT_TYPE_CLOSE_REQ)stopping=SIGHUP;
    else if(e->type==GR_EVENT_TYPE_UPDATE&&e->update.utype==GR_UPDATE_SIZE){
        cols=e->update.width/8;rows=e->update.height/16;
        if(cols>VT_MAX_COLS)cols=VT_MAX_COLS;if(rows>VT_MAX_ROWS)rows=VT_MAX_ROWS;
        if(!cols)cols=1;if(!rows)rows=1;
        vt_resize(&terminal,cols,rows);memset(&size,0,sizeof(size));
        size.ws_col=cols;size.ws_row=rows;size.ws_xpixel=cols*8;size.ws_ypixel=rows*16;
        ioctl(master,TIOCSWINSZ,&size);
        if(getenv("NANOX_TRACE"))fprintf(stderr,"NXTERM_SIZE %d %u %u\n",getpid(),rows,cols);
        GrSetGCForeground(gc,palette[0]);
        GrFillRect(window,gc,0,0,e->update.width,e->update.height);invalidate();
    }
}
static void cleanup(void){
    unsigned i;int status;
    if(master>=0){close(master);master=-1;}
    if(child>0){
        kill(-child,SIGHUP);kill(-child,SIGCONT);
        for(i=0;i<20;i++){if(waitpid(child,&status,WNOHANG)==child){child=0;break;}usleep(10000);}
        if(child>0){kill(-child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}}
    }
    if(getenv("NANOX_TRACE"))fprintf(stderr,"NXTERM_EXIT %d\n",getpid());
    if(connected){connected=0;GrClose();}
}
int main(int argc,char **argv){
    unsigned i;int n,status=0,command=1;unsigned cols=64,rows=20;
    char path[]="/dev/ptyp0",slave[]="/dev/ttyp0",buf[512];char *shell[]={"sh",NULL};
    GR_WM_PROPERTIES props;GR_EVENT e;struct winsize size;struct termios settings;fd_set reads,writes;struct timeval wait;int maxfd;
    if(argc>1&&!strcmp(argv[1],"--help")){puts("usage: nxterm [COMMAND [ARG...]]\nA Nano-X terminal (default: sh).");return 0;}
    if(GrOpen()<0){fputs("nxterm: cannot connect to Nano-X\n",stderr);return 1;}connected=1;atexit(cleanup);
    vt_init_size(&terminal,cols,rows);terminal.reply=reply;
    window=GrNewWindow(GR_ROOT_WINDOW_ID,24+(getpid()%4)*16,32+(getpid()%4)*16,cols*8,rows*16,0,GR_RGB(0,0,0),0);
    if(!window)return 1;
    memset(&props,0,sizeof(props));props.flags=GR_WM_FLAGS_TITLE|GR_WM_FLAGS_PROPS;
    props.title="Terminal";props.props=GR_WM_PROPS_APPWINDOW;
    GrSetWMProperties(window,&props);gc=GrNewGC();GrSetGCUseBackground(gc,GR_TRUE);
    GrSelectEvents(window,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_KEY_DOWN|GR_EVENT_MASK_CLOSE_REQ|GR_EVENT_MASK_UPDATE);
    for(i=0;i<RIPPLE_PTY_COUNT;i++){path[9]=slave[9]='0'+i;master=open(path,O_RDWR|O_NONBLOCK|O_NOCTTY);if(master>=0)break;if(errno!=EBUSY){perror("nxterm");return 1;}}
    if(master<0){fputs("nxterm: no free terminals\n",stderr);return 1;}
    /* Backspace sends DEL. Raw-mode applications such as Levee read VERASE
     * instead of accepting both DEL and Ctrl-H like the shell editor does. */
    if(tcgetattr(master,&settings)<0)return 1;
    settings.c_cc[VERASE]=127;
    if(tcsetattr(master,TCSANOW,&settings)<0)return 1;
    memset(&size,0,sizeof(size));size.ws_col=cols;size.ws_row=rows;size.ws_xpixel=cols*8;size.ws_ypixel=rows*16;
    if(ioctl(master,TIOCSWINSZ,&size)<0)return 1;
    child=fork();if(child<0)return 1;
    if(!child){
        int fd;close(master);if(setsid()<0)_exit(125);fd=open(slave,O_RDWR);if(fd<0)_exit(125);
        dup2(fd,0);dup2(fd,1);dup2(fd,2);for(i=3;i<32;i++)close(i);
        putenv("TERM=ansi");execvp(command<argc?argv[command]:"/bin/sh",command<argc?argv+command:shell);_exit(127);
    }
    signal(SIGTERM,caught);signal(SIGINT,caught);signal(SIGHUP,caught);
    GrMapWindow(window);GrSetFocus(window);invalidate();
    if(getenv("NANOX_TRACE"))fprintf(stderr,"NXTERM_READY %d %u\n",getpid(),window);
    while(!stopping){
        for(i=0;i<8;i++){
            n=read(master,buf,sizeof(buf));if(n>0){terminal.dirty[terminal.y]=1;vt_feed(&terminal,buf,n);}else{if(!n)stopping=SIGHUP;else if(errno!=EAGAIN&&errno!=EINTR)stopping=SIGTERM;break;}
        }
        if(pending_count){n=write(master,pending,pending_count);if(n>0){pending_count-=n;memmove(pending,pending+n,pending_count);}else if(n<0&&errno!=EAGAIN&&errno!=EINTR)stopping=SIGTERM;}
        draw();
        if(waitpid(child,&status,WNOHANG)==child){child=0;break;}
        FD_ZERO(&reads);FD_ZERO(&writes);maxfd=master;FD_SET(master,&reads);if(pending_count)FD_SET(master,&writes);
        GrPrepareSelect(&maxfd,&reads);wait.tv_sec=1;wait.tv_usec=0;
        n=select(maxfd+1,&reads,&writes,NULL,&wait);
        if(n>0)GrServiceSelect(&reads,event);else if(n<0&&errno!=EINTR){stopping=SIGTERM;break;}
    }
    return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
