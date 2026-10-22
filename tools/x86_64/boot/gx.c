/* SPDX-License-Identifier: GPL-2.0-or-later
 * The first bare-metal GX backend: bounded owned RAM surfaces and explicit
 * publication. Rendering never reads device memory. Optional retained/raster
 * accelerators may report unsupported; the existing LVGL adapter falls back.
 */
#include "gx.h"
#define WINDOWS 16
#define EVENTS 64
#define MAX_WIDTH 1920
#ifdef BOOT_DESKTOP
#define MAX_HEIGHT 1200
#else
#define MAX_HEIGHT 1080
#endif
#define EINVAL_ (-22)
#define ENOMEM_ (-12)
#define EPERM_ (-1)
#define EFAULT_ (-14)
#define EOPNOTSUPP_ (-95)
typedef struct { uint16_t type,flags,code,buttons; int32_t x,y,dx,dy,wx,wy; uint32_t sequence; } Event;
typedef struct {
    uint64_t owner;
    uint32_t handle,*pixels;
    unsigned width,height,format,flags;
    uint16_t held[16],buttons;
    int32_t x,y;
    Event events[EVENTS];
    unsigned head,count,overflow;
#ifdef BOOT_DESKTOP
    uint32_t *front,process;
    unsigned pid,mapped,focused,captured,closing;
    unsigned pending_width,pending_height;
    int32_t left,top,right,bottom;
    char title[64];
#endif
} Surface;
static GxPlatform platform;
static Surface surfaces[WINDOWS];
static uint32_t generation,sequence;
static Surface *visible;
#ifdef BOOT_DESKTOP
static void desktop_publish(Surface *);
static void desktop_destroy(Surface *);
static void desktop_init(void);
static void desktop_key(unsigned,int);
static void desktop_pointer(int32_t,int32_t,uint32_t,int32_t);
static int desktop_active(void);
static void desktop_owner_release(uint64_t);
#endif
static void zero(void *destination,size_t size) { unsigned char *p=destination;while(size--)*p++=0; }
static uint16_t *span(const GxGuest *g,uint32_t p,size_t n,int write) {
    return g && g->span ? g->span(g->opaque,p,n,write) : 0;
}
static uint32_t rgb565(unsigned c) {
    unsigned r=(c>>11)&31,g=(c>>5)&63,b=c&31;
    return ((r<<3)|(r>>2))<<16 | ((g<<2)|(g>>4))<<8 | (b<<3)|(b>>2);
}
static uint32_t quantize(uint32_t c) {
    return rgb565(((c>>19)&31)<<11 | ((c>>10)&63)<<5 | ((c>>3)&31));
}
static uint32_t colour(Surface *s,uint32_t c) { return s->format==1?quantize(c):c; }
static int rectangle(Surface *s,unsigned x,unsigned y,unsigned w,unsigned h) {
    return x<=s->width && y<=s->height && w<=s->width-x && h<=s->height-y;
}
static Surface *owned(uint64_t owner,uint32_t handle,int32_t *error) {
    for(unsigned i=0;i<WINDOWS;i++)if(handle && surfaces[i].handle==handle) {
        if(surfaces[i].owner!=owner){*error=EPERM_;return 0;}
        return &surfaces[i];
    }
    *error=EINVAL_;return 0;
}
static void publish(Surface *s) {
#ifdef BOOT_DESKTOP
    desktop_publish(s);
#else
    if(s->flags&2)return;
    visible=s;
    if(platform.present)platform.present(s->pixels,s->width,s->height);
#endif
}
static void destroy(Surface *s) {
#ifdef BOOT_DESKTOP
    desktop_destroy(s);
#endif
    int was_visible=visible==s;
    if(was_visible)visible=0;
    if(s->pixels && platform.release)platform.release(s->pixels,(size_t)s->width*s->height*4);
    zero(s,sizeof(*s));
    if(was_visible && platform.restore_console)platform.restore_console();
}
void gx_bind(const GxPlatform *p) {
    for(unsigned i=0;i<WINDOWS;i++)if(surfaces[i].handle)destroy(&surfaces[i]);
    platform=*p;
#ifdef BOOT_DESKTOP
    desktop_init();
#endif
}
void gx_release(uint64_t owner) {
    for(unsigned i=0;i<WINDOWS;i++)if(surfaces[i].handle && surfaces[i].owner==owner)destroy(&surfaces[i]);
#ifdef BOOT_DESKTOP
    desktop_owner_release(owner);
#endif
}
int gx_active(void) {
#ifdef BOOT_DESKTOP
    return desktop_active();
#else
    return visible!=0;
#endif
}
static void enqueue(Surface *s,Event event) {
    event.sequence=++sequence;
    if(s->count==EVENTS) { s->head=(s->head+1)%EVENTS;--s->count;s->overflow=1; }
    s->events[(s->head+s->count)%EVENTS]=event;++s->count;
}
void gx_key(unsigned usage,int down,unsigned modifiers) {
    (void)modifiers;
#ifdef BOOT_DESKTOP
    desktop_key(usage,down);
    return;
#endif
    if(!visible || usage>=256)return;
    unsigned bit=1U<<(usage&15);
    if(down)visible->held[usage/16]|=bit;else visible->held[usage/16]&=~bit;
    /* Native window close shortcut, delivered as the existing GX event. */
    if(down && usage==61 && (visible->held[14]&0x44)) {
        enqueue(visible,(Event){.type=8});return;
    }
    enqueue(visible,(Event){.type=1,.flags=down?1:0,.code=usage});
}
void gx_pointer(int32_t x,int32_t y,uint32_t buttons,int32_t wheel) {
#ifdef BOOT_DESKTOP
    desktop_pointer(x,y,buttons,wheel);
    return;
#endif
    Surface *s=visible;if(!s)return;
    buttons&=7;
    if(x!=s->x || y!=s->y)enqueue(s,(Event){.type=2,.buttons=buttons,.x=x,.y=y});
    unsigned changed=s->buttons^buttons;
    for(unsigned i=0;i<3;i++)if(changed&(1U<<i))
        enqueue(s,(Event){.type=3,.flags=(buttons&(1U<<i))?1:0,.code=i+1,.buttons=buttons,.x=x,.y=y});
    if(wheel)enqueue(s,(Event){.type=4,.buttons=buttons,.x=x,.y=y,.wy=wheel});
    s->x=x;s->y=y;s->buttons=buttons;
}
static void word(uint16_t *p,unsigned at,uint32_t value) {p[at]=value;p[at+1]=value>>16;}
static int32_t raster(unsigned op,uint32_t a[12],const GxGuest *g) {
    unsigned w=a[2],h=a[3],ds=a[4],ss=a[5];
    if(!w || !h || w>MAX_WIDTH*2 || h>MAX_HEIGHT*2 || ds<w || (op==16 && ss<w))return EINVAL_;
    size_t n=(size_t)(h-1)*ds+w;
    uint16_t *src=0,*dst=0;
    if(op==16) {
        size_t sn=(size_t)(h-1)*ss+w;
        src=span(g,a[1],sn,0);if(!src)return EFAULT_;
        dst=span(g,a[0],n,0);if(!dst)return EFAULT_;
        uintptr_t d=(uintptr_t)dst,s=(uintptr_t)src;
        if(d<s+sn*2 && s<d+n*2)return EINVAL_;
    }
    dst=span(g,a[0],n,1);if(!dst)return EFAULT_;
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)dst[(size_t)y*ds+x]=op==15?(uint16_t)a[6]:src[(size_t)y*ss+x];
    return 0;
}
static int32_t blend_bgr(uint32_t a[12],const GxGuest *g) {
    unsigned w=a[2],h=a[3],stride=a[4],ms=a[5];
    if(!w || !h || w>MAX_WIDTH*2 || h>MAX_HEIGHT*2 || stride<(size_t)w*3 ||
       (a[1] && ms<w) || a[6]>255 || a[7]>255 || a[8]>255 || a[9]>255)return EINVAL_;
    size_t n=(size_t)(h-1)*stride+(size_t)w*3;
    uint16_t *dst=span(g,a[0],n,0),*mask=0;
    if(!dst)return EFAULT_;
    if(a[1]) {
        size_t mn=(size_t)(h-1)*ms+w;
        mask=span(g,a[1],mn,0);if(!mask)return EFAULT_;
        uintptr_t d=(uintptr_t)dst,m=(uintptr_t)mask;
        if(d<m+mn*2 && m<d+n*2)return EINVAL_;
    }
    if(!a[9])return 0;
    dst=span(g,a[0],n,1);if(!dst)return EFAULT_;
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++) {
        unsigned mix=mask?(mask[(size_t)y*ms+x]&255):255;
        if(a[9]<253)mix=mask?(mix*a[9])>>8:a[9];
        if(!mix)continue;
        for(unsigned c=0;c<3;c++) {
            size_t at=(size_t)y*stride+x*3+c;
            dst[at]=mix>=253?a[6+c]:(a[6+c]*mix+(uint32_t)dst[at]*(255-mix))>>8;
        }
    }
    return 0;
}
#ifdef BOOT_DESKTOP
#include "gx-desktop.inc"
#define gx_call gx_basic_call
#endif
int32_t gx_call(uint64_t owner,unsigned op,uint32_t a[12],const GxGuest *g) {
    int32_t error=0;
    if(!owner)return EPERM_;
    if(op==11)return 0;
    if(op==15 || op==16)return raster(op,a,g);
    if(op==25)return blend_bgr(a,g);
    if(op==26){a[1]=2;a[2]=3;return 0;}
    /* Kernel polling walks all leases, even while init/another process runs.
     * Readiness reveals no event contents and must wake sleeping owners. */
    if(op==10) {
        for(unsigned i=0;i<WINDOWS;i++)if(a[0] && surfaces[i].handle==a[0])
            return surfaces[i].count || surfaces[i].overflow;
        return EINVAL_;
    }
    if(op==21 || op==22 || op==29 || op==30)return EOPNOTSUPP_;
    if(op==1) {
        unsigned w=a[0],h=a[1];
        if(!w || !h || w>MAX_WIDTH || h>MAX_HEIGHT || (a[3]&~2U) || a[4]>2)return EINVAL_;
        if(!span(g,a[2],64,0))return EFAULT_;
        if(!platform.allocate || !platform.width || !platform.height)return EOPNOTSUPP_;
        Surface *s=0;for(unsigned i=0;i<WINDOWS;i++)if(!surfaces[i].handle){s=&surfaces[i];break;}
        if(!s)return ENOMEM_;
        uint32_t *pixels=platform.allocate((size_t)w*h*4);if(!pixels)return ENOMEM_;
        zero(pixels,(size_t)w*h*4);zero(s,sizeof(*s));
        uint32_t handle;
        do{handle=(++generation<<4)|(unsigned)(s-surfaces);}while(!handle);
        s->owner=owner;s->handle=handle;s->pixels=pixels;s->width=w;s->height=h;s->format=a[4]==2?2:1;s->flags=a[3];
        a[0]=handle;return 0;
    }
    Surface *s=owned(owner,a[0],&error);if(!s)return error;
    if(op==2){destroy(s);return 0;}
    if(op==4 || op==13){publish(s);return 0;}
    if(op==7)return span(g,a[1],64,0)?0:EFAULT_;
    if(op==14){a[1]=(uint32_t)s->x;a[2]=(uint32_t)s->y;return 0;}
    if(op==24){if(a[1]>2)return EINVAL_;a[1]=1;a[2]=s->width;a[3]=s->height;return 0;}
    if(op==28){if(a[1]<100 || a[1]>300)return EINVAL_;a[1]=100;return 0;}
    if(op==12) {
        unsigned w=a[1],h=a[2],flags=a[3];
        if(!w || !h || w>MAX_WIDTH || h>MAX_HEIGHT || (flags&~3U))return EINVAL_;
        uint32_t *pixels=s->pixels;
        if(w!=s->width || h!=s->height) {
            pixels=platform.allocate((size_t)w*h*4);if(!pixels)return ENOMEM_;
            zero(pixels,(size_t)w*h*4);
            for(unsigned y=0;y<h && y<s->height;y++)for(unsigned x=0;x<w && x<s->width;x++)pixels[(size_t)y*w+x]=s->pixels[(size_t)y*s->width+x];
            platform.release(s->pixels,(size_t)s->width*s->height*4);
        }
        int active=visible==s;
        s->pixels=pixels;s->width=w;s->height=h;s->flags=flags;
        if(active && (flags&2)){visible=0;if(platform.restore_console)platform.restore_console();}
        /* Configure changes storage/visibility; it never publishes new pixels. */
        return 0;
    }
    if(op==8) {
        uint16_t *e=span(g,a[1],21,1);if(!e)return EFAULT_;
        Event event={0};
        if(s->overflow){event.type=9;event.sequence=++sequence;s->overflow=0;}
        else if(s->count){event=s->events[s->head];s->head=(s->head+1)%EVENTS;--s->count;}
        zero(e,42);e[0]=1;word(e,1,s->handle);word(e,3,event.sequence);e[5]=event.type;e[6]=event.flags;e[7]=event.code;e[8]=event.buttons;
        word(e,9,event.x);word(e,11,event.y);word(e,13,event.dx);word(e,15,event.dy);word(e,17,event.wx);word(e,19,event.wy);return 0;
    }
    if(op==9) {
        uint16_t *state=span(g,a[1],25,1);if(!state)return EFAULT_;
        zero(state,50);state[0]=1;word(state,1,s->handle);state[3]=visible==s?8:0;state[4]=s->buttons;word(state,21,s->x);word(state,23,s->y);
        for(unsigned i=0;i<16;i++)state[5+i]=s->held[i];return 0;
    }
    if(op==3 || op==17 || op==18 || op==23) {
        unsigned x=a[1],y=a[2],w=a[3],h=a[4],stride=a[5],bgr=op==23;
        if(!w || !h)return 0;
        if(w>MAX_WIDTH || h>MAX_HEIGHT || stride<(size_t)w*(bgr?3:1) ||
           ((op==17 || op==18)?(x || y):!rectangle(s,x,y,w,h)))return EINVAL_;
        size_t cells=(size_t)(h-1)*stride+(size_t)w*(bgr?3:1);
        uint16_t *pixels=span(g,a[6],cells,0),*palette=0;
        if(!pixels)return EFAULT_;
        if(op==18){palette=span(g,a[7],256,0);if(!palette)return EFAULT_;}
        if(op==17 || op==18) {
            unsigned dw=s->width,dh=(unsigned)((uint64_t)h*dw/w);
            if(dh>s->height){dh=s->height;dw=(unsigned)((uint64_t)w*dh/h);}
            if(!dw)dw=1;if(!dh)dh=1;
            unsigned left=(s->width-dw)/2,top=(s->height-dh)/2;
            zero(s->pixels,(size_t)s->width*s->height*4);
            for(unsigned j=0;j<dh;j++)for(unsigned i=0;i<dw;i++) {
                unsigned v=pixels[(size_t)((uint64_t)j*h/dh)*stride+(uint64_t)i*w/dw];
                s->pixels[(size_t)(top+j)*s->width+left+i]=rgb565(palette?palette[v&255]:v);
            }
        } else for(unsigned j=0;j<h;j++)for(unsigned i=0;i<w;i++) {
            size_t at=(size_t)j*stride+i*(bgr?3:1);
            uint32_t c=bgr?((pixels[at+2]&255)<<16 | (pixels[at+1]&255)<<8 | (pixels[at]&255)):rgb565(pixels[at]);
            s->pixels[(size_t)(y+j)*s->width+x+i]=colour(s,c);
        }
        return 0;
    }
    if(op==5 || op==27) {
        unsigned x=a[1],y=a[2],w=a[3],h=a[4];
        if(!rectangle(s,x,y,w,h) || (op==27 && a[5]>0xffffff))return EINVAL_;
        uint32_t c=op==5?rgb565(a[5]):colour(s,a[5]);
        for(unsigned j=0;j<h;j++)for(unsigned i=0;i<w;i++)s->pixels[(size_t)(y+j)*s->width+x+i]=c;
        return 0;
    }
    if(op==6) {
        unsigned x=a[1],y=a[2],w=a[3],h=a[4],dx=a[5],dy=a[6];
        if(!rectangle(s,x,y,w,h) || !rectangle(s,dx,dy,w,h))return EINVAL_;
        int reverse=dy>y || (dy==y && dx>x);
        for(unsigned row=0;row<h;row++) {
            unsigned j=reverse?h-1-row:row;
            for(unsigned column=0;column<w;column++) {
                unsigned i=reverse?w-1-column:column;
                s->pixels[(size_t)(dy+j)*s->width+dx+i]=s->pixels[(size_t)(y+j)*s->width+x+i];
            }
        }
        return 0;
    }
    return EOPNOTSUPP_;
}
#ifdef BOOT_DESKTOP
#undef gx_call
int32_t gx_call(uint64_t owner,unsigned op,uint32_t a[12],const GxGuest *g) {
    return desktop_call(owner,op,a,g);
}
#endif
