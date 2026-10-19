/* SPDX-License-Identifier: GPL-2.0-only */
/* Exercise the real atlas builder/painter without a server. Guards and strict
 * image-draw stubs catch bad raster writes and protocol source rectangles. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "utils/errors.h"
#include "netsurf/plotters.h"
#include "../../../libraries/image/nano.h"
#include "../frontend/font.inc"
struct plot_context {struct rect limit,clip;unsigned text_runs,rectangles;};
static GR_WINDOW_ID wid=1;static GR_GC_ID gc=1;
static GR_COLOR nano_colour(colour c){return c;}
static unsigned drawings,images;
void GrSetGCForeground(GR_GC_ID id,GR_COLOR c){assert(id==1);}
RiImage *ri_image_new(unsigned w,unsigned h,const RiLimits *limits)
{
    RiImage *im=malloc(sizeof(*im));size_t n=(size_t)w*h*4;
    assert(im&&w<=256&&h<=128);im->width=w;im->height=h;im->opaque=0;
    im->rgba=calloc(n+32,1);assert(im->rgba);memset(im->rgba+n,0xa5,32);images++;
    return im;
}
static void guarded(const RiImage *im)
{ unsigned i;for(i=0;i<32;i++)assert(im->rgba[(size_t)im->width*im->height*4+i]==0xa5); }
void ri_nano_forget(const RiImage *im){guarded(im);}
void ri_image_free(RiImage *im){guarded(im);free(im->rgba);free(im);images--;}
int ri_nano_draw_rects(const RiImage *im,GR_DRAW_ID target,GR_GC_ID id,
                      const struct fde_rect *rects,unsigned count,unsigned tint);
#include "../frontend/font-paint.inc"
int ri_nano_draw_rects(const RiImage *im,GR_DRAW_ID target,GR_GC_ID id,
                      const struct fde_rect *rects,unsigned count,unsigned tint)
{
    unsigned i;assert(target==1&&id==1&&tint==1&&count>0&&count<=9);guarded(im);
    for(i=0;i<count;i++) {
        const struct fde_rect *r=rects+i;
        assert(r->sw&&r->sh&&r->sw==r->width&&r->sh==r->height);
        assert(r->sx+r->sw<=im->width&&r->sy+r->sh<=im->height);
    }
    drawings++;return RI_OK;
}
int main(int argc,char **argv)
{
    unsigned face,size,i,slot,per;struct nf_atlas *a;struct nf_style f;
    struct plot_context p={{0,0,600,400},{0,0,600,400},0,0};
    struct redraw_context ctx={.priv=&p};plot_font_style_t style;
    const unsigned sizes[]={8,13,16,18,20,24,32,40,48,64};
    assert(nf_load(argc>1?argv[1]:"ports/netsurf/fonts/web.nfa"));
    for(face=0;face<12;face++)for(size=0;size<sizeof(sizes)/sizeof(*sizes);size++) {
        f.face=face;f.size=sizes[size];per=nf_page_size(f.size);
        for(i=0;i<nf_count;i+=per) {
            uint32_t cp=nf_u16(nf_data+20+i*2UL);
            a=nf_atlas(f,cp,ru_width(cp),&slot);assert(a&&a->image&&slot==0);guarded(a->image);assert(images<=3);
        }
        memset(&style,0,sizeof(style));style.family=face/4;style.size=(long)f.size*768;
        style.weight=face&1?700:400;style.flags=face&2?FONTF_ITALIC:0;
        {
            const char *text="WW ii Á é é — Ő Украïна Є ґ Ελληνικά 中 🙂 abcdefghijklmnopqrstuvwxyz";
            assert(nf_paint(&ctx,&style,-20,100,text,strlen(text))==NSERROR_OK);
        }
    }
    nf_atlas_clear();assert(images==0);free(nf_data);
    printf("Font paint: all 12 faces, 10 sizes, every glyph page, raster guards and %u draw batches passed\n",drawings);
    return 0;
}
