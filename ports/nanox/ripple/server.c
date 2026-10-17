/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bounded C16 local transport around the upstream Nano-X dispatcher.
 * Reads/writes never wait for another client. One select also waits for GX,
 * queued output and the nearest upstream monotonic timer. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "serv.h"
#include "nxproto.h"
#include "screen.h"
#include "server.h"
#define OUTPUT_CELLS 8192
#define CLIENT_LIMIT 12
struct peer {
    unsigned used, need, sent, length, opened, bad;
    char input[MAXREQUESTSZ], output[OUTPUT_CELLS];
};
static struct peer *peers[FD_SETSIZE];
static unsigned peer_count;
static volatile int stopping;
static unsigned held[16];
static MWKEYMOD modifiers;
static unsigned repeat_key;
static MWTIMEOUT repeat_at;
extern GR_CLIENT *root_client;
extern int un_sock;
static void caught(int sig){stopping=1;signal(sig,caught);}
void GdPlatformInit(void) {}
void GdBell(void) {}
void GdDelay(MWTIMEOUT ms){struct timeval t;t.tv_sec=ms/1000;t.tv_usec=(ms%1000)*1000;select(0,NULL,NULL,NULL,&t);}
MWTIMEOUT GdGetTickCount(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return 0;return t.tv_sec*1000UL+t.tv_nsec/1000000UL;}
void nxr_time(struct timeval *t){struct timespec s;clock_gettime(CLOCK_MONOTONIC,&s);t->tv_sec=s.tv_sec;t->tv_usec=s.tv_nsec/1000;}
void nxr_bad(int fd){if(fd>=0&&fd<FD_SETSIZE&&peers[fd])peers[fd]->bad=1;}
int nxr_open_socket(void){
    struct sockaddr_un addr;int probe,saved;
    memset(&addr,0,sizeof(addr));addr.sun_family=AF_UNIX;strcpy(addr.sun_path,GR_NAMED_SOCKET);
    un_sock=socket(AF_UNIX,SOCK_STREAM,0);if(un_sock<0)return -1;
    if(bind(un_sock,(struct sockaddr *)&addr,sizeof(addr))<0){
        if(errno!=EADDRINUSE)return -1;
        probe=socket(AF_UNIX,SOCK_STREAM,0);if(probe<0)return -1;
        if(connect(probe,(struct sockaddr *)&addr,sizeof(addr))==0){close(probe);return -1;}
        saved=errno;close(probe);
        if(saved!=ECONNREFUSED&&saved!=ENOENT)return -1;
        if(unlink(GR_NAMED_SOCKET)<0&&errno!=ENOENT)return -1;
        if(bind(un_sock,(struct sockaddr *)&addr,sizeof(addr))<0)return -1;
    }
    return listen(un_sock,CLIENT_LIMIT)<0?-1:1;
}
int nxr_accept(int fd){
    struct peer *p;
    if(fd<0||fd>=FD_SETSIZE||peer_count>=CLIENT_LIMIT)return -1;
    p=calloc(1,sizeof(*p));if(!p)return -1;
    p->need=sizeof(nxReq);peers[fd]=p;++peer_count;
    if(fcntl(fd,F_SETFL,O_NONBLOCK)<0){nxr_forget(fd);return -1;}
    return 0;
}
void nxr_forget(int fd){if(fd>=0&&fd<FD_SETSIZE&&peers[fd]){free(peers[fd]);peers[fd]=NULL;--peer_count;}}
int nxr_write(int fd,const void *data,int n){
    struct peer *p;
    if(fd<0||fd>=FD_SETSIZE||!(p=peers[fd])||n<0)return -1;
    if(p->sent){memmove(p->output,p->output+p->sent,p->length-p->sent);p->length-=p->sent;p->sent=0;}
    if((unsigned)n>OUTPUT_CELLS-p->length){p->bad=1;return -1;}
    memcpy(p->output+p->length,data,n);p->length+=n;return 0;
}
static void flush(int fd){
    struct peer *p=peers[fd];int n;
    if(!p||p->sent==p->length)return;
    n=write(fd,p->output+p->sent,p->length-p->sent);
    if(n>0){p->sent+=n;if(p->sent==p->length)p->sent=p->length=0;}
    else if(n==0||(errno!=EAGAIN&&errno!=EINTR))p->bad=1;
}
static GR_WINDOW *find_window(GR_WINDOW_ID id){GR_WINDOW *w;for(w=listwp;w;w=w->next)if(w->id==id)return w;return NULL;}
static int depth_ok(GR_WINDOW *parent){unsigned n=0;while(parent){if(++n>=32)return 0;parent=parent->parent;}return 1;}
static int resources(nxReq *r){
    unsigned n=0;GR_WINDOW *w,*parent,*node;GR_GC *g;GR_PIXMAP *p;GR_REGION *region;GR_TIMER *timer;GR_FONT *font;GR_CURSOR *cursor;
    switch(r->reqType){
        case GrNumNewWindow:case GrNumNewInputWindow:
            for(w=listwp;w;w=w->next)++n;
            return n<256&&depth_ok(find_window(((nxNewWindowReq *)r)->parentid));
        case GrNumNewGC:case GrNumCopyGC:for(g=listgcp;g;g=g->next)++n;return n<128;
        case GrNumNewPixmapEx:for(p=listpp;p;p=p->next)++n;return n<16;
        case GrNumCreateFontEx:for(font=listfontp;font;font=font->next)++n;return n<32;
        case GrNumNewCursor:for(cursor=listcursorp;cursor;cursor=cursor->next)++n;return n<64;
        case GrNumNewRegion:case GrNumNewBitmapRegion:for(region=listregionp;region;region=region->next)++n;return n<128;
        case GrNumCreateTimer:for(timer=list_timer;timer;timer=timer->next)++n;return n<128;
        case GrNumReparentWindow:{nxReparentWindowReq *q=(nxReparentWindowReq *)r;
            w=find_window(q->windowid);parent=find_window(q->parentid);if(!w||!parent)return 0;
            for(node=listwp;node;node=node->next){GR_WINDOW *walk=node;n=0;while(walk){if(++n>32)return 0;walk=walk==w?parent:walk->parent;}}
            return 1;}
    }return 1;
}
/* Only audited requests are enabled. Fixed requests must match exactly;
 * variable requests check their full payload before any upstream code sees it. */
