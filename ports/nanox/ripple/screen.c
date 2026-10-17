/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Nano-X RGB565 screen driver. All addresses/strides count C16 cells.
 * Rasterize locally; only completed dirty rectangles cross GX (kernel DMA).
 * The service owns one RVM desktop display, not one host window per client. */
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "screen.h"
#include "genfont.h"
#include "fb.h"

extern int gr_mode;
static int fd=-1, dirty, left, top, right, bottom;
static struct gx_window window;
static unsigned short *allocation;
static PSD screen_open(PSD psd);
SCREENDEVICE scrdev={.Open=screen_open};

static void damage(PSD psd,int x,int y,int width,int height) {
    if(psd!=&scrdev||width<=0||height<=0)return;
    if(!dirty){left=x;top=y;right=x+width;bottom=y+height;dirty=1;}
    else {if(x<left)left=x;if(y<top)top=y;if(x+width>right)right=x+width;if(y+height>bottom)bottom=y+height;}
}
static void raster(unsigned short *dst,unsigned short src,int op) {
    APPLYOP(op,1,(unsigned short),src,*(unsigned short *),dst,0,0);
}
static unsigned short *pixel(PSD psd,int x,int y) {
    return (unsigned short *)psd->addr+(unsigned long)y*psd->pitch+x;
}
static void drawpixel(PSD psd,MWCOORD x,MWCOORD y,MWPIXELVAL colour) {
    raster(pixel(psd,x,y),colour,gr_mode);damage(psd,x,y,1,1);
}
static MWPIXELVAL readpixel(PSD psd,MWCOORD x,MWCOORD y) {return *pixel(psd,x,y);}
static void horizontal(PSD psd,MWCOORD x1,MWCOORD x2,MWCOORD y,MWPIXELVAL colour) {
    unsigned short *out=pixel(psd,x1,y);int x;
    if(gr_mode==MWROP_COPY)for(x=x1;x<=x2;x++)*out++=colour;
    else for(x=x1;x<=x2;x++)raster(out++,colour,gr_mode);
    damage(psd,x1,y,x2-x1+1,1);
}
static void vertical(PSD psd,MWCOORD x,MWCOORD y1,MWCOORD y2,MWPIXELVAL colour) {
    int y;for(y=y1;y<=y2;y++)raster(pixel(psd,x,y),colour,gr_mode);
    damage(psd,x,y1,1,y2-y1+1);
}
static void fill(PSD psd,MWCOORD x1,MWCOORD y1,MWCOORD x2,MWCOORD y2,MWPIXELVAL colour) {
    int y;for(y=y1;y<=y2;y++)horizontal(psd,x1,x2,y,colour);
}
static void frameblit(PSD psd,PMWBLITPARMS p) {
    int x,y;unsigned short *out;const unsigned short *in;
    for(y=0;y<p->height;y++) {
        out=(unsigned short *)p->data_out+(unsigned long)(p->dsty+y)*p->dst_pitch+p->dstx;
        in=(const unsigned short *)p->data+(unsigned long)(p->srcy+y)*p->src_pitch+p->srcx;
        if(p->op==MWROP_COPY)memcpy(out,in,p->width*sizeof(*out));
        else for(x=0;x<p->width;x++)raster(out+x,in[x],p->op);
    }
    damage(psd,p->dstx,p->dsty,p->width,p->height);
}
static void monochrome(PSD psd,PMWBLITPARMS p) {
    int x,y,bit;const MWIMAGEBITS *in;unsigned short *out;
    for(y=0;y<p->height;y++) {
        in=(const MWIMAGEBITS *)p->data+(unsigned long)(p->srcy+y)*p->src_pitch;
        out=(unsigned short *)p->data_out+(unsigned long)(p->dsty+y)*p->dst_pitch+p->dstx;
        for(x=0;x<p->width;x++) {
            bit=p->srcx+x;
            if(in[bit/16]&(0x8000U>>(bit%16)))*out=p->fg_pixelval;
            else if(p->usebg)*out=p->bg_pixelval;
            out++;
        }
    }
    damage(psd,p->dstx,p->dsty,p->width,p->height);
}
/* Preserve the original source even when the engine splits a self-blit into
 * several disjoint clip rectangles. Per-row memmove alone is insufficient. */
