/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/wait.h>
#include <syscalls.h>
#include <gx.h>
extern long call(unsigned,long,long,long,long);
#include "pty-limits.h"
#include "../../graphics/terminal.h"
#include "../../graphics/terminal.c"
#include "../../graphics/fonts/rom8x16.inc"
static const unsigned short palette[16]={0,0xa800,0x0540,0xad40,0x0015,0xa815,0x0555,0xad55,0x52aa,0xfaaa,0x57ea,0xffea,0x52bf,0xfabf,0x57ff,0xffff};
static struct vt terminal;
static int gfx=-1,master=-1,child,result,trace_input;
static struct gx_window window;
static unsigned short *row_pixels;
static volatile int stopping;
static char pending[256];
static unsigned pending_count,pending_head,held[16],repeat_key;
static unsigned long now_ms,repeat_at;
static void caught(int sig){stopping=sig;signal(sig,caught);}
static int request(unsigned code,void *p) {
    if(ioctl(gfx,code,p)<0){result=1;stopping=SIGTERM;return -1;}return 0;
}
static void cleanup(void) {
    struct gx_handle h={GX_VERSION,0};
    if(gfx>=0&&window.window){h.handle=window.window;ioctl(gfx,GX_DESTROY,&h);}
    if(master>=0)close(master);
    if(gfx>=0)close(gfx);
    free(row_pixels);
}
static void queue(const char *s) {
    unsigned n=strlen(s);
    if(pending_head){memmove(pending,pending+pending_head,pending_count);pending_head=0;}
    if(n<=sizeof(pending)-pending_count){memcpy(pending+pending_count,s,n);pending_count+=n;}
}
static void reply(struct vt *t,const char *s){(void)t;queue(s);}
/* Shadow cells describe pixels already uploaded, independently of the VT's
 * current/alternate model. Only dirty rows are compared; typing paints two
 * cells (old/new cursor), while cleared space uses the host bulk fill. */