static int valid(nxReq *r,unsigned n){
    unsigned size=0,extra;char *data;
    switch(r->reqType){
#include "requests.inc"
        default:return 0;
    }
    if(n<size)return 0;
    extra=n-size;data=(char *)r+size;
    switch(r->reqType){
        case GrNumOpen:return !extra&&((nxOpenReq *)r)->c16_magic==0x4e583101UL;
        case GrNumText:{nxTextReq *t=(nxTextReq *)r;return t->count>=0&&(unsigned)t->count==extra&&!(t->flags&(MWTF_UC16|MWTF_UC32|MWTF_XCHAR2B));}
        case GrNumGetGCTextSize:{nxGetGCTextSizeReq *t=(nxGetGCTextSizeReq *)r;return t->charcount==extra&&!(t->flags&MWTF_PACKMASK);}
        case GrNumCreateFontEx:return extra&&extra<=128&&memchr(data,0,extra)!=NULL;
        case GrNumSetWMProperties:
            return extra>=sizeof(GR_WM_PROPERTIES)&&extra<=sizeof(GR_WM_PROPERTIES)+128&&
                (extra==sizeof(GR_WM_PROPERTIES)||memchr(data+sizeof(GR_WM_PROPERTIES),0,extra-sizeof(GR_WM_PROPERTIES))!=NULL);
        case GrNumBitmap:{nxBitmapReq *b=(nxBitmapReq *)r;return b->width>0&&b->width<=640&&b->height>0&&b->height<=480&&extra==(unsigned long)((b->width+15)/16)*b->height;}
        case GrNumNewBitmapRegion:{nxNewBitmapRegionReq *b=(nxNewBitmapRegionReq *)r;return b->width>0&&b->width<=640&&b->height>0&&b->height<=480&&extra==(unsigned long)((b->width+15)/16)*b->height;}
        case GrNumSetWindowRegion:return !extra&&((nxSetWindowRegionReq *)r)->type==GR_WINDOW_BOUNDING_MASK;
        case GrNumNewCursor:{nxNewCursorReq *c=(nxNewCursorReq *)r;return c->width>0&&c->width<=MWMAX_CURSOR_SIZE&&c->height>0&&c->height<=MWMAX_CURSOR_SIZE&&extra==2UL*((c->width+15)/16)*c->height;}
        default:return !extra;
    }
}
void nxr_receive(int fd){
    struct peer *p=peers[fd];nxReq *r;int got;unsigned budget=16;
    while(p&&!p->bad&&budget--){
        got=read(fd,p->input+p->used,p->need-p->used);
        if(got==0){p->bad=1;break;}
        if(got<0){if(errno!=EAGAIN&&errno!=EINTR)p->bad=1;break;}
        p->used+=got;if(p->used<p->need)continue;
        r=(nxReq *)p->input;
        if(p->need==sizeof(nxReq)){
            if(r->hilength||r->length<sizeof(nxReq)||r->length>MAXREQUESTSZ){p->bad=1;break;}
            p->need=r->length;if(p->used<p->need)continue;
        }
        if(!valid(r,p->used)||(!p->opened&&r->reqType!=GrNumOpen)||(p->opened&&r->reqType==GrNumOpen)){
            if(getenv("NANOX_TRACE"))fprintf(stderr,"Nano-X rejected fd=%d type=%u cells=%u opened=%u\n",fd,r->reqType,p->used,p->opened);
            p->bad=1;break;
        }
        if(!resources(r)){
            if(r->reqType==GrNumReparentWindow){p->bad=1;break;}
            /* Valid allocation requests report a zero resource ID on exhaustion,
             * as the upstream API does. Preserve the connection and its objects. */
            {short type=r->reqType;GR_ID none=0;nxr_write(fd,&type,sizeof(type));nxr_write(fd,&none,sizeof(none));}
            p->used=0;p->need=sizeof(nxReq);continue;
        }
        p->opened=1;
        if(getenv("NANOX_TRACE_WIRE"))fprintf(stderr,"NXREQ %d %u %u\n",fd,r->reqType,p->used);
        nxr_dispatch(fd,r);
        if(peers[fd]!=p)return;
        p->used=0;p->need=sizeof(nxReq);
    }
}
/* GX transports HID usages. Nano-X clients receive its portable key symbols,
 * ASCII characters and modifiers, never host keycodes. */
