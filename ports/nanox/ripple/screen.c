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
#if NXR_SERVER
#include "server.h"
#endif

extern int gr_mode;
static int fd=-1, dirty, left, top, right, bottom;
static struct gx_window window;
static unsigned short *allocation;
#define SCREEN_CELLS (640UL*480)
#define BLIT_ROWS 16
#define BLIT_CELLS (640UL*BLIT_ROWS)
static unsigned short *blit_scratch;
static unsigned raster_caps;
static int bulk_raster(unsigned short *dst,unsigned dpitch,const unsigned short *src,unsigned spitch,int width,int height,unsigned colour,int copy){
    struct gx_raster r;
    if(!(raster_caps&GX_CAP_RASTER)||(unsigned long)width*height<512)return 0;
    r.version=GX_VERSION;r.width=width;r.height=height;r.dst_stride=dpitch;r.src_stride=spitch;
    r.dst=dst;r.src=src;r.colour=colour;
    return ioctl(fd,copy?GX_RASTER_COPY:GX_RASTER_FILL,&r)==0;
}
static PSD screen_open(PSD psd);
SCREENDEVICE scrdev={.Open=screen_open};

static void damage(PSD psd,int x,int y,int width,int height) {
    if(psd!=&scrdev){
#if NXR_SERVER
        nxr_buffer_damage(psd,x,y,width,height);
#endif
        return;
    }
    if(width<=0||height<=0)return;
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
    int y;
    if(gr_mode==MWROP_COPY&&bulk_raster(pixel(psd,x1,y1),psd->pitch,NULL,0,x2-x1+1,y2-y1+1,colour,0)){damage(psd,x1,y1,x2-x1+1,y2-y1+1);return;}
    for(y=y1;y<=y2;y++)horizontal(psd,x1,x2,y,colour);
}
static void frameblit(PSD psd,PMWBLITPARMS p) {
    int x,y,row,reverse=p->data_out==p->data&&p->dsty>p->srcy;
    unsigned short *out;const unsigned short *in;
    if(p->op==MWROP_COPY&&bulk_raster((unsigned short *)p->data_out+(unsigned long)p->dsty*p->dst_pitch+p->dstx,p->dst_pitch,(const unsigned short *)p->data+(unsigned long)p->srcy*p->src_pitch+p->srcx,p->src_pitch,p->width,p->height,0,1)){damage(psd,p->dstx,p->dsty,p->width,p->height);return;}
    for(y=0;y<p->height;y++) {
        row=reverse?p->height-1-y:y;
        out=(unsigned short *)p->data_out+(unsigned long)(p->dsty+row)*p->dst_pitch+p->dstx;
        in=(const unsigned short *)p->data+(unsigned long)(p->srcy+row)*p->src_pitch+p->srcx;
        if(p->op==MWROP_COPY)memmove(out,in,p->width*sizeof(*out));
        else for(x=0;x<p->width;x++)raster(out+x,in[x],p->op);
    }
    damage(psd,p->dstx,p->dsty,p->width,p->height);
}
static void monochrome(PSD psd,PMWBLITPARMS p) {
    int x,y,width=p->width,height=p->height,spitch=p->src_pitch,dpitch=p->dst_pitch;
    unsigned bits,mask,firstmask=0x8000U>>(p->srcx%16),opaque=p->usebg;
    unsigned short fg=p->fg_pixelval,bg=p->bg_pixelval,*out,*dest;
    const MWIMAGEBITS *in,*source;
    int fast=width==8&&p->srcx==0&&opaque;
    source=(const MWIMAGEBITS *)p->data+(unsigned long)p->srcy*spitch+p->srcx/16;
    dest=(unsigned short *)p->data_out+(unsigned long)p->dsty*dpitch+p->dstx;
    for(y=0;y<height;y++) {
        in=source;out=dest;source+=spitch;dest+=dpitch;bits=*in;
        if(fast){
            out[0]=(bits&0x8000U)?fg:bg;out[1]=(bits&0x4000U)?fg:bg;
            out[2]=(bits&0x2000U)?fg:bg;out[3]=(bits&0x1000U)?fg:bg;
            out[4]=(bits&0x0800U)?fg:bg;out[5]=(bits&0x0400U)?fg:bg;
            out[6]=(bits&0x0200U)?fg:bg;out[7]=(bits&0x0100U)?fg:bg;
        }else{
            mask=firstmask;
            for(x=0;x<width;x++) {
                if(bits&mask)*out=fg;else if(opaque)*out=bg;
                out++;mask>>=1;if(!mask&&x+1<width){mask=0x8000U;bits=*++in;}
            }
        }
    }
    damage(psd,p->dstx,p->dsty,width,height);
}
/* Preserve the source across disjoint clip rectangles without a window-sized
 * allocation. Stripes traverse in memmove order; each stripe snapshots its
 * complete source before any destination clip can overwrite it. */
