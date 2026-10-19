/* SPDX-License-Identifier: MIT */
#include "nano.h"
#include <fde_nano.h>
#include <stdlib.h>
#include <string.h>
#define TILE_W 128
#define TILE_H 32
static unsigned sample(long coordinate,long origin,unsigned extent,unsigned source) {
    long phase=(coordinate-origin)%(long)extent;
    if(phase<0)phase+=extent;
    return (unsigned)((unsigned long)phase*source/extent);
}
int ri_nano_draw(const RiImage *im,GR_DRAW_ID target,GR_GC_ID gc,int x,int y,int width,int height,
                 const struct RiClip *clip,unsigned flags) {
    int l,t,r,b,tx,ty,w,h,xx,yy,result=RI_OK;
    unsigned short *data;unsigned xs[TILE_W],sy,count,chunk,alpha;
    unsigned long cells,offset,total;const RiByte *pixel;
    struct fde_rect rect;FDE_IMAGE id;
    if(!im||!im->rgba||!im->width||!im->height||im->width>4096||im->height>4096||!clip||
       width<=0||height<=0||(flags&~3U)||clip->x0<0||clip->y0<0||clip->x1>1920||clip->y1>1080)
        return RI_CORRUPT;
    l=clip->x0;t=clip->y0;r=clip->x1;b=clip->y1;
    if(!(flags&RI_REPEAT_X)){if(l<x)l=x;if((long)r>(long)x+width)r=(long)x+width;}
    if(!(flags&RI_REPEAT_Y)){if(t<y)t=y;if((long)b>(long)y+height)b=(long)y+height;}
    if(l>=r||t>=b)return RI_OK;
    alpha=!im->opaque;
    data=malloc(TILE_W*TILE_H*2UL*sizeof(*data));if(!data)return RI_MEMORY;
    for(ty=t;ty<b;ty+=h)for(tx=l;tx<r;tx+=w) {
        w=r-tx;if(w>TILE_W)w=TILE_W;h=b-ty;if(h>TILE_H)h=TILE_H;
        cells=(unsigned long)w*h;
        for(xx=0;xx<w;xx++)xs[xx]=sample(tx+xx,x,width,im->width);
        for(yy=0;yy<h;yy++) {
            sy=sample(ty+yy,y,height,im->height);
            for(xx=0;xx<w;xx++) {
                offset=(unsigned long)yy*w+xx;
                pixel=im->rgba+((unsigned long)sy*im->width+xs[xx])*4;
                data[offset]=ri_image_rgb565(pixel);
                if(alpha)data[cells+offset]=pixel[3];
            }
        }
        id=fde_image_create(w,h,alpha?FDE_IMAGE_ALPHA:0);
        if(!id){result=RI_LIMIT;goto done;}
        total=cells*(alpha?2:1);offset=0;
        while(offset<total) {
            chunk=total-offset>2048?2048:(unsigned)(total-offset);
            if(fde_image_upload(id,offset,data+offset,chunk)<0){result=RI_LIMIT;break;}
            offset+=chunk;
        }
        rect.x=tx;rect.y=ty;rect.sx=rect.sy=0;rect.sw=rect.width=w;rect.sh=rect.height=h;
        if(result==RI_OK&&fde_image_draw(id,target,gc,&rect,1,0)<0)result=RI_LIMIT;
        count=fde_image_destroy(id)<0;
        if(count||result!=RI_OK){if(count)result=RI_LIMIT;goto done;}
    }
done:
    free(data);return result;
}

/* Explicit retained-resource API. The owner invalidates entries before freeing
 * or changing RGBA, and supplies a GC clipped to the passed rectangle. Three
 * entries leave the connection's fourth resource slot for the tile fallback. */
