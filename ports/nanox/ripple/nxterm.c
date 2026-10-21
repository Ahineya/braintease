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
#include "nx_clipboard.h"
#include "../../../ports/graphics/terminal-input.h"
#include "../../../ports/graphics/terminal-selection.h"
#include "pty-limits.h"
#include "../../../ports/graphics/terminal.h"
#include "../../../ports/graphics/terminal.c"
#include "../../../libraries/unicode/font.c"
#define GRAPHICS_COLS VT_MAX_COLS
#define GRAPHICS_ROWS VT_MAX_ROWS
static struct vt terminal;
static GR_WINDOW_ID window;
static GR_GC_ID gc;
static int master=-1,child,connected;
static volatile int stopping;
static struct vt_input pending;
static struct vt_selection selection;
static const GR_COLOR palette[16]={
    GR_RGB(0,0,0),GR_RGB(168,0,0),GR_RGB(0,168,0),GR_RGB(168,168,0),
    GR_RGB(0,0,168),GR_RGB(168,0,168),GR_RGB(0,168,168),GR_RGB(168,168,168),
    GR_RGB(84,84,84),GR_RGB(255,84,84),GR_RGB(84,255,84),GR_RGB(255,255,84),
    GR_RGB(84,84,255),GR_RGB(255,84,255),GR_RGB(84,255,255),GR_RGB(255,255,255)};