void nxr_engine_blit(PSD,int,int,int,int,PSD,int,int,int);
void GdBlit(PSD dst,MWCOORD dx,MWCOORD dy,MWCOORD w,MWCOORD h,PSD src,MWCOORD sx,MWCOORD sy,int op) {
    SCREENDEVICE snapshot;
    int x,y,cw,ch,at,rows,row,reverse;
    long endx,endy;
    if(w<=0||h<=0)return;
    if(dst->addr!=src->addr){nxr_engine_blit(dst,dx,dy,w,h,src,sx,sy,op);return;}
    x=sx<0?0:sx;y=sy<0?0:sy;
    endx=(long)sx+w;endy=(long)sy+h;
    if(endx>src->xres)endx=src->xres;if(endy>src->yres)endy=src->yres;
    cw=endx-x;ch=endy-y;if(cw<=0||ch<=0)return;
    dx+=x-sx;dy+=y-sy;
    /* A single visible copy can use the overlap-safe GX raster/memmove path
     * directly. Nonoverlapping copies also need no snapshot across clips. */
    if(dx>=x+cw||x>=dx+cw||dy>=y+ch||y>=dy+ch||
       (op==MWROP_COPY&&GdClipArea(dst,dx,dy,dx+cw-1,dy+ch-1)==CLIP_VISIBLE)){
        nxr_engine_blit(dst,dx,dy,cw,ch,src,x,y,op);return;
    }
    snapshot=*src;snapshot.flags=PSF_MEMORY;snapshot.addr=(unsigned char *)blit_scratch;
    snapshot.pitch=cw;snapshot.xres=snapshot.xvirtres=cw;
    reverse=dy>y;
    for(at=0;at<ch;at+=rows){
        rows=ch-at;if(rows>BLIT_ROWS)rows=BLIT_ROWS;
        row=reverse?ch-at-rows:at;
        GdCheckCursor(src,x,y,x+cw-1,y+ch-1);
        if(!bulk_raster(blit_scratch,cw,pixel(src,x,y+row),src->pitch,cw,rows,0,1))
            for(h=0;h<rows;h++)memcpy(blit_scratch+(unsigned long)h*cw,pixel(src,x,y+row+h),cw*sizeof(*blit_scratch));
        snapshot.yres=snapshot.yvirtres=rows;snapshot.size=(unsigned long)cw*rows;
        nxr_engine_blit(dst,dx,dy+row,cw,rows,&snapshot,0,0,op);
    }
    GdFixCursor(src);
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
    fd=-1;window.window=0;free(allocation);allocation=NULL;blit_scratch=NULL;psd->addr=NULL;dirty=0;
    if(clipregion){GdDestroyRegion(clipregion);clipregion=NULL;}
}
static PSD screen_open(PSD psd) {
    struct gx_info info;
    if(fd>=0)return NULL;
    fd=open("/dev/gfx",O_RDWR|O_CLOEXEC);if(fd<0)return NULL;
    memset(&info,0,sizeof(info));info.version=GX_VERSION;
    if(ioctl(fd,GX_INFO,&info)<0||info.max_width<640||info.max_height<480)goto failed;
    raster_caps=info.flags;
    allocation=malloc((SCREEN_CELLS+BLIT_CELLS+3)*sizeof(*allocation));if(!allocation)goto failed;
    allocation[0]=allocation[SCREEN_CELLS+1]=allocation[SCREEN_CELLS+BLIT_CELLS+2]=0x5aa5;
    blit_scratch=allocation+SCREEN_CELLS+2;
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
    if(!allocation||allocation[0]!=0x5aa5||allocation[SCREEN_CELLS+1]!=0x5aa5||allocation[SCREEN_CELLS+BLIT_CELLS+2]!=0x5aa5){GdError("Nano-X: framebuffer guard corrupted\n");return -1;}
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
