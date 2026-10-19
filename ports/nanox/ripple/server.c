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
#include <ripple_unicode.h>
#include "nx_clipboard.h"
#define OUTPUT_CELLS 8192
#define CLIENT_LIMIT 12
struct peer {
    unsigned used, sent, length, opened, bad, direct;
    char input[2*MAXREQUESTSZ], output[OUTPUT_CELLS];
};
static struct peer *peers[FD_SETSIZE];
static unsigned peer_count;
static int trace_wire,trace_reject;
static volatile int stopping;
static int wakefd[2]={-1,-1};
static unsigned held[16];
static MWKEYMOD modifiers;
static unsigned repeat_key;
static MWTIMEOUT repeat_at;
extern GR_CLIENT *root_client;
extern int un_sock;
static void caught(int sig){
    int saved=errno;char byte=1;stopping=1;signal(sig,caught);
    if(wakefd[1]>=0)write(wakefd[1],&byte,1);
    errno=saved;
}
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
    peers[fd]=p;++peer_count;
    if(fcntl(fd,F_SETFL,O_NONBLOCK)<0){nxr_forget(fd);return -1;}
    return 0;
}
void nxr_forget(int fd){nxr_draw_target(NULL);nxr_workspace_forget(fd);nxr_image_forget(fd);nxr_clipboard_forget(fd);if(fd>=0&&fd<FD_SETSIZE&&peers[fd]){free(peers[fd]);peers[fd]=NULL;--peer_count;}}
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
        case GrNumNewPixmapEx:n=nxr_image_count();for(p=listpp;p;p=p->next)if(!nxr_internal_pixmap(p))++n;return n<16;
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
    if(r->reqType==251)return nxr_clipboard_valid(r,n);
    if(r->reqType==252)return nxr_workspace_valid(r,n);
    if(r->reqType==253)return nxr_image_valid(r,n);
    switch(r->reqType){
        case NXR_COMMIT:size=sizeof(nxReq);break;
#include "requests.inc"
        default:return 0;
    }
    if(n<size)return 0;
    extra=n-size;data=(char *)r+size;
    switch(r->reqType){
        case GrNumOpen:return !extra&&((nxOpenReq *)r)->c16_magic==0x4e583102UL&&((nxOpenReq *)r)->c16_direct<=1;
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
/* One read stages many packets. Count completed requests, not header/body reads.
 * The caller revisits staged packets without sleeping, with a per-client quota. */
static int staged(struct peer *p){
    nxReq *r=(nxReq *)p->input;
    return p->used>=sizeof(nxReq)&&(r->length<sizeof(nxReq)||r->length>MAXREQUESTSZ||r->hilength||p->used>=r->length);
}
void nxr_receive(int fd){
    struct peer *p=peers[fd];nxReq *r;int got;unsigned budget=32,at=0,n;
    if(!p||p->bad)return;
    if(p->used<sizeof(p->input)){
        got=read(fd,p->input+p->used,sizeof(p->input)-p->used);
        if(got==0){p->bad=1;return;}
        if(got<0){if(errno!=EAGAIN&&errno!=EINTR){p->bad=1;return;}}
        else p->used+=got;
    }
    while(!p->bad&&budget--&&p->used-at>=sizeof(nxReq)){
        r=(nxReq *)(p->input+at);n=r->length;
        if(r->hilength||n<sizeof(nxReq)||n>MAXREQUESTSZ){p->bad=1;break;}
        if(p->used-at<n)break;
        if(!valid(r,n)||(!p->opened&&r->reqType!=GrNumOpen)||(p->opened&&r->reqType==GrNumOpen)){
            if(trace_reject)fprintf(stderr,"Nano-X rejected fd=%d type=%u cells=%u opened=%u\n",fd,r->reqType,n,p->opened);
            p->bad=1;break;
        }
        if(!resources(r)){
            if(trace_reject)fprintf(stderr,"Nano-X quota fd=%d type=%u\n",fd,r->reqType);
            if(r->reqType==GrNumReparentWindow){p->bad=1;break;}
            {short type=r->reqType;GR_ID none=0;nxr_write(fd,&type,sizeof(type));nxr_write(fd,&none,sizeof(none));}
        }else{
            if(r->reqType==GrNumOpen)p->direct=((nxOpenReq *)r)->c16_direct;
            p->opened=1;
            if(trace_wire)fprintf(stderr,"NXREQ %d %u %u\n",fd,r->reqType,n);
            nxr_dispatch(fd,r);
            if(peers[fd]!=p)return;
        }
        at+=n;
    }
    if(at){p->used-=at;memmove(p->input,p->input+at,p->used);}
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
        case 88:return MWKEY_ENTER;
    }return 0;
}
static void input(void){
    struct gx_event e;MWKEY k;unsigned i,buttons;
    while(nxr_event(&e)==0&&e.type){
        if(e.type==GX_CLOSE){stopping=1;break;}
        if(e.type==GX_ACTION&&e.code>=GX_ACTION_COPY&&e.code<=GX_ACTION_SELECT_ALL){
            GsDeliverKeyboardEvent(0,GR_EVENT_TYPE_KEY_DOWN,NXR_KEY_COPY+e.code-1,0,0);
        }else if(e.type==GX_KEY&&e.code<256){
            if(e.flags&GX_DOWN)held[e.code>>4]|=1U<<(e.code&15);else held[e.code>>4]&=~(1U<<(e.code&15));
            modifiers=0;
            if((held[14]&((1U<<1)|(1U<<5))))modifiers|=MWKMOD_SHIFT;
            if((held[14]&((1U<<0)|(1U<<4))))modifiers|=MWKMOD_CTRL;
            if((held[14]&((1U<<2)|(1U<<6))))modifiers|=MWKMOD_ALT;
            k=(e.flags&GX_TEXT_KEY)?0:key(e.code);
            if(trace_wire)fprintf(stderr,"NXINPUT %u %u %u focus=%u\n",e.code,e.flags,k,focuswp->id);
            if(k&&(e.flags&GX_DOWN)){repeat_key=e.code;repeat_at=GdGetTickCount()+500;}
            else if(!(e.flags&GX_DOWN)&&repeat_key==e.code)repeat_key=0;
            if(k&&(e.flags&GX_DOWN)&&nxr_workspace_key(k,modifiers)){repeat_key=0;continue;}
            if(k)GsDeliverKeyboardEvent(0,(e.flags&GX_DOWN)?GR_EVENT_TYPE_KEY_DOWN:GR_EVENT_TYPE_KEY_UP,k,modifiers,e.code);
        }else if(e.type==GX_TEXT){
            /* Preserve the legacy 16-bit Nano-X key ABI: printable non-ASCII
             * text is an ordered UTF-8 byte stream, never KEY_* symbols. */
            unsigned char bytes[4];unsigned n=ru_encode((uint32_t)e.dx,bytes);
            if(e.dx<128&&nxr_workspace_key((MWKEY)e.dx,modifiers)){repeat_key=0;continue;}
            /* Text is already shifted/layout-resolved. Prefix Alt once for
             * the complete encoding, not once per continuation octet. */
            for(i=0;i<n;i++)GsDeliverKeyboardEvent(0,GR_EVENT_TYPE_KEY_DOWN,bytes[i],i?0:modifiers&MWKMOD_ALT,0);
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
    if(!started){started=1;
        if(pipe(wakefd)<0||fcntl(wakefd[0],F_SETFL,O_NONBLOCK)<0||fcntl(wakefd[1],F_SETFL,O_NONBLOCK)<0)GsTerminate();
        trace_wire=getenv("NANOX_TRACE_WIRE")!=NULL;trace_reject=getenv("NANOX_TRACE")!=NULL;signal(SIGTERM,caught);signal(SIGINT,caught);fcntl(un_sock,F_SETFL,O_NONBLOCK);puts("Nano-X ready");
        {char *ready=getenv("NANOX_READY_FD");if(ready&&*ready){int out=atoi(ready);write(out,"R",1);close(out);}}}
    nxr_draw_target(NULL);input();
    if(stopping)GsTerminate();
    if(repeat_key&&(long)(GdGetTickCount()-repeat_at)>=0){
        GsDeliverKeyboardEvent(0,GR_EVENT_TYPE_KEY_DOWN,key(repeat_key),modifiers,repeat_key);
        repeat_at=GdGetTickCount()+50;
    }
    /* Complete staged packets even when the socket has no new data. */
    for(fd=0;fd<FD_SETSIZE;fd++)if(peers[fd]&&staged(peers[fd]))nxr_receive(fd);
    nxr_workspace_sync();
    /* Drain upstream event queues fairly before waiting. */
    for(c=root_client;c;c=c->next)if(c->waiting_for_event&&c->eventhead){curclient=c;c->waiting_for_event=0;GrGetNextEventWrapperFinish(c->id);}
    for(fd=0;fd<FD_SETSIZE;fd++)if(peers[fd]){if(peers[fd]->bad)GsClose(fd);else {flush(fd);if(peers[fd]&&peers[fd]->bad)GsClose(fd);}}
    if(nxr_present()<0){stopping=1;return;}
    FD_ZERO(&readset);FD_ZERO(&writeset);maxfd=un_sock;FD_SET(un_sock,&readset);
    /* Preserve a signal arriving between the stop check and select(). */
    FD_SET(wakefd[0],&readset);if(wakefd[0]>maxfd)maxfd=wakefd[0];
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
    for(fd=0;fd<FD_SETSIZE;fd++)if(peers[fd]&&staged(peers[fd])){tv.tv_sec=tv.tv_usec=0;wait=&tv;break;}
    ready=select(maxfd+1,&readset,&writeset,NULL,wait);
    if(stopping)GsTerminate();
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

/* Only this request's target can accumulate backing-buffer damage. */
static GR_WINDOW *paint_target;
void nxr_draw_target(GR_WINDOW *w){paint_target=w;}
int nxr_internal_pixmap(GR_PIXMAP *p){GR_WINDOW *w;for(w=listwp;w;w=w->next)if(w->buffer==p)return 1;return 0;}
int nxr_buffer_window(GR_WINDOW *w){
    int required=(w->props&GR_WM_PROPS_BUFFERED)!=0;
    if(curclient&&peers[curclient->id]&&peers[curclient->id]->direct)return 1;
    w->props|=GR_WM_PROPS_BUFFERED|GR_WM_PROPS_BUFFER_MWPF|GR_WM_PROPS_C16_BATCHED;
    GsInitWindowBuffer(w,w->width,w->height);
    if(w->buffer)return 1;
    w->props&=~(GR_WM_PROPS_BUFFERED|GR_WM_PROPS_BUFFER_MWPF|GR_WM_PROPS_C16_BATCHED|GR_WM_PROPS_DRAWING_DONE);
    w->c16_dirty=0;
    /* Automatic backing stores are an optimization. Exposure-driven clients
     * can continue drawing directly when another large window fills the heap.
     * Explicitly requested buffered windows still report allocation failure. */
    if(!required){if(trace_reject)fprintf(stderr,"NXR_DIRECT_FALLBACK %u %d %d\n",w->id,w->width,w->height);return 1;}
    fprintf(stderr,"NXR_BUFFER_FAILED %u %d %d\n",w->id,w->width,w->height);return 0;
}
void nxr_buffer_damage(PSD p,int x,int y,int width,int height){
    GR_WINDOW *w=paint_target;
    if(!w||!w->buffer||w->buffer->psd!=p||!(w->props&GR_WM_PROPS_C16_BATCHED)||width<=0||height<=0)return;
    if(x<0){width+=x;x=0;}if(y<0){height+=y;y=0;}
    if(x+width>w->width)width=w->width-x;if(y+height>w->height)height=w->height-y;
    if(width<=0||height<=0)return;
    if(!w->c16_dirty){w->c16_left=x;w->c16_top=y;w->c16_right=x+width;w->c16_bottom=y+height;w->c16_dirty=1;}
    else{if(x<w->c16_left)w->c16_left=x;if(y<w->c16_top)w->c16_top=y;
        if(x+width>w->c16_right)w->c16_right=x+width;if(y+height>w->c16_bottom)w->c16_bottom=y+height;}
    w->props&=~GR_WM_PROPS_DRAWING_DONE;
}
int nxr_clear_buffer(GR_WINDOW *w,int x,int y,int width,int height,int expose){
    PSD p;
    if(!(w->props&GR_WM_PROPS_C16_BATCHED)||!w->buffer||expose==2)return 0;
    p=w->buffer->psd;nxr_draw_target(w);clipwp=NULL;curgcp=NULL;
    GdSetClipRegion(p,GdAllocRectRegion(0,0,w->width,w->height));
    GdSetMode(MWROP_COPY);GdSetForegroundColor(p,w->background);GdFillRect(p,x,y,width,height);
    if(expose)GsDeliverExposureEvent(w,x,y,width,height);
    return 1;
}
void nxr_commit(int fd){
    GR_WINDOW *w;GR_CLIENT *client=GsFindClient(fd);if(!client)return;
    curclient=client;nxr_draw_target(NULL);
    for(w=listwp;w;w=w->next)if(w->owner==client&&w->c16_dirty&&(w->props&GR_WM_PROPS_C16_BATCHED)){
        w->props|=GR_WM_PROPS_DRAWING_DONE;
        GrClearArea(w->id,w->c16_left,w->c16_top,w->c16_right-w->c16_left,w->c16_bottom-w->c16_top,2);
        w->c16_dirty=0;
    }
    curgcp=NULL;clipwp=NULL;
}