void nxr_engine_blit(PSD,int,int,int,int,PSD,int,int,int);
void GdBlit(PSD dst,MWCOORD dx,MWCOORD dy,MWCOORD w,MWCOORD h,PSD src,MWCOORD sx,MWCOORD sy,int op) {
    SCREENDEVICE snapshot;unsigned char *saved=NULL;
    if(w<=0||h<=0)return;
    if(dst->addr==src->addr) {
        saved=malloc(src->size);
        if(!saved){GdError("Nano-X: blit snapshot allocation failed\n");return;}
        memcpy(saved,src->addr,src->size);snapshot=*src;snapshot.addr=saved;src=&snapshot;
    }
    nxr_engine_blit(dst,dx,dy,w,h,src,sx,sy,op);free(saved);
}
static void screen_info(PSD psd,PMWSCREENINFO info) {
    memset(info,0,sizeof(*info));info->rows=psd->yres;info->cols=psd->xres;
    info->planes=1;info->bpp=16;info->data_format=MWIF_RGB565;info->ncolors=65536UL;
    info->fonts=NUMBER_FONTS;info->pixtype=MWPF_TRUECOLOR565;info->size=psd->size;
    info->rmask=0xf800;info->gmask=0x07e0;info->bmask=0x001f;
}
static void screen_close(PSD psd) {
    struct gx_handle h;
    if(fd>=0){if(window.window){h.version=GX_VERSION;h.handle=window.window;ioctl(fd,GX_DESTROY,&h);}close(fd);}
    fd=-1;window.window=0;free(allocation);allocation=NULL;psd->addr=NULL;dirty=0;
    if(clipregion){GdDestroyRegion(clipregion);clipregion=NULL;}
}
static PSD screen_open(PSD psd) {
    struct gx_info info;
    if(fd>=0)return NULL;
    fd=open("/dev/gfx",O_RDWR|O_CLOEXEC);if(fd<0)return NULL;
    memset(&info,0,sizeof(info));info.version=GX_VERSION;
    if(ioctl(fd,GX_INFO,&info)<0||info.max_width<640||info.max_height<480)goto failed;
    allocation=malloc((640UL*480+2)*sizeof(*allocation));if(!allocation)goto failed;
    allocation[0]=allocation[640UL*480+1]=0x5aa5;
    memset(&window,0,sizeof(window));window.version=GX_VERSION;window.width=640;window.height=480;
    window.format=GX_RGB565;strcpy(window.title,"Nano-X");
    if(ioctl(fd,GX_CREATE,&window)<0)goto failed;
    psd->flags=PSF_SCREEN;psd->xres=psd->xvirtres=640;psd->yres=psd->yvirtres=480;
    psd->planes=1;psd->bpp=16;psd->data_format=MWIF_RGB565;psd->pitch=640;
    psd->size=640UL*480;psd->addr=(unsigned char *)(allocation+1);
    psd->ncolors=65536UL;psd->pixtype=MWPF_TRUECOLOR565;psd->builtin_fonts=gen_fonts;
    psd->Close=screen_close;psd->GetScreenInfo=screen_info;psd->Update=damage;
    psd->DrawPixel=drawpixel;psd->ReadPixel=readpixel;psd->DrawHorzLine=horizontal;
    psd->DrawVertLine=vertical;psd->FillRect=fill;psd->FrameBlit=frameblit;
    psd->BlitCopyMaskMonoWordMSB=monochrome;
    return psd;
failed:
    screen_close(psd);return NULL;
}
int nxr_present(void) {
    struct gx_upload u;struct gx_handle h;
    if(!allocation||allocation[0]!=0x5aa5||allocation[640UL*480+1]!=0x5aa5){GdError("Nano-X: framebuffer guard corrupted\n");return -1;}
    if(!dirty)return 0;
    memset(&u,0,sizeof(u));u.version=GX_VERSION;u.surface=window.surface;
    u.x=left;u.y=top;u.width=right-left;u.height=bottom-top;u.stride=scrdev.pitch;u.pixels=pixel(&scrdev,left,top);
    if(ioctl(fd,GX_UPLOAD,&u)<0)return -1;
    h.version=GX_VERSION;h.handle=window.window;
    if(ioctl(fd,GX_PRESENT,&h)<0)return -1;
    dirty=0;return 0;
}
int nxr_descriptor(void){return fd;}
int nxr_event(struct gx_event *e){memset(e,0,sizeof(*e));e->version=GX_VERSION;e->window=window.window;return ioctl(fd,GX_EVENT,e);}
int GdError(const char *format,...) {int result;va_list args;va_start(args,format);result=vfprintf(stderr,format,args);va_end(args);return result;}
#if !NXR_SERVER
/* The engine demo has no software cursor or input driver. */
void GdCheckCursor(PSD p,MWCOORD a,MWCOORD b,MWCOORD c,MWCOORD d){}
void GdFixCursor(PSD p){}
void GdGetButtonInfo(int *buttons){*buttons=0;}
void GdGetModifierInfo(MWKEYMOD *mods,MWKEYMOD *current){if(mods)*mods=0;if(current)*current=0;}
MWBOOL GdGetCursorPos(MWCOORD *x,MWCOORD *y){*x=*y=0;return FALSE;}

#endif
#if NXR_SERVER
MWIMGDATFMT set_data_formatex(int type,int bpp){return type==MWPF_TRUECOLOR565&&bpp==16?MWIF_RGB565:0;}
/* RGB565 memory pixmaps use exactly the same cell raster callbacks. */
static void free_pixmap(PSD p){if(p->flags&PSF_ADDRMALLOC)free(p->addr);free(p);}
PSD GdCreatePixmap(PSD root,MWCOORD width,MWCOORD height,MWIMGDATFMT format,void *pixels,int palsize){
    PSD p;unsigned long cells;
    if(width<=0||height<=0||width>640||height>480||palsize||
       (format&&format!=MWIF_RGB565))return NULL;
    cells=(unsigned long)width*height;
    p=malloc(sizeof(*p));if(!p)return NULL;*p=*root;
    p->flags=PSF_MEMORY;p->xres=p->xvirtres=width;p->yres=p->yvirtres=height;
    p->pitch=width;p->size=cells;p->addr=pixels;p->FreeMemGC=free_pixmap;
    if(!pixels){p->addr=calloc(cells,sizeof(unsigned short));if(!p->addr){free(p);return NULL;}p->flags|=PSF_ADDRMALLOC;}
    return p;
}
#endif
