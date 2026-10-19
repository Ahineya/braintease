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
#include <sys/select.h>
#include <time.h>
#include <syscalls.h>
#include <gx.h>
extern long call(unsigned,long,long,long,long);
#include "pty-limits.h"
#include "../../graphics/terminal.h"
#include "../../graphics/terminal.c"
#include "../../graphics/terminal-input.h"
#include "../../graphics/terminal-selection.h"
#include "../../../libraries/unicode/font.c"
#define GRAPHICS_COLS VT_MAX_COLS
#define GRAPHICS_ROWS VT_MAX_ROWS
#include "../../graphics/fonts/rom8x16.inc"
static const unsigned short palette[16]={0,0xa800,0x0540,0xad40,0x0015,0xa815,0x0555,0xad55,0x52aa,0xfaaa,0x57ea,0xffea,0x52bf,0xfabf,0x57ff,0xffff};
static struct vt terminal;
static int gfx=-1,master=-1,child,result,trace_input;
static struct gx_window window;
static unsigned short *row_pixels;
static volatile int stopping;
static struct vt_input pending;
static struct vt_selection selection;
static unsigned held[16],repeat_key;
static unsigned long now_ms,repeat_at;
static unsigned max_cols,max_rows;
static unsigned cell_width=8,cell_height=16,native_text;
static uint16_t *text_row;
static unsigned long sync_since;
static unsigned long repaint_after;
static void clear_selection(void);
static void caught(int sig){stopping=sig;signal(sig,caught);}
static int request(unsigned code,void *p) {
    if(ioctl(gfx,code,p)<0){result=1;stopping=SIGTERM;return -1;}return 0;
}
static void cleanup(void) {
    vt_selection_clear(&selection);
    vt_destroy(&terminal);
    struct gx_handle h={GX_VERSION,0};
    if(gfx>=0&&window.window){h.handle=window.window;ioctl(gfx,GX_DESTROY,&h);}
    if(master>=0)close(master);
    if(gfx>=0)close(gfx);
    free(row_pixels);free(text_row);free(pending.text);
}
static void queue(const char *s) {
    clear_selection();
    if(vt_input_put(&pending,s,strlen(s)))fputs("gterm: input queue full\n",stderr);
}
static void reply(struct vt *t,const char *s){(void)t;if(vt_input_put(&pending,s,strlen(s)))fputs("gterm: input queue full\n",stderr);}
/* Shadow cells describe pixels already uploaded, independently of the VT's
 * current/alternate model. Only dirty rows are compared; typing paints two
 * cells (old/new cursor), while cleared space uses the host bulk fill. */
