/* SPDX-License-Identifier: GPL-2.0-only */
#include <fde_nano.h>
/* Native builds reserve eight retained text resources independently of photos.
 * Keep their glyph atlases alive across the common body/heading/UI styles. */
#ifdef RIPPLE_NETSURF_HOSTED
#define NF_ATLASES 8
#else
#define NF_ATLASES 3
#endif
struct nf_atlas {
    struct nf_style style;
    unsigned page,count;uint32_t fallback;
    unsigned long used;
    RiImage *image;
    struct nf_glyph glyphs[96];
    unsigned xs[96],ys[96];
};
static struct nf_atlas nf_atlases[NF_ATLASES];
static unsigned long nf_clock;
static void nf_atlas_free(struct nf_atlas *a)
{ if(a->image){ri_nano_forget(a->image);ri_image_free(a->image);}memset(a,0,sizeof(*a)); }
static void nf_atlas_clear(void)
{ unsigned i;for(i=0;i<NF_ATLASES;i++)nf_atlas_free(nf_atlases+i); }
static void nf_raster(struct nf_style f,uint32_t cp,struct nf_glyph *g,RiImage *image,unsigned x,unsigned y)
{
    unsigned char source[4096];unsigned sw,sh,xx,yy,i=0,run,a,b,c,d;
    unsigned long at,end,fx,fy,value;unsigned sx,sy,wx,wy;
    if(!g->width||!g->height)return;
    if(g->record) {
        const unsigned char *r=g->record;
        sw=r[8];sh=r[9];at=nf_u32(r);end=at+nf_u16(r+4);
        while(at<end) { a=nf_data[at++];run=(a>>4)+1;while(run--)source[i++]=(a&15)*17; }
    } else {
        sw=ru_glyph_width(cp)*8;sh=16;if(sw>16)sw=16;
        for(yy=0;yy<sh;yy++) {
            unsigned bits=ru_font_row(cp,yy);
            for(xx=0;xx<sw;xx++)source[yy*sw+xx]=(bits&(0x8000U>>xx))?255:0;
        }
    }
    if(!sw||!sh)return;
    for(yy=0;yy<g->height;yy++) {
        long pos=(long)(((2UL*yy+1)*sh*128UL)/g->height)-128;
        if(pos<0)pos=0;if(pos>(long)(sh-1)*256)pos=(long)(sh-1)*256;
        fy=pos;sy=fy>>8;wy=fy&255;
        for(xx=0;xx<g->width;xx++) {
            long pos=(long)(((2UL*xx+1)*sw*128UL)/g->width)-128;
            if(pos<0)pos=0;if(pos>(long)(sw-1)*256)pos=(long)(sw-1)*256;
            fx=pos;sx=fx>>8;wx=fx&255;
            a=source[sy*sw+sx];b=source[sy*sw+(sx+1<sw?sx+1:sx)];
            c=source[(sy+1<sh?sy+1:sy)*sw+sx];d=source[(sy+1<sh?sy+1:sy)*sw+(sx+1<sw?sx+1:sx)];
            value=((unsigned long)a*(256-wx)+(unsigned long)b*wx)*(256-wy)
                +((unsigned long)c*(256-wx)+(unsigned long)d*wx)*wy;
            image->rgba[((y+yy)*(unsigned long)image->width+x+xx)*4+3]=(value+32768UL)>>16;
        }
    }
}
static struct nf_atlas *nf_atlas(struct nf_style f,uint32_t cp,unsigned cells,unsigned *slot)
{
    unsigned count=nf_page_size(f.size),i,x=0,y=0,row=0,w,h;
    int index=nf_index(cp);unsigned page=index<0?0:(unsigned)index/count;
    struct nf_atlas *a=nf_atlases;uint32_t fallback=index<0?cp:0;
    *slot=index<0?0:(unsigned)index%count;
    for(i=0;i<NF_ATLASES;i++) {
        struct nf_atlas *p=nf_atlases+i;
        if(p->image&&p->style.face==f.face&&p->style.size==f.size&&p->page==page&&p->fallback==fallback) { p->used=++nf_clock;return p; }
        if(!p->image|| (a->image&&p->used<a->used))a=p;
    }
    nf_atlas_free(a);a->style=f;a->page=page;a->fallback=fallback;
    a->count=index<0?1:count;if(index>=0&&page*count+a->count>nf_count)a->count=nf_count-page*count;
    for(i=0;i<a->count;i++) {
        uint32_t code=index<0?cp:nf_u16(nf_data+20+(page*count+i)*2UL);
        struct nf_glyph *g=a->glyphs+i;*g=nf_glyph(f,code,index<0?cells:ru_width(code));
        if(g->width>=256||g->height>=128){nf_atlas_free(a);return NULL;}
        w=g->width+1;h=g->height+1;
        if(x+w>256){y+=row;x=row=0;}
        if(y+h>128){nf_atlas_free(a);return NULL;}
        a->xs[i]=x;a->ys[i]=y;x+=w;if(h>row)row=h;
    }
    a->image=ri_image_new(256,y+row,NULL);if(!a->image){nf_atlas_free(a);return NULL;}
    for(i=0;i<a->count;i++) {
        uint32_t code=index<0?cp:nf_u16(nf_data+20+(page*count+i)*2UL);
        nf_raster(f,code,a->glyphs+i,a->image,a->xs[i],a->ys[i]);
    }
    a->used=++nf_clock;return a;
}
static nserror nf_paint(const struct redraw_context *ctx,const plot_font_style_t *style,int x,int y,const char *s,size_t n)
{
    struct plot_context *p=ctx->priv;struct nf_style f=nf_style(style);
    size_t at=0,next;unsigned cells,slot,count=0;uint32_t cp;
    struct nf_atlas *batch=NULL,*a;struct fde_rect rects[9];nserror result=NSERROR_OK;
    if((long)y+f.size<=p->clip.y0||(long)y-f.size*2>=p->clip.y1)return NSERROR_OK;
    GrSetGCForeground(gc,nano_colour(style->foreground));
    while(at<n) {
        struct nf_glyph g;
        cp=nf_cluster(s,n,at,&next,&cells);at=next;g=nf_glyph(f,cp,cells);
        if((long)x+g.left>=p->clip.x1)break;
        if(g.width&&g.height&&(long)x+g.left+g.width>p->clip.x0&&(long)y+g.top+g.height>p->clip.y0&&y+g.top<p->clip.y1) {
            /* Flush before choosing a different client atlas: allocation may
             * evict the oldest one, including the pending batch's storage. */
            if(count) {
                int index=nf_index(cp);unsigned per=nf_page_size(f.size);
                if(batch->fallback!=(index<0?cp:0)||batch->page!=(index<0?0:(unsigned)index/per)||count==9) {
                    if(ri_nano_draw_rects(batch->image,draw_target,gc,rects,count,1)!=RI_OK)return NSERROR_NOMEM;
                    p->text_runs++;count=0;
                }
            }
            a=nf_atlas(f,cp,cells,&slot);if(!a)return NSERROR_NOMEM;batch=a;
            rects[count].x=x+g.left;rects[count].y=y+g.top;rects[count].sx=a->xs[slot];rects[count].sy=a->ys[slot];
            rects[count].sw=rects[count].width=g.width;rects[count].sh=rects[count].height=g.height;count++;
        }
        if((long)x+g.advance>32760)break;x+=g.advance;
    }
    if(count) {
        if(ri_nano_draw_rects(batch->image,draw_target,gc,rects,count,1)!=RI_OK)result=NSERROR_NOMEM;
        p->text_runs++;
    }
    return result;
}