static struct vt_cell painted[VT_ROWS*VT_COLS];
static unsigned char painted_rows[VT_ROWS];
static int painted_cursor=-1;
static int cell_changed(struct vt_cell *cells,unsigned at,int cursor) {
    return !painted_rows[at/VT_COLS] || cells[at].ch!=painted[at].ch ||
        cells[at].colour!=painted[at].colour || (int)at==painted_cursor || (int)at==cursor;
}
static int draw(int show_cursor) {
    unsigned y,x,end,sy,sx,fg,bg,bits,at,span;struct vt_cell *cells=vt_cells(&terminal);
    int cursor=show_cursor&&terminal.visible?(int)(terminal.y*VT_COLS+terminal.x):-1;
    struct gx_upload u;struct gx_fill f;
    if(painted_cursor>=0)terminal.dirty[painted_cursor/VT_COLS]=1;
    if(cursor>=0)terminal.dirty[cursor/VT_COLS]=1;
    memset(&u,0,sizeof(u));u.version=GX_VERSION;u.surface=window.surface;u.height=16;u.pixels=row_pixels;
    memset(&f,0,sizeof(f));f.version=GX_VERSION;f.surface=window.surface;f.height=16;
    for(y=0;y<VT_ROWS;y++)if(terminal.dirty[y]) {
        for(x=0;x<VT_COLS;x=end) {
            at=y*VT_COLS+x;end=x+1;
            if(!cell_changed(cells,at,cursor))continue;
            bg=palette[(cells[at].colour>>4)&15];
            if(cells[at].ch==' '&&(int)at!=cursor) {
                while(end<VT_COLS&&cells[y*VT_COLS+end].ch==' '&&
                    palette[(cells[y*VT_COLS+end].colour>>4)&15]==bg&&(int)(y*VT_COLS+end)!=cursor)end++;
                f.x=x*8;f.y=y*16;f.width=(end-x)*8;f.colour=bg;
                if(request(GX_FILL,&f))return -1;
            } else {
                while(end<VT_COLS&&cell_changed(cells,y*VT_COLS+end,cursor)&&cells[y*VT_COLS+end].ch!=' ')end++;
                span=(end-x)*8;
                for(at=x;at<end;at++) {
                    struct vt_cell *cell=cells+y*VT_COLS+at;
                    fg=palette[cell->colour&15];bg=palette[(cell->colour>>4)&15];
                    for(sy=0;sy<16;sy++) {
                        bits=gx_font[(cell->ch&255)*16+sy];
                        if((int)(y*VT_COLS+at)==cursor&&sy==15)bits=65535;
                        for(sx=0;sx<8;sx++)row_pixels[sy*span+(at-x)*8+sx]=(bits&(0x8000U>>sx))?fg:bg;
                    }
                }
                u.x=x*8;u.y=y*16;u.width=u.stride=span;
                if(request(GX_UPLOAD,&u))return -1;
            }
            memcpy(painted+y*VT_COLS+x,cells+y*VT_COLS+x,(end-x)*sizeof(*cells));
        }
        painted_rows[y]=1;terminal.dirty[y]=0;
    }
    painted_cursor=cursor;
    return 0;
}
static void scroll_pixels(struct vt *t,int amount) {
    unsigned n=amount<0?-amount:amount,h=(t->bottom-t->top+1-n)*16;
    struct gx_copy c;struct gx_fill f;
    /* Complete pending text without copying a software cursor into the scroll. */
    t->dirty[t->y]=1;if(draw(0))return;
    memset(&c,0,sizeof(c));c.version=GX_VERSION;c.surface=window.surface;
    c.width=640;c.height=h;c.sy=(t->top+(amount>0?n:0))*16;c.dy=(t->top+(amount<0?n:0))*16;
    if(h&&request(GX_COPY,&c))return;
    memmove(painted+(t->top+(amount<0?n:0))*VT_COLS,
        painted+(t->top+(amount>0?n:0))*VT_COLS,(unsigned long)(h/16)*VT_COLS*sizeof(*painted));
    memmove(painted_rows+t->top+(amount<0?n:0),painted_rows+t->top+(amount>0?n:0),h/16);
    memset(painted_rows+(amount>0?t->bottom+1-n:t->top),0,n);
    memset(&f,0,sizeof(f));f.version=GX_VERSION;f.surface=window.surface;
    f.y=(amount>0?t->bottom+1-n:t->top)*16;f.width=640;f.height=n*16;
    f.colour=palette[t->reverse?t->fg:t->bg];request(GX_FILL,&f);
}
static int down(unsigned key){return held[key>>4]&(1U<<(key&15));}
static void key(unsigned code) {
    unsigned shift=down(225)||down(229),ctrl=down(224)||down(228);char text[8]={0};unsigned c=0;
    static const char normal[]="1234567890",shifted[]="!@#$%^&*()";
    if(code>=4&&code<=29)c=(shift?'A':'a')+code-4;
    else if(code>=30&&code<=39)c=(shift?shifted:normal)[code-30];
    else switch(code) {
        case 40:c='\r';break;case 41:c=27;break;case 42:c=127;break;case 43:c='\t';break;case 44:c=' ';break;
        case 45:c=shift?'_':'-';break;case 46:c=shift?'+':'=';break;case 47:c=shift?'{':'[';break;
        case 48:c=shift?'}':']';break;case 49:c=shift?'|':'\\';break;case 51:c=shift?':':';';break;
        case 52:c=shift?'"':'\'';break;case 53:c=shift?'~':'`';break;case 54:c=shift?'<':',';break;
        case 55:c=shift?'>':'.';break;case 56:c=shift?'?':'/';break;
        case 79:case 80:case 81:case 82:
            text[0]=27;text[1]=terminal.appcursor?'O':'[';text[2]=code==79?'C':code==80?'D':code==81?'B':'A';queue(text);return;
        case 58:queue("\033OP");return;case 59:queue("\033OQ");return;
        case 60:queue("\033OR");return;case 61:queue("\033OS");return;
        case 62:queue("\033[15~");return;case 63:queue("\033[17~");return;
        case 64:queue("\033[18~");return;case 65:queue("\033[19~");return;
        case 66:queue("\033[20~");return;case 67:queue("\033[21~");return;
        case 68:queue("\033[23~");return;case 69:queue("\033[24~");return;
        case 74:queue("\033[H");return;case 77:queue("\033[F");return;
        case 73:queue("\033[2~");return;case 76:queue("\033[3~");return;
        case 75:queue("\033[5~");return;case 78:queue("\033[6~");return;
    }
    if(!c)return;
    if(ctrl) {if((c>='a'&&c<='z')||(c>='@'&&c<='_'))c&=31;else if(c==' ')c=0;}
    if(!c){
        if(pending_head){memmove(pending,pending+pending_head,pending_count);pending_head=0;}
        if(pending_count<sizeof(pending))pending[pending_count++]=0;return;
    }
    text[0]=c;
    if(down(226)||down(230))queue("\033");
    queue(text);
}
int main(int argc,char **argv) {
    char path[]="/dev/ptyp0",slave[]="/dev/ttyp0",output[512];char *sh[]={"sh",NULL};
    unsigned i;int n,status=0,dirty;struct winsize size={VT_ROWS,VT_COLS,640,480};
    struct gx_handle present;struct gx_event event;struct gx_wait wait;
    if(argc>1&&!strcmp(argv[1],"--help")){puts("usage: gterm [COMMAND [ARG...]]\nOpen a graphical terminal (default sh). Requires RVM --visual.");return 0;}
    trace_input=getenv("GTERM_TRACE_INPUT")!=NULL;
    gfx=open("/dev/gfx",O_RDWR|O_CLOEXEC);if(gfx<0){perror("gterm");return 1;}
    atexit(cleanup);
    row_pixels=malloc(640UL*16*sizeof(*row_pixels));if(!row_pixels){perror("gterm");return 1;}
    memset(&window,0,sizeof(window));window.version=GX_VERSION;window.width=640;window.height=480;window.format=GX_RGB565;strcpy(window.title,"Terminal");
    if(request(GX_CREATE,&window)){perror("gterm");return 1;}
    for(i=0;i<RIPPLE_PTY_COUNT;i++){path[9]=slave[9]='0'+i;master=open(path,O_RDWR|O_NONBLOCK|O_NOCTTY);if(master>=0)break;if(errno!=EBUSY){perror("gterm");return 1;}}
    if(master<0){fputs("gterm: no free terminals\n",stderr);return 1;}
    ioctl(master,TIOCSWINSZ,&size);child=fork();
    if(child<0){perror("gterm");return 1;}
    if(!child) {
        int fd;close(gfx);close(master);if(setsid()<0)_exit(125);
        fd=open(slave,O_RDWR);if(fd<0)_exit(125);dup2(fd,0);dup2(fd,1);dup2(fd,2);
        for(i=3;i<32;i++)close(i);
        if(putenv("TERM=ansi"))_exit(125);
        execvp(argc>1?argv[1]:"/bin/sh",argc>1?argv+1:sh);_exit(127);
    }
    vt_init(&terminal);terminal.scroll=scroll_pixels;terminal.reply=reply;
    signal(SIGINT,caught);signal(SIGTERM,caught);signal(SIGHUP,caught);
    present.version=GX_VERSION;present.handle=window.window;
    while(!stopping) {
        /* Bound each batch so continuous output cannot starve window input. */
        for(i=0;i<8;i++) {
            n=read(master,output,sizeof(output));
            if(n>0)vt_feed(&terminal,output,n);else {if(!n)stopping=SIGHUP;else if(errno!=EAGAIN&&errno!=EINTR){result=1;stopping=SIGHUP;}break;}
        }
        for(i=0;i<32;i++) {
            memset(&event,0,sizeof(event));event.version=GX_VERSION;event.window=window.window;
            if(request(GX_EVENT,&event)||!event.type)break;
            if(trace_input)fprintf(stderr,"gterm input: type=%u code=%u flags=%u\n",event.type,event.code,event.flags);
            if(event.type==GX_CLOSE){stopping=SIGHUP;break;}
            if(event.type==GX_OVERFLOW||(event.type==GX_FOCUS&&!(event.flags&GX_FOCUSED))){memset(held,0,sizeof(held));repeat_key=0;}
            if(event.type==GX_KEY&&event.code<256) {
                unsigned bit=1U<<(event.code&15);
                if(event.flags&GX_DOWN){
                    held[event.code>>4]|=bit;key(event.code);
                    if(event.code<224){repeat_key=event.code;repeat_at=now_ms+500;}
                } else {held[event.code>>4]&=~bit;if(repeat_key==event.code)repeat_key=0;}
            }
        }
        if(repeat_key&&(long)(now_ms-repeat_at)>=0){key(repeat_key);repeat_at=now_ms+100;}
        if(pending_count) {
            n=write(master,pending+pending_head,pending_count);
            if(n>0){pending_head+=n;pending_count-=n;if(!pending_count)pending_head=0;}
            else if(n<0&&errno!=EAGAIN&&errno!=EINTR){result=1;break;}
        }
        dirty=0;for(i=0;i<VT_ROWS;i++)dirty|=terminal.dirty[i];
        if(dirty&&(draw(1)||request(GX_PRESENT,&present)))break;
        if(waitpid(child,&status,WNOHANG)==child) {
            while((n=read(master,output,sizeof(output)))>0)vt_feed(&terminal,output,n);
            draw(1);request(GX_PRESENT,&present);child=0;break;
        }
        memset(&wait,0,sizeof(wait));wait.version=GX_VERSION;wait.window=window.window;wait.master=master;
        wait.flags=GX_READY_INPUT|GX_READY_READ|(pending_count?GX_READY_WRITE:0);
        wait.timeout=repeat_key?1:10;
        if(ioctl(gfx,GX_WAIT,&wait)<0){if(errno==EINTR)continue;result=1;break;}
        now_ms=wait.milliseconds;
    }
    if(child) {
        kill(-child,SIGHUP);kill(-child,SIGCONT);
        /* Give the session time to handle hangup, then bound teardown even if
         * the foreground command ignores it. Never block the whole desktop. */
        for(i=0;i<10;i++) {
            if(waitpid(child,&status,WNOHANG)==child){child=0;break;}
            call(37,1,0,0,0);
        }
        if(child){kill(-child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}}
    }
    return result?1:WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