static struct vt_cell painted[GRAPHICS_ROWS*GRAPHICS_COLS];
static unsigned char painted_rows[GRAPHICS_ROWS];
static int painted_cursor=-1;
static void invalidate(void){memset(painted_rows,0,sizeof(painted_rows));vt_dirty(&terminal);}
static void clear_selection(void){if(selection.active){vt_selection_clear(&selection);invalidate();}}
static unsigned cell_colour(struct vt_cell *cells,unsigned at){return vt_selection_colour(&selection,at,cells[at].colour);}
static void clipboard_action(unsigned action) {
    struct gx_clipboard c;size_t length=0;char *text;
    if(action==GX_ACTION_SELECT_ALL){if(!vt_selection_begin(&terminal,&selection,0)){selection.dragging=0;selection.end=terminal.cols*terminal.rows-1;}invalidate();return;}
    memset(&c,0,sizeof(c));c.version=GX_VERSION;c.window=window.window;c.capacity=GX_CLIPBOARD_MAX;
    if(action==GX_ACTION_PASTE) {
        clear_selection();
        text=malloc(GX_CLIPBOARD_MAX+1);if(!text)return;c.text=text;
        if(ioctl(gfx,GX_CLIPBOARD_READ,&c)==0&&vt_input_paste(&pending,text,c.length,terminal.bracketed_paste))fputs("gterm: input queue full\n",stderr);
        free(text);
    } else if(action==GX_ACTION_COPY||action==GX_ACTION_CUT) {
        text=vt_selection_text(&terminal,&selection,&length);if(!text)return;
        c.text=text;c.length=length;
        if(ioctl(gfx,GX_CLIPBOARD_WRITE,&c)<0)perror("gterm clipboard");free(text);
    }
}
static int cell_changed(struct vt_cell *cells,unsigned at,int cursor) {
    return !painted_rows[at/terminal.cols] || cells[at].ch!=painted[at].ch ||
        cells[at].colour!=painted[at].colour || (int)at==painted_cursor || (int)at==cursor;
}
static int draw_native(int cursor){
    struct vt_cell *cells=vt_cells(&terminal);struct gx_terminal_draw row;
    unsigned y,x,i,j,n,used,width;unsigned char bytes[4];
    memset(&row,0,sizeof(row));row.version=GX_VERSION;row.window=window.window;
    row.cols=terminal.cols;row.rows=terminal.rows;row.data=text_row;
    row.cursor_x=terminal.x;row.cursor_y=terminal.y;row.cursor_shape=terminal.cursor_shape;row.cursor_visible=cursor>=0;
    for(y=0;y<terminal.rows;y++)if(terminal.dirty[y]){
        for(x=0;x<terminal.cols;x++)if(cell_changed(cells,y*terminal.cols+x,cursor))break;
        if(x<terminal.cols){
            used=0;
            for(x=0;x<terminal.cols;x++){
                uint32_t glyph=cells[y*terminal.cols+x].ch;
                text_row[used++]=cell_colour(cells,y*terminal.cols+x);
                width=glyph==RU_CONTINUATION?0:ru_glyph_width(glyph)==2?2:1;
                text_row[used++]=width;n=used++;text_row[n]=0;
                if(width)for(i=0;i<ru_scalar_count(glyph);i++){
                    unsigned length=ru_encode(ru_scalar(glyph,i),bytes);
                    for(j=0;j<length;j++)text_row[used++]=bytes[j];text_row[n]+=length;
                }
            }
            row.row=y;row.length=used;if(request(GX_TERMINAL_DRAW,&row))return -1;
            memcpy(painted+y*terminal.cols,cells+y*terminal.cols,terminal.cols*sizeof(*cells));
        }
        painted_rows[y]=1;terminal.dirty[y]=0;
    }
    return 0;
}
static int draw(int show_cursor) {
    unsigned y,x,end,sy,sx,fg,bg,bits,at,span;struct vt_cell *cells=vt_cells(&terminal);
    int cursor=show_cursor&&terminal.visible?(int)(terminal.y*terminal.cols+terminal.x):-1;
    struct gx_upload u;struct gx_fill f;
    if(painted_cursor>=0)terminal.dirty[painted_cursor/terminal.cols]=1;
    if(cursor>=0)terminal.dirty[cursor/terminal.cols]=1;
    if(native_text){int result=draw_native(cursor);painted_cursor=cursor;return result;}
    memset(&u,0,sizeof(u));u.version=GX_VERSION;u.surface=window.surface;u.height=16;u.pixels=row_pixels;
    memset(&f,0,sizeof(f));f.version=GX_VERSION;f.surface=window.surface;f.height=16;
    for(y=0;y<terminal.rows;y++)if(terminal.dirty[y]) {
        for(x=0;x<terminal.cols;x=end) {
            at=y*terminal.cols+x;end=x+1;
            if(!cell_changed(cells,at,cursor))continue;
            bg=palette[(cell_colour(cells,at)>>4)&15];
            if(cells[at].ch==' '&&(int)at!=cursor) {
                while(end<terminal.cols&&cells[y*terminal.cols+end].ch==' '&&
                    palette[(cell_colour(cells,y*terminal.cols+end)>>4)&15]==bg&&(int)(y*terminal.cols+end)!=cursor)end++;
                f.x=x*8;f.y=y*16;f.width=(end-x)*8;f.colour=bg;
                if(request(GX_FILL,&f))return -1;
            } else {
                while(end<terminal.cols&&cell_changed(cells,y*terminal.cols+end,cursor)&&cells[y*terminal.cols+end].ch!=' ')end++;
                span=(end-x)*8;
                for(at=x;at<end;at++) {
                    struct vt_cell *cell=cells+y*terminal.cols+at;
                    fg=palette[cell_colour(cells,y*terminal.cols+at)&15];bg=palette[(cell_colour(cells,y*terminal.cols+at)>>4)&15];
                    for(sy=0;sy<16;sy++) {
                        if(cell->ch<128)bits=gx_font[(unsigned)cell->ch*16+sy];
                        else if(cell->ch==RU_CONTINUATION)bits=at?ru_font_row(cells[y*terminal.cols+at-1].ch,sy)<<8:0;
                        else bits=ru_font_row(cell->ch,sy);
                        if((int)(y*terminal.cols+at)==cursor&&sy==15)bits=65535;
                        for(sx=0;sx<8;sx++)row_pixels[sy*span+(at-x)*8+sx]=(bits&(0x8000U>>sx))?fg:bg;
                    }
                }
                u.x=x*8;u.y=y*16;u.width=u.stride=span;
                if(request(GX_UPLOAD,&u))return -1;
            }
            memcpy(painted+y*terminal.cols+x,cells+y*terminal.cols+x,(end-x)*sizeof(*cells));
        }
        painted_rows[y]=1;terminal.dirty[y]=0;
    }
    painted_cursor=cursor;
    return 0;
}
static void scroll_pixels(struct vt *t,int amount) {
    unsigned n=amount<0?-amount:amount,h=(t->bottom-t->top+1-n)*16;
    struct gx_copy c;struct gx_fill f;
    if(native_text){unsigned y;for(y=t->top;y<=t->bottom;y++)t->dirty[y]=1;return;}
    /* Complete pending text without copying a software cursor into the scroll. */
    t->dirty[t->y]=1;if(draw(0))return;
    memset(&c,0,sizeof(c));c.version=GX_VERSION;c.surface=window.surface;
    c.width=t->cols*8;c.height=h;c.sy=(t->top+(amount>0?n:0))*16;c.dy=(t->top+(amount<0?n:0))*16;
    if(h&&request(GX_COPY,&c))return;
    memmove(painted+(t->top+(amount<0?n:0))*terminal.cols,
        painted+(t->top+(amount>0?n:0))*terminal.cols,(unsigned long)(h/16)*terminal.cols*sizeof(*painted));
    memmove(painted_rows+t->top+(amount<0?n:0),painted_rows+t->top+(amount>0?n:0),h/16);
    memset(painted_rows+(amount>0?t->bottom+1-n:t->top),0,n);
    memset(&f,0,sizeof(f));f.version=GX_VERSION;f.surface=window.surface;
    f.y=(amount>0?t->bottom+1-n:t->top)*16;f.width=t->cols*8;f.height=n*16;
    f.colour=palette[t->reverse?t->fg:t->bg];request(GX_FILL,&f);
}
static int down(unsigned key){return held[key>>4]&(1U<<(key&15));}
static void key(unsigned code) {
    unsigned shift=down(225)||down(229),ctrl=down(224)||down(228);char text[8]={0};unsigned c=0;
    static const char normal[]="1234567890",shifted[]="!@#$%^&*()";
    if(ctrl&&shift&&code==25){clipboard_action(GX_ACTION_PASTE);return;}
    if(ctrl&&shift&&code==6){clipboard_action(GX_ACTION_COPY);return;}
    if(code>=4&&code<=29)c=(shift?'A':'a')+code-4;
    else if(code>=30&&code<=39)c=(shift?shifted:normal)[code-30];
    else switch(code) {
        case 40:case 88:c='\r';break;case 41:c=27;break;case 42:c=127;break;case 43:c='\t';break;case 44:c=' ';break;
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
        char zero=0;clear_selection();if(vt_input_put(&pending,&zero,1))fputs("gterm: input queue full\n",stderr);return;
    }
    text[0]=c;
    if(down(226)||down(230))queue("\033");
    queue(text);
}
static void usage(void) {
    puts("usage: gterm [-g COLSxROWS] [--] [COMMAND [ARG...]]\n"
         "Open a graphical terminal (default sh, 80x30).\n"
         "--geometry COLSxROWS also sets the size, within this display's limits.\n"
         "Requires a graphical display. Example: gterm -g 64x20");
}
static int geometry(const char *s,unsigned *cols,unsigned *rows) {
    unsigned c=0,r=0;
    if(*s<'0'||*s>'9')return -1;
    while(*s>='0'&&*s<='9'){c=c*10+*s++-'0';if(c>GRAPHICS_COLS)return -1;}
    if(*s++!='x'||*s<'0'||*s>'9')return -1;
    while(*s>='0'&&*s<='9'){r=r*10+*s++-'0';if(r>GRAPHICS_ROWS)return -1;}
    if(*s||!c||!r)return -1;
    *cols=c;*rows=r;return 0;
}
static void resize_terminal(long width,long height) {
    struct gx_configure accepted;
    struct winsize size;
    unsigned cols,rows;
    if(width<=0||height<=0)return;
    vt_selection_clear(&selection);
    cols=width/cell_width;rows=height/cell_height;
    if(!cols)cols=1;if(!rows)rows=1;
    if(cols>max_cols)cols=max_cols;if(rows>max_rows)rows=max_rows;
    /* The model reserves both screens before changing dimensions. On failure
     * acknowledge the old geometry so a native gesture cannot kill the shell. */
    if(!vt_resize(&terminal,cols,rows)){
        memset(&size,0,sizeof(size));size.ws_col=cols;size.ws_row=rows;
        size.ws_xpixel=cols*cell_width;size.ws_ypixel=rows*cell_height;
        if(ioctl(master,TIOCSWINSZ,&size)<0){result=1;stopping=SIGTERM;return;}
    }
    accepted.version=GX_VERSION;accepted.window=window.window;
    accepted.width=terminal.cols*cell_width;accepted.height=terminal.rows*cell_height;accepted.flags=0;
    if(request(GX_CONFIGURE,&accepted))return;
    window.width=accepted.width;window.height=accepted.height;
    /* Give the foreground job one frame to respond to SIGWINCH, rather than
     * publishing the old screen's resized overlap followed by its repaint. */
    repaint_after=now_ms+30;
    memset(painted_rows,0,sizeof(painted_rows));painted_cursor=-1;vt_dirty(&terminal);
    if(trace_input)fprintf(stderr,"GTERM_SIZE %u %u\n",terminal.rows,terminal.cols);
}
int main(int argc,char **argv) {
    char path[]="/dev/ptyp0",slave[]="/dev/ttyp0",output[512];char *sh[]={"sh",NULL};
    unsigned i,cols=VT_DEFAULT_COLS,rows=VT_DEFAULT_ROWS;int n,status=0,dirty,command=1;
    struct winsize size;
    struct gx_handle present;struct gx_event event;
    struct gx_info info;
    fd_set reads,writes;struct timeval timeout;struct timespec clock;
    while(command<argc) {
        const char *option=argv[command];
        if(!strcmp(option,"--")){command++;break;}
        if(!strcmp(option,"--help")){usage();return 0;}
        if(!strcmp(option,"-g")||!strcmp(option,"--geometry")) {
            if(++command>=argc||geometry(argv[command],&cols,&rows)) {
                fputs("gterm: expected geometry COLSxROWS within 1x1..240x100\n",stderr);return 2;
            }
            command++;continue;
        }
        if(option[0]=='-'){fputs("gterm: unknown option (see --help)\n",stderr);return 2;}
        break;
    }
    if(vt_init_size(&terminal,cols,rows))return 2;
    size.ws_row=rows;size.ws_col=cols;
    trace_input=getenv("GTERM_TRACE_INPUT")!=NULL;
    gfx=open("/dev/gfx",O_RDWR|O_CLOEXEC);if(gfx<0){perror("gterm");return 1;}
    atexit(cleanup);
    memset(&info,0,sizeof(info));info.version=GX_VERSION;
    if(request(GX_INFO,&info))return 1;
    if(info.flags&GX_CAP_TERMINAL){
        struct gx_terminal_metrics m;m.version=GX_VERSION;
        if(request(GX_TERMINAL_METRICS,&m))return 1;
        if(!m.width||!m.height||m.width>64||m.height>128)return 1;
        cell_width=m.width;cell_height=m.height;native_text=1;
        text_row=malloc((unsigned long)GRAPHICS_COLS*67*sizeof(*text_row));if(!text_row)return 1;
    }
    size.ws_xpixel=cols*cell_width;size.ws_ypixel=rows*cell_height;
    max_cols=info.max_width/cell_width;max_rows=info.max_height/cell_height;
    if(max_cols>GRAPHICS_COLS)max_cols=GRAPHICS_COLS;if(max_rows>GRAPHICS_ROWS)max_rows=GRAPHICS_ROWS;
    if(cols>max_cols||rows>max_rows){fputs("gterm: geometry exceeds this display's limits\n",stderr);return 2;}
    if(!native_text){row_pixels=malloc((unsigned long)max_cols*8*16*sizeof(*row_pixels));if(!row_pixels){perror("gterm");return 1;}}
    memset(&window,0,sizeof(window));window.version=GX_VERSION;window.width=cols*cell_width;window.height=rows*cell_height;window.format=GX_RGB565;strcpy(window.title,"Terminal");
    if(request(GX_CREATE,&window)){perror("gterm");return 1;}
    for(i=0;i<RIPPLE_PTY_COUNT;i++){path[9]=slave[9]=RIPPLE_PTY_DIGIT(i);master=open(path,O_RDWR|O_NONBLOCK|O_NOCTTY);if(master>=0)break;if(errno!=EBUSY){perror("gterm");return 1;}}
    if(master<0){fputs("gterm: no free terminals\n",stderr);return 1;}
    /* Keep the slave's advertised erase character aligned with HID Backspace,
     * including applications that switch to raw mode and consult VERASE. */
    {
        struct termios settings;
        if(tcgetattr(master,&settings)<0){perror("gterm terminal");return 1;}
        settings.c_cc[VERASE]=127;
        if(tcsetattr(master,TCSANOW,&settings)<0){perror("gterm terminal");return 1;}
    }
    if(ioctl(master,TIOCSWINSZ,&size)<0){perror("gterm size");return 1;}child=fork();
    if(child<0){perror("gterm");return 1;}
    if(!child) {
        int fd;close(gfx);close(master);if(setsid()<0)_exit(125);
        fd=open(slave,O_RDWR);if(fd<0)_exit(125);dup2(fd,0);dup2(fd,1);dup2(fd,2);
        for(i=3;i<32;i++)close(i);
        if(putenv("TERM=ansi"))_exit(125);
        execvp(command<argc?argv[command]:"/bin/sh",command<argc?argv+command:sh);_exit(127);
    }
    terminal.scroll=scroll_pixels;terminal.reply=reply;
    signal(SIGINT,caught);signal(SIGTERM,caught);signal(SIGHUP,caught);
    present.version=GX_VERSION;present.handle=window.window;
    if(clock_gettime(CLOCK_MONOTONIC,&clock)<0)return 1;
    now_ms=clock.tv_sec*1000UL+clock.tv_nsec/1000000UL;
    while(!stopping) {
        /* Bound each batch so continuous output cannot starve window input. */
        for(i=0;i<8;i++) {
            n=read(master,output,sizeof(output));
            if(n>0){vt_feed(&terminal,output,n);}else {if(!n)stopping=SIGHUP;else if(errno!=EAGAIN&&errno!=EINTR){result=1;stopping=SIGHUP;}break;}
        }
        if(vt_selection_update(&terminal,&selection))invalidate();
        for(i=0;i<32;i++) {
            memset(&event,0,sizeof(event));event.version=GX_VERSION;event.window=window.window;
            if(request(GX_EVENT,&event)||!event.type)break;
            if(trace_input)fprintf(stderr,"gterm input: type=%u code=%u flags=%u\n",event.type,event.code,event.flags);
            if(event.type==GX_CLOSE){stopping=SIGHUP;break;}
            if(event.type==GX_ACTION){clipboard_action(event.code);continue;}
            if(event.type==GX_BUTTON&&event.code==0) {
                unsigned at=vt_selection_at(&terminal,event.x/cell_width,event.y/cell_height);
                if(event.flags&GX_DOWN)vt_selection_begin(&terminal,&selection,at);
                if(selection.active){selection.end=at;vt_selection_update(&terminal,&selection);if(!(event.flags&GX_DOWN))selection.dragging=0;}
                invalidate();continue;
            }
            if(event.type==GX_MOTION&&selection.dragging){selection.end=vt_selection_at(&terminal,event.x/cell_width,event.y/cell_height);invalidate();continue;}
            if(event.type==GX_RESIZE){resize_terminal(event.x,event.y);continue;}
            if(event.type==GX_OVERFLOW||(event.type==GX_FOCUS&&!(event.flags&GX_FOCUSED))){memset(held,0,sizeof(held));repeat_key=0;}
            if(event.type==GX_TEXT) {
                char encoded[5];unsigned n=ru_encode((uint32_t)event.dx,(unsigned char *)encoded);
                if(down(226)||down(230))queue("\033");
                encoded[n]=0;queue(encoded);
            }
            if(event.type==GX_KEY&&event.code<256) {
                unsigned bit=1U<<(event.code&15);
                if(event.flags&GX_DOWN){
                    held[event.code>>4]|=bit;if(!(event.flags&GX_TEXT_KEY))key(event.code);
                    if(event.code<224&&!(event.flags&GX_TEXT_KEY)){repeat_key=event.code;repeat_at=now_ms+500;}
                } else {held[event.code>>4]&=~bit;if(repeat_key==event.code)repeat_key=0;}
            }
        }
        if(repeat_key&&(long)(now_ms-repeat_at)>=0){key(repeat_key);repeat_at=now_ms+100;}
        if(pending.length) {
            n=write(master,pending.text+pending.head,pending.length>512?512:pending.length);
            if(n>0)vt_input_consume(&pending,n);
            else if(n<0&&errno!=EAGAIN&&errno!=EINTR){result=1;break;}
        }
        dirty=0;for(i=0;i<terminal.rows;i++)dirty|=terminal.dirty[i];
        if(!terminal.synchronized)sync_since=0;
        else if(!sync_since)sync_since=now_ms?now_ms:1;
        /* An interrupted writer cannot leave the screen frozen indefinitely. */
        if(terminal.synchronized&&(long)(now_ms-sync_since)>=1000){terminal.synchronized=0;if(vt_selection_update(&terminal,&selection))invalidate();}
        if(dirty&&!terminal.synchronized&&(!repaint_after||(long)(now_ms-repaint_after)>=0)){
            repaint_after=0;if(draw(1)||request(GX_PRESENT,&present))break;
        }
        if(waitpid(child,&status,WNOHANG)==child) {
            while((n=read(master,output,sizeof(output)))>0)vt_feed(&terminal,output,n);
            draw(1);request(GX_PRESENT,&present);child=0;break;
        }
        FD_ZERO(&reads);FD_ZERO(&writes);FD_SET(gfx,&reads);FD_SET(master,&reads);
        if(pending.length)FD_SET(master,&writes);
        timeout.tv_sec=repeat_key?0:1;timeout.tv_usec=repeat_key?100000UL:0;
        if(repaint_after&&(long)(repaint_after-now_ms)>0){timeout.tv_sec=0;timeout.tv_usec=(repaint_after-now_ms)*1000UL;}
        if(select((gfx>master?gfx:master)+1,&reads,&writes,NULL,&timeout)<0) {
            if(errno==EINTR)continue;result=1;break;
        }
        if(clock_gettime(CLOCK_MONOTONIC,&clock)<0){result=1;break;}
        now_ms=clock.tv_sec*1000UL+clock.tv_nsec/1000000UL;
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