static MWKEY key(unsigned code){
    int shift=held[225>>4]&(1U<<(225&15))||held[229>>4]&(1U<<(229&15));
    static const char digits[]="1234567890", shifted[]="!@#$%^&*()";
    static const MWKEY special[]={MWKEY_ENTER,MWKEY_ESCAPE,MWKEY_BACKSPACE,MWKEY_TAB,' '};
    if(code>=4&&code<=29)return (shift?'A':'a')+code-4;
    if(code>=30&&code<=39)return (shift?shifted:digits)[code-30];
    if(code>=40&&code<=44)return special[code-40];
    if(code>=58&&code<=69)return MWKEY_F1+code-58;
    switch(code){
        case 45:return shift?'_':'-';case 46:return shift?'+':'=';case 47:return shift?'{':'[';
        case 48:return shift?'}':']';case 49:return shift?'|':'\\';case 51:return shift?':':';';
        case 52:return shift?'"':'\'';case 53:return shift?'~':'`';case 54:return shift?'<':',';
        case 55:return shift?'>':'.';case 56:return shift?'?':'/';
        case 73:return MWKEY_INSERT;case 74:return MWKEY_HOME;case 75:return MWKEY_PAGEUP;
        case 76:return MWKEY_DELETE;case 77:return MWKEY_END;case 78:return MWKEY_PAGEDOWN;
        case 79:return MWKEY_RIGHT;case 80:return MWKEY_LEFT;case 81:return MWKEY_DOWN;case 82:return MWKEY_UP;
    }return 0;
}
static void input(void){
    struct gx_event e;MWKEY k;unsigned i,buttons;
    while(nxr_event(&e)==0&&e.type){
        if(e.type==GX_CLOSE){stopping=1;break;}
        if(e.type==GX_KEY&&e.code<256){
            if(e.flags&GX_DOWN)held[e.code>>4]|=1U<<(e.code&15);else held[e.code>>4]&=~(1U<<(e.code&15));
            modifiers=0;
            if((held[14]&((1U<<1)|(1U<<5))))modifiers|=MWKMOD_SHIFT;
            if((held[14]&((1U<<0)|(1U<<4))))modifiers|=MWKMOD_CTRL;
            if((held[14]&((1U<<2)|(1U<<6))))modifiers|=MWKMOD_ALT;
            k=key(e.code);
            if(getenv("NANOX_TRACE_WIRE"))fprintf(stderr,"NXINPUT %u %u %u focus=%u\n",e.code,e.flags,k,focuswp->id);
            if(k&&(e.flags&GX_DOWN)){repeat_key=e.code;repeat_at=GdGetTickCount()+500;}
            else if(!(e.flags&GX_DOWN)&&repeat_key==e.code)repeat_key=0;
            if(k)GsDeliverKeyboardEvent(0,(e.flags&GX_DOWN)?GR_EVENT_TYPE_KEY_DOWN:GR_EVENT_TYPE_KEY_UP,k,modifiers,e.code);
        }else if(e.type==GX_MOTION||e.type==GX_BUTTON||e.type==GX_WHEEL){
            if(e.type==GX_WHEEL&&!e.wheel_y)continue;
            buttons=(e.buttons&1?GR_BUTTON_L:0)|(e.buttons&2?GR_BUTTON_R:0)|(e.buttons&4?GR_BUTTON_M:0);
            if(e.type==GX_WHEEL)buttons|=e.wheel_y>0?GR_BUTTON_SCROLLUP:GR_BUTTON_SCROLLDN;
            GsHandleMouseStatus(e.x,e.y,buttons);
            if(e.type==GX_WHEEL)GsHandleMouseStatus(e.x,e.y,buttons&~(GR_BUTTON_SCROLLUP|GR_BUTTON_SCROLLDN));
        }else if(e.type==GX_FOCUS||e.type==GX_OVERFLOW){
            if(e.type==GX_OVERFLOW||!(e.flags&GX_FOCUSED)){
                for(i=0;i<256;i++)if(held[i>>4]&(1U<<(i&15))){k=key(i);if(k)GsDeliverKeyboardEvent(0,GR_EVENT_TYPE_KEY_UP,k,0,i);}
                memset(held,0,sizeof(held));modifiers=0;repeat_key=0;GsHandleMouseStatus(cursorx,cursory,0);
            }
        }
    }
}
void nxr_select(GR_TIMEOUT timeout){
    fd_set readset,writeset;struct timeval tv,*wait=NULL;GR_CLIENT *c;int fd,maxfd,ready;
    static int started;
    if(!started){started=1;signal(SIGTERM,caught);signal(SIGINT,caught);fcntl(un_sock,F_SETFL,O_NONBLOCK);puts("Nano-X ready");
        {char *ready=getenv("NANOX_READY_FD");if(ready&&*ready){int out=atoi(ready);write(out,"R",1);close(out);}}}
    input();
    if(stopping)GsTerminate();
    if(repeat_key&&(long)(GdGetTickCount()-repeat_at)>=0){
        GsDeliverKeyboardEvent(0,GR_EVENT_TYPE_KEY_DOWN,key(repeat_key),modifiers,repeat_key);
        repeat_at=GdGetTickCount()+50;
    }
    /* Drain upstream event queues fairly before waiting. */
    for(c=root_client;c;c=c->next)if(c->waiting_for_event&&c->eventhead){curclient=c;c->waiting_for_event=0;GrGetNextEventWrapperFinish(c->id);}
    for(fd=0;fd<FD_SETSIZE;fd++)if(peers[fd]){if(peers[fd]->bad)GsClose(fd);else {flush(fd);if(peers[fd]&&peers[fd]->bad)GsClose(fd);}}
    if(nxr_present()<0){stopping=1;return;}
    FD_ZERO(&readset);FD_ZERO(&writeset);maxfd=un_sock;FD_SET(un_sock,&readset);
    fd=nxr_descriptor();if(fd>=0){FD_SET(fd,&readset);if(fd>maxfd)maxfd=fd;}
    for(fd=0;fd<FD_SETSIZE;fd++)if(peers[fd]){
        FD_SET(fd,&readset);if(peers[fd]->length>peers[fd]->sent)FD_SET(fd,&writeset);if(fd>maxfd)maxfd=fd;
    }
    if(GdGetNextTimeout(&tv,timeout))wait=&tv;
    if(repeat_key){
        long remaining=(long)(repeat_at-GdGetTickCount());if(remaining<0)remaining=0;
        if(!wait||tv.tv_sec*1000UL+tv.tv_usec/1000UL>(unsigned long)remaining){
            tv.tv_sec=remaining/1000;tv.tv_usec=(remaining%1000)*1000;wait=&tv;
        }
    }
    ready=select(maxfd+1,&readset,&writeset,NULL,wait);
    if(ready<0&&errno!=EINTR){stopping=1;return;}
    if(ready>0){
        fd=nxr_descriptor();if(fd>=0&&FD_ISSET(fd,&readset))input();
        if(FD_ISSET(un_sock,&readset))GsAcceptClient();
        for(fd=0;fd<FD_SETSIZE;fd++)if(peers[fd]){
            if(FD_ISSET(fd,&writeset))flush(fd);
            if(peers[fd]&&FD_ISSET(fd,&readset))nxr_receive(fd);
        }
    }
    GdTimeout();
}
/* Input is delivered by the GX event pump, so no separate driver FD is needed. */
static int no_keyboard(KBDDEVICE *d){return -2;}static void no_close(void){}
static int no_key(MWKEY *k,MWKEYMOD *m,MWSCANCODE *s){return 0;}
static void keymods(MWKEYMOD *all,MWKEYMOD *cur){if(all)*all=MWKMOD_SHIFT|MWKMOD_CTRL|MWKMOD_ALT;if(cur)*cur=modifiers;}
KBDDEVICE kbddev={no_keyboard,no_close,keymods,no_key};
static int no_mouse(MOUSEDEVICE *d){return -3;}static int buttons(void){return GR_BUTTON_L|GR_BUTTON_R|GR_BUTTON_M;}
static void accel(int *s,int *t){*s=1;*t=0;}static int no_motion(MWCOORD *x,MWCOORD *y,MWCOORD *z,int *b){return 0;}
MOUSEDEVICE mousedev={no_mouse,no_close,buttons,accel,no_motion};