#define CACHE_ENTRIES 3
#define CACHE_CELLS 65536UL
struct retained {
    const RiImage *image;
    FDE_IMAGE id;
    unsigned width,height,repeat;
    int scale_w,scale_h;
    unsigned long cells,used;
};
static struct retained retained[CACHE_ENTRIES];
static unsigned long retained_cells,retained_clock;
static void release_retained(struct retained *entry) {
    if(entry->id)fde_image_destroy(entry->id);
    retained_cells-=entry->cells;
    memset(entry,0,sizeof(*entry));
}
void ri_nano_forget(const RiImage *image) {
    unsigned i;
    for(i=0;i<CACHE_ENTRIES;i++)if(retained[i].image==image)release_retained(retained+i);
}
void ri_nano_clear(void) {
    unsigned i;
    for(i=0;i<CACHE_ENTRIES;i++)release_retained(retained+i);
}
static struct retained *oldest_retained(void) {
    unsigned i;struct retained *entry=retained;
    for(i=0;i<CACHE_ENTRIES;i++) {
        if(retained[i].id&&(!entry->id||retained[i].used<entry->used))entry=retained+i;
    }
    return entry;
}
static struct retained *retain_image(const RiImage *im,int w,int h,unsigned repeat) {
    unsigned i,n,pass,sw=im->width,sh=im->height,alpha=!im->opaque;
    unsigned short chunk[2048];unsigned long cells,at,offset,pixels;
    struct retained *entry;
    /* Expand small CSS patterns once, instead of one synchronous request for
     * every 4x2 tile. Keep the exact integral period and nearest-neighbour phase. */
    if(repeat) {
        if(w>128||h>128)return NULL;
        sw=(repeat&RI_REPEAT_X)?((127/w)+1)*w:w;
        sh=(repeat&RI_REPEAT_Y)?((31/h)+1)*h:h;
    }
    pixels=(unsigned long)sw*sh;cells=pixels*(alpha?2:1);
    if(sw>512||sh>512||cells>CACHE_CELLS)return NULL;
    for(i=0;i<CACHE_ENTRIES;i++) {
        entry=retained+i;
        if(entry->image==im&&entry->repeat==repeat&&
           (!repeat||(entry->scale_w==w&&entry->scale_h==h))) {
            entry->used=++retained_clock;return entry;
        }
    }
    while(retained_cells>CACHE_CELLS-cells)release_retained(oldest_retained());
    entry=oldest_retained();
    for(i=0;i<CACHE_ENTRIES;i++)if(!retained[i].id){entry=retained+i;break;}
    release_retained(entry);
    entry->id=fde_image_create(sw,sh,alpha?FDE_IMAGE_ALPHA:0);
    /* Other clients share the server budget. Drop our own LRU and retry once. */
    if(!entry->id) {
        struct retained *old=oldest_retained();
        /* An empty slot is not an eviction candidate. */
        for(i=0;i<CACHE_ENTRIES;i++)if(retained[i].id&&(!old->id||retained[i].used<old->used))old=retained+i;
        if(old->id)release_retained(old);
        entry->id=fde_image_create(sw,sh,alpha?FDE_IMAGE_ALPHA:0);
    }
    if(!entry->id)return NULL;
    offset=0;
    for(pass=0;pass<=alpha;pass++)for(at=0;at<pixels;at+=n) {
        n=pixels-at>2048?2048:(unsigned)(pixels-at);
        for(i=0;i<n;i++) {
            unsigned sx=(at+i)%sw,sy=(at+i)/sw;
            const RiByte *p;
            if(repeat) { sx=sample(sx,0,w,im->width);sy=sample(sy,0,h,im->height); }
            p=im->rgba+((unsigned long)sy*im->width+sx)*4;
            chunk[i]=pass?p[3]:ri_image_rgb565(p);
        }
        if(fde_image_upload(entry->id,offset,chunk,n)<0) { fde_image_destroy(entry->id);memset(entry,0,sizeof(*entry));return NULL; }
        offset+=n;
    }
    entry->image=im;entry->width=sw;entry->height=sh;entry->repeat=repeat;
    entry->scale_w=w;entry->scale_h=h;entry->cells=cells;entry->used=++retained_clock;
    retained_cells+=cells;return entry;
}
static long tile_origin(int start,int origin,unsigned extent) {
    long phase=((long)start-origin)%(long)extent;
    if(phase<0)phase+=extent;
    return (long)start-phase;
}
int ri_nano_draw_cached(const RiImage *im,GR_DRAW_ID target,GR_GC_ID gc,int x,int y,int w,int h,
                        const struct RiClip *clip,unsigned flags) {
    struct retained *entry;struct fde_rect rects[9];unsigned n=0;
    long tx,ty,l,t,r,b,start;
    if(!im||!im->rgba||!im->width||!im->height||!clip||w<=0||h<=0||
       (flags&~3U)||clip->x0<0||clip->y0<0||clip->x1>1920||clip->y1>1080)return RI_CORRUPT;
    l=clip->x0;t=clip->y0;r=clip->x1;b=clip->y1;
    if(!(flags&RI_REPEAT_X)){if(l<x)l=x;if(r>(long)x+w)r=(long)x+w;}
    if(!(flags&RI_REPEAT_Y)){if(t<y)t=y;if(b>(long)y+h)b=(long)y+h;}
    if(l>=r||t>=b)return RI_OK;
    /* Oversized destinations retain the bounded visible-tile fallback. */
    if(w>1920||h>1080||(entry=retain_image(im,w,h,flags))==NULL)
        return ri_nano_draw(im,target,gc,x,y,w,h,clip,flags);
    start=flags?tile_origin(l,x,entry->width):x;
    for(ty=flags?tile_origin(t,y,entry->height):y;ty<b;ty+=flags?entry->height:h)
        for(tx=start;tx<r;tx+=flags?entry->width:w) {
            struct fde_rect *q=rects+n++;
            q->x=tx;q->y=ty;q->sx=q->sy=0;q->sw=entry->width;q->sh=entry->height;
            q->width=flags?entry->width:w;q->height=flags?entry->height:h;
            if(n==9) { if(fde_image_draw(entry->id,target,gc,rects,n,0)<0)return RI_LIMIT;n=0; }
        }
    return n&&fde_image_draw(entry->id,target,gc,rects,n,0)<0?RI_LIMIT:RI_OK;
}
