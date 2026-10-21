/* SPDX-License-Identifier: MIT */
/* Real Nano-X adapter, independent in-memory server and sampling oracle. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../image/nano.h"
#include <fde_nano.h>
#include "../image/pixel.h"
struct resource {unsigned w,h,alpha;unsigned short *data;unsigned long received;};
#ifdef RIPPLE_NETSURF_HOSTED
#define RESOURCES 16
#define RESOURCE_CELLS 524288UL
#else
#define RESOURCES 4
#define RESOURCE_CELLS 65536UL
#endif
static struct resource resources[RESOURCES];
static unsigned creates,uploads,draws,destroys,live,fail_create,fail_upload;
static unsigned short canvas[1920*1080];
static unsigned cw=64,ch=48;
static struct RiClip clipping;
uint16_t ri_image_rgb565(const RiByte *p) {return (p[0]>>3)<<11|(p[1]>>2)<<5|(p[2]>>3);}
FDE_IMAGE fde_image_create(unsigned w,unsigned h,unsigned flags) {
    unsigned i;++creates;if(fail_create){--fail_create;return 0;}
    if((unsigned long)w*h*(flags?2:1)>RESOURCE_CELLS)return 0;
    for(i=0;i<RESOURCES;i++)if(!resources[i].data) {
        resources[i].w=w;resources[i].h=h;resources[i].alpha=flags;
        resources[i].data=calloc((size_t)w*h*(flags?2:1),sizeof(unsigned short));
        resources[i].received=0;assert(resources[i].data);live++;return i+1;
    }
    return 0;
}
int fde_image_upload(FDE_IMAGE id,unsigned long offset,const unsigned short *data,unsigned n) {
    struct resource *r=resources+id-1;assert(id&&id<=RESOURCES&&r->data&&offset==r->received);
    ++uploads;if(fail_upload){--fail_upload;return -1;}
    assert(n<=2048&&offset+n<=(unsigned long)r->w*r->h*(r->alpha?2:1));
    memcpy(r->data+offset,data,n*sizeof(*data));r->received+=n;return 0;
}
int fde_image_destroy(FDE_IMAGE id) {
    assert(id&&id<=RESOURCES&&resources[id-1].data);free(resources[id-1].data);
    memset(resources+id-1,0,sizeof(resources[0]));++destroys;live--;return 0;
}
int fde_image_draw(FDE_IMAGE id,GR_DRAW_ID target,GR_GC_ID gc,const struct fde_rect *rects,unsigned n,unsigned flags) {
    struct resource *r=resources+id-1;unsigned i,x,y;assert(id&&r->data&&n<=9);++draws;
    for(i=0;i<n;i++)for(y=clipping.y0;y<(unsigned)clipping.y1;y++)for(x=clipping.x0;x<(unsigned)clipping.x1;x++) {
        const struct fde_rect *q=rects+i;unsigned long at;
        if((long)x<q->x||(long)y<q->y||(long)x>=q->x+q->width||(long)y>=q->y+q->height)continue;
        at=(q->sy+(y-(long)q->y)*q->sh/q->height)*r->w+q->sx+(x-(long)q->x)*q->sw/q->width;
        canvas[y*cw+x]=r->alpha?ri_pixel_over565(r->data[at],r->data[(unsigned long)r->w*r->h+at],canvas[y*cw+x]):r->data[at];
    }
    return 0;
}
static void compare(RiImage *im,int x,int y,int w,int h,unsigned flags) {
    unsigned xx,yy;memset(canvas,0x55,(size_t)cw*ch*sizeof(*canvas));
    assert(ri_nano_draw_cached(im,1,1,x,y,w,h,&clipping,flags)==RI_OK);
    for(yy=0;yy<ch;yy++)for(xx=0;xx<cw;xx++) {
        unsigned short expected=0x5555;
        if(xx>=(unsigned)clipping.x0&&xx<(unsigned)clipping.x1&&yy>=(unsigned)clipping.y0&&yy<(unsigned)clipping.y1&&
          ((flags&RI_REPEAT_X)||((long)xx>=x&&(long)xx<x+w))&&
          ((flags&RI_REPEAT_Y)||((long)yy>=y&&(long)yy<y+h))) {
            long dx=((long)xx-x)%w,dy=((long)yy-y)%h;const RiByte *p;
            if(dx<0)dx+=w;if(dy<0)dy+=h;
            p=im->rgba+((dy*im->height/h)*im->width+dx*im->width/w)*4;
            expected=im->opaque?ri_image_rgb565(p):ri_pixel_over565(ri_image_rgb565(p),p[3],expected);
        }
        assert(canvas[yy*cw+xx]==expected);
    }
}
int main(void) {
    RiImage im={.width=17,.height=11};unsigned i,w,h,before,initial;
    im.rgba=malloc(4*17*11*sizeof(*im.rgba));assert(im.rgba);
    for(i=0;i<17*11;i++){im.rgba[i*4]=i%256;im.rgba[i*4+1]=i*7%256;im.rgba[i*4+2]=i*13%256;im.rgba[i*4+3]=i*29%256;}
    clipping=(struct RiClip){3,5,61,43};
    for(i=0;i<96;i++) {w=1+i*7%90;h=1+i*11%80;compare(&im,-9+i%20,-7+i%17,w,h,i%4);}
    ri_nano_clear();assert(!live);
    before=uploads;
    for(i=0;i<8;i++)assert(ri_nano_draw(&im,1,1,-4,-6,80,60,&clipping,0)==RI_OK);
    printf("visible-tile baseline: 8 repaints, %u uploads\n",uploads-before);
    initial=uploads;compare(&im,-4,-6,80,60,0);before=uploads;initial=before-initial;
    for(i=0;i<8;i++)compare(&im,-4,-6,80,60,0);
    assert(uploads==before);printf("retained image: 8 repaints, 0 uploads (initial %u chunks)\n",initial);
    ri_nano_forget(&im);assert(!live);
    compare(&im,-4,-6,4,2,3);before=uploads;
    for(i=0;i<8;i++)compare(&im,-4,-6,4,2,3);
    assert(uploads==before);ri_nano_clear();assert(!live);
    fail_create=2;compare(&im,0,0,17,11,0);ri_nano_clear();assert(!live);
    fail_upload=1;compare(&im,0,0,17,11,0);ri_nano_clear();assert(!live);
    im.opaque=1;compare(&im,-3,1,53,47,0);ri_nano_forget(&im);
    im.rgba[0]=255;compare(&im,0,0,17,11,0);ri_nano_clear();
    {RiImage large={.width=256,.height=256};large.rgba=malloc(256UL*256*4);assert(large.rgba);
     memset(large.rgba,128,256UL*256*4);compare(&large,-10,-12,256,256,0);ri_nano_forget(&large);free(large.rgba);assert(!live);}
#ifdef RIPPLE_NETSURF_HOSTED
    { RiImage photos[10];struct fde_rect r={0};
      r.sw=r.width=17;r.sh=r.height=11;
      assert(ri_nano_draw_rects(&im,1,1,&r,1,1)==RI_OK);
      for(i=0;i<10;i++){photos[i]=im;compare(photos+i,0,0,17,11,0);}
      before=uploads;
      assert(ri_nano_draw_rects(&im,1,1,&r,1,1)==RI_OK&&uploads==before);
      ri_nano_clear();assert(!live);
    }
#endif
    cw=1500;ch=900;clipping=(struct RiClip){0,0,1500,900};compare(&im,1100,700,380,180,0);
    ri_nano_clear();free(im.rgba);assert(!live);
    puts("retained image: clipping, scaling, alpha, CSS phase, fallback, eviction, hosted bounds and cleanup passed");return 0;
}