static void caught(int s){stopping=s;signal(s,caught);}
static void clear_selection(void);
static void queue(const char *s){clear_selection();if(vt_input_put(&pending,s,strlen(s)))fputs("nxterm: input queue full\n",stderr);}
static void reply(struct vt *t,const char *s){if(vt_input_put(&pending,s,strlen(s)))fputs("nxterm: input queue full\n",stderr);}
/* Last rendered cells let a keystroke repaint a short span, not 64 glyphs. */
static struct vt_cell painted[GRAPHICS_COLS*GRAPHICS_ROWS];
static unsigned char known[GRAPHICS_ROWS];
static unsigned cursor_x,cursor_y,cursor_shown;
static void invalidate(void){unsigned i;for(i=0;i<terminal.rows;i++){terminal.dirty[i]=1;known[i]=0;}}
static void clear_selection(void){if(selection.active){vt_selection_clear(&selection);invalidate();}}
#define CHANGED(x,y) (!known[y]||cells[(y)*terminal.cols+(x)].ch!=painted[(y)*terminal.cols+(x)].ch||cells[(y)*terminal.cols+(x)].colour!=painted[(y)*terminal.cols+(x)].colour||(cursor_shown&&moved&&(x)==cursor_x&&(y)==cursor_y))
static int draw_cells(int show_cursor){
    unsigned y,x,end,colour,changed=0,moved=cursor_shown!=((unsigned)show_cursor&&terminal.visible)||cursor_x!=terminal.x||cursor_y!=terminal.y;
    char text[GRAPHICS_COLS];struct vt_cell *cells=vt_cells(&terminal);
    for(y=0;y<terminal.rows;y++)if(terminal.dirty[y]||(moved&&cursor_shown&&y==cursor_y)){
        x=0;while(x<terminal.cols){
            if(!CHANGED(x,y)){x++;continue;}
            colour=vt_selection_colour(&selection,y*terminal.cols+x,cells[y*terminal.cols+x].colour);
            GrSetGCForeground(gc,palette[colour&15]);GrSetGCBackground(gc,palette[(colour>>4)&15]);
            if(cells[y*terminal.cols+x].ch>=128) {
                GR_BITMAP rows[16];unsigned r;uint32_t ch=cells[y*terminal.cols+x].ch;
                for(r=0;r<16;r++)rows[r]=ch==RU_CONTINUATION?(x?ru_font_row(cells[y*terminal.cols+x-1].ch,r)<<8:0):ru_font_row(ch,r);
                GrBitmap(window,gc,x*8,y*16,8,16,rows);
                painted[y*terminal.cols+x]=cells[y*terminal.cols+x];end=x+1;
            } else {
                end=x;
                while(end<terminal.cols&&vt_selection_colour(&selection,y*terminal.cols+end,cells[y*terminal.cols+end].colour)==colour&&CHANGED(end,y)&&cells[y*terminal.cols+end].ch<128){
                    text[end-x]=(unsigned)cells[y*terminal.cols+end].ch;
                    painted[y*terminal.cols+end]=cells[y*terminal.cols+end];end++;
                }
                GrText(window,gc,x*8,y*16,text,end-x,GR_TFASCII|GR_TFTOP);
            }
            x=end;changed=1;
        }
        terminal.dirty[y]=0;known[y]=1;
    }
    cursor_shown=show_cursor&&terminal.visible;cursor_x=terminal.x;cursor_y=terminal.y;
    if(cursor_shown&&(changed||moved)){
        GrSetGCForeground(gc,palette[cells[terminal.y*terminal.cols+terminal.x].colour&15]);
        GrLine(window,gc,terminal.x*8,terminal.y*16+15,terminal.x*8+7,terminal.y*16+15);
    }
    return changed||moved;
}
#undef CHANGED
static void draw(void){if(draw_cells(1))GrFlush();}
static void scroll_pixels(struct vt *t,int amount){
    unsigned n=amount<0?-amount:amount,rows=t->bottom-t->top+1,from,to,y;
    /* Apply pending characters and erase the cursor before copying old pixels. */
    draw_cells(0);
    if(n<rows){
        from=t->top+(amount>0?n:0);to=t->top+(amount<0?n:0);
        GrCopyArea(window,gc,0,to*16,t->cols*8,(rows-n)*16,window,0,from*16,MWROP_COPY);
        memmove(painted+to*t->cols,painted+from*t->cols,(unsigned long)(rows-n)*t->cols*sizeof(*painted));
    }
    from=amount>0?t->bottom+1-n:t->top;
    for(y=from;y<from+n;y++){known[y]=0;t->dirty[y]=1;}
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
    if(k==NXR_KEY_PASTE||(e->modifiers&(MWKMOD_CTRL|MWKMOD_SHIFT))==(MWKMOD_CTRL|MWKMOD_SHIFT)&&(k=='v'||k=='V')) {
        clear_selection();
        size_t length;char *text=nx_clipboard_get(window,&length);
        if(text){if(vt_input_paste(&pending,text,length,terminal.bracketed_paste))fputs("nxterm: paste too large or out of memory\n",stderr);free(text);}
        return;
    }
    if(k==NXR_KEY_SELECT_ALL){if(!vt_selection_begin(&terminal,&selection,0)){selection.dragging=0;selection.end=terminal.cols*terminal.rows-1;}invalidate();return;}
    if(k==NXR_KEY_COPY||k==NXR_KEY_CUT||((e->modifiers&(MWKMOD_CTRL|MWKMOD_SHIFT))==(MWKMOD_CTRL|MWKMOD_SHIFT)&&(k=='c'||k=='C'))) {
        size_t length;char *text=vt_selection_text(&terminal,&selection,&length);
        if(text){if(nx_clipboard_set(window,text,length))fputs("nxterm: cannot copy selection\n",stderr);free(text);}return;
    }
    if(k>255)return;
    if(e->modifiers&MWKMOD_CTRL){if((k>='a'&&k<='z')||(k>='@'&&k<='_'))k&=31;else if(k==' ')k=0;}
    if(e->modifiers&MWKMOD_ALT)queue("\033");
    if(k){s[0]=k;queue(s);}else {char zero=0;clear_selection();if(vt_input_put(&pending,&zero,1))fputs("nxterm: input queue full\n",stderr);}
}
static void report_mouse(unsigned button,int x,int y,int released,int motion,unsigned modifiers) {
    if(motion&&pending.length>VT_INPUT_MAX-128)return;
    char text[40];unsigned n=vt_mouse_report(&terminal,text,button,x,y,released,motion,modifiers);
    if(n&&vt_input_put(&pending,text,n))fputs("nxterm: mouse input queue full\n",stderr);
}
static void cancel_mouse(void) {
    unsigned i;for(i=0;i<3;i++)if(terminal.mouse_buttons&(1U<<i))report_mouse(i,terminal.mouse_x,terminal.mouse_y,1,0,0);
    terminal.mouse_buttons=terminal.mouse_known=0;selection.dragging=0;
}
static int application_mouse(GR_EVENT *e) {
    unsigned mods,bits,button,i;int x,y,motion=e->type==GR_EVENT_TYPE_MOUSE_POSITION;
    if(!motion&&e->type!=GR_EVENT_TYPE_BUTTON_DOWN&&e->type!=GR_EVENT_TYPE_BUTTON_UP)return 0;
    mods=motion?e->mouse.modifiers:e->button.modifiers;
    if(!terminal.mouse_mode||selection.dragging||((mods&MWKMOD_SHIFT)&&!terminal.mouse_buttons))return 0;
    x=(motion?e->mouse.x:e->button.x)/8;y=(motion?e->mouse.y:e->button.y)/16;
    mods=(mods&MWKMOD_SHIFT?4:0)|(mods&MWKMOD_ALT?8:0)|(mods&MWKMOD_CTRL?16:0);
    if(motion){button=terminal.mouse_buttons&1?0:terminal.mouse_buttons&2?1:terminal.mouse_buttons&4?2:3;report_mouse(button,x,y,0,1,mods);}
    else {
        bits=e->button.changebuttons;
        for(i=0;i<5;i++) {
            button=i==0?GR_BUTTON_L:i==1?GR_BUTTON_M:i==2?GR_BUTTON_R:i==3?GR_BUTTON_SCROLLUP:GR_BUTTON_SCROLLDN;
            if(bits&button){if(e->type==GR_EVENT_TYPE_BUTTON_DOWN)clear_selection();report_mouse(i<3?i:i==3?64:65,x,y,e->type==GR_EVENT_TYPE_BUTTON_UP,0,mods);}
        }
    }
    return 1;
}
static void event(GR_EVENT *e){
    struct winsize size;unsigned cols,rows;
    if(application_mouse(e))return;
    if(e->type==GR_EVENT_TYPE_EXPOSURE)invalidate();
    else if(e->type==GR_EVENT_TYPE_KEY_DOWN)key(&e->keystroke);
    else if((e->type==GR_EVENT_TYPE_BUTTON_DOWN||e->type==GR_EVENT_TYPE_BUTTON_UP)&&(e->button.changebuttons&GR_BUTTON_L)) {
        unsigned at=vt_selection_at(&terminal,e->button.x/8,e->button.y/16);
        if(e->type==GR_EVENT_TYPE_BUTTON_DOWN)vt_selection_begin(&terminal,&selection,at);
        if(selection.active){selection.end=at;vt_selection_update(&terminal,&selection);if(e->type==GR_EVENT_TYPE_BUTTON_UP)selection.dragging=0;}invalidate();
    } else if(e->type==GR_EVENT_TYPE_MOUSE_POSITION&&selection.dragging) {
        selection.end=vt_selection_at(&terminal,e->mouse.x/8,e->mouse.y/16);invalidate();
    }
    else if(e->type==GR_EVENT_TYPE_CLOSE_REQ)stopping=SIGHUP;
    else if(e->type==GR_EVENT_TYPE_FOCUS_OUT)cancel_mouse();
    else if(e->type==GR_EVENT_TYPE_UPDATE&&e->update.utype==GR_UPDATE_SIZE){
        cancel_mouse();
        vt_selection_clear(&selection);
        cols=e->update.width/8;rows=e->update.height/16;
        if(cols>GRAPHICS_COLS)cols=GRAPHICS_COLS;if(rows>GRAPHICS_ROWS)rows=GRAPHICS_ROWS;
        if(!cols)cols=1;if(!rows)rows=1;
        if(vt_resize(&terminal,cols,rows)){GrResizeWindow(window,terminal.cols*8,terminal.rows*16);return;}
        cursor_shown=0;memset(&size,0,sizeof(size));
        size.ws_col=cols;size.ws_row=rows;size.ws_xpixel=cols*8;size.ws_ypixel=rows*16;
        ioctl(master,TIOCSWINSZ,&size);
        if(getenv("NANOX_TRACE"))fprintf(stderr,"NXTERM_SIZE %d %u %u\n",getpid(),rows,cols);
        GrSetGCForeground(gc,palette[0]);
        GrFillRect(window,gc,0,0,e->update.width,e->update.height);invalidate();
    }
}
static void cleanup(void){
    vt_selection_clear(&selection);vt_destroy(&terminal);free(pending.text);
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
    if(vt_init_size(&terminal,cols,rows))return 1;terminal.reply=reply;terminal.scroll=scroll_pixels;
    window=GrNewWindow(GR_ROOT_WINDOW_ID,24+(getpid()%4)*16,32+(getpid()%4)*16,cols*8,rows*16,0,GR_RGB(0,0,0),0);
    if(!window)return 1;
    memset(&props,0,sizeof(props));props.flags=GR_WM_FLAGS_TITLE|GR_WM_FLAGS_PROPS;
    props.title="Terminal";props.props=GR_WM_PROPS_APPWINDOW;
    GrSetWMProperties(window,&props);gc=GrNewGC();GrSetGCUseBackground(gc,GR_TRUE);GrSetGCGraphicsExposure(gc,GR_FALSE);
    GrSelectEvents(window,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_KEY_DOWN|GR_EVENT_MASK_CLOSE_REQ|GR_EVENT_MASK_UPDATE|GR_EVENT_MASK_BUTTON_DOWN|GR_EVENT_MASK_BUTTON_UP|GR_EVENT_MASK_MOUSE_POSITION|GR_EVENT_MASK_FOCUS_OUT);
    for(i=0;i<RIPPLE_PTY_COUNT;i++){path[9]=slave[9]=RIPPLE_PTY_DIGIT(i);master=open(path,O_RDWR|O_NONBLOCK|O_NOCTTY);if(master>=0)break;if(errno!=EBUSY){perror("nxterm");return 1;}}
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
        if(vt_selection_update(&terminal,&selection))invalidate();
        if(pending.length){n=write(master,pending.text+pending.head,pending.length>512?512:pending.length);if(n>0){vt_input_consume(&pending,n);}else if(n<0&&errno!=EAGAIN&&errno!=EINTR)stopping=SIGTERM;}
        draw();
        if(waitpid(child,&status,WNOHANG)==child){child=0;break;}
        FD_ZERO(&reads);FD_ZERO(&writes);maxfd=master;FD_SET(master,&reads);if(pending.length)FD_SET(master,&writes);
        GrPrepareSelect(&maxfd,&reads);wait.tv_sec=1;wait.tv_usec=0;
        n=select(maxfd+1,&reads,&writes,NULL,&wait);
        if(n>0)GrServiceSelect(&reads,event);else if(n<0&&errno!=EINTR){stopping=SIGTERM;break;}
    }
    return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
