/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>
#include "serv.h"
#include "nxproto.h"
#include "server.h"
#include "image-wire.h"
#include "../../../libraries/image/pixel.h"
#ifdef NXR_HOSTED
#define DRAW_WIDTH 1920
#define DRAW_HEIGHT 1080
#else
#define DRAW_WIDTH 1024
#define DRAW_HEIGHT 768
#endif
#define IMAGE_LIMIT 16
#define IMAGE_CELLS 65536UL
#define TOTAL_CELLS 131072UL
struct image {
    unsigned long id, cells, received;
    unsigned short *data;
    unsigned width, height, flags;
    int owner;
};
static struct image images[IMAGE_LIMIT];
static unsigned long serial, allocated;
unsigned nxr_image_count(void) {
    unsigned n, count = 0;
    for (n = 0; n < IMAGE_LIMIT; ++n) if (images[n].id) ++count;
    return count;
}
static void release(struct image *s) {
    allocated -= s->cells; free(s->data); memset(s, 0, sizeof(*s));
}
void nxr_image_forget(int fd) {
    unsigned n;
    for (n = 0; n < IMAGE_LIMIT; ++n) if (images[n].id && images[n].owner == fd) release(images + n);
}
int nxr_image_valid(void *packet, unsigned n) {
    struct nxr_image_req *q = packet;
    unsigned long extra;
    if (n < sizeof(*q) || q->version != NXR_IMAGE_VERSION) return 0;
    extra = n - sizeof(*q);
    switch (q->op) {
    case NXR_IMAGE_CREATE: case NXR_IMAGE_DESTROY: return !extra;
    case NXR_IMAGE_UPLOAD:
        return q->count && q->count <= NXR_IMAGE_CHUNK && extra == (unsigned long)q->count * sizeof(unsigned short);
    case NXR_IMAGE_DRAW:
        return q->count && q->count <= 9 && extra == (unsigned long)q->count * sizeof(struct fde_rect);
    }
    return 0;
}
static unsigned create(int fd, struct nxr_image_req *q, unsigned long *id) {
    struct image *slot = NULL;
    GR_PIXMAP *p;
    unsigned n, owned = 0, objects = nxr_image_count();
    unsigned long cells;
    if (!q->width || !q->height || q->width > 512 || q->height > 512 ||
        (q->flags > NXR_IMAGE_MASK_ONLY && q->flags != NXR_IMAGE_ALPHA))
        return NXR_IMAGE_INVALID;
    cells = q->flags == NXR_IMAGE_MASK_ONLY ? 0 : (unsigned long)q->width * q->height;
    if (q->flags == NXR_IMAGE_ALPHA) cells *= 2;
    else if (q->flags) cells += (unsigned long)((q->width + 15) / 16) * q->height;
    if (cells > IMAGE_CELLS) return NXR_IMAGE_INVALID;
    for (n = 0; n < IMAGE_LIMIT; ++n) {
        if (images[n].id && images[n].owner == fd) ++owned;
        if (!images[n].id) slot = images + n;
    }
    for (p = listpp; p; p = p->next) if (!nxr_internal_pixmap(p)) ++objects;
    if (!slot || owned >= 4 || objects >= 16 || cells > TOTAL_CELLS - allocated || serial == 0xffffffffUL)
        return NXR_IMAGE_QUOTA;
    slot->data = malloc(cells * sizeof(unsigned short));
    if (!slot->data) return NXR_IMAGE_NOMEM;
    slot->id = ++serial; slot->owner = fd; slot->cells = cells;
    slot->width = q->width; slot->height = q->height; slot->flags = q->flags;
    slot->received = 0; allocated += cells; *id = slot->id;
    return NXR_IMAGE_OK;
}
struct paint {GR_DRAWABLE *drawable;};
static void span(void *context, int x, int y, unsigned width, unsigned short colour) {
    struct paint *p = context;
    GdSetForegroundPixelVal(p->drawable->psd, colour);
    GdFillRect(p->drawable->psd, p->drawable->x + x, p->drawable->y + y, width, 1);
}
/* Convert scaled/alpha images in bounded stripes, then use the existing
 * clipping/cursor-aware RGB565 blitter. Photographs must not turn every colour
 * change into GdSetForegroundPixelVal + GdFillRect + damage bookkeeping.
 * The screen driver's bulk copy path can use the existing GX/DMA service. */
#define IMAGE_ROWS 4
static unsigned short image_rows[DRAW_WIDTH*IMAGE_ROWS];
static void colour_draw(struct image *s,const struct fde_rect *r,unsigned cw,unsigned ch,
                        struct paint *p,unsigned tinted,unsigned short tint) {
    long right=(long)r->x+r->width,bottom=(long)r->y+r->height;
    int left=r->x<0?0:r->x,top=r->y<0?0:r->y,x,y,yy,rows,width;
    unsigned sx,sy,a,acc,colour;
    unsigned long row,at,pixels=(unsigned long)s->width*s->height;
    PSD psd=p->drawable->psd;SCREENDEVICE source=*psd;
    if(right>cw)right=cw;if(bottom>ch)bottom=ch;
    if(right<=left||bottom<=top)return;
    source.flags=PSF_MEMORY;
    if(!s->flags&&!tinted&&r->sw==r->width&&r->sh==r->height) {
        source.addr=(unsigned char *)s->data;source.pitch=s->width;source.size=pixels;
        source.xres=source.xvirtres=s->width;source.yres=source.yvirtres=s->height;
        GdBlit(psd,p->drawable->x+r->x,p->drawable->y+r->y,r->width,r->height,
               &source,r->sx,r->sy,MWROP_COPY);return;
    }
    width=(int)right-left;source.addr=(unsigned char *)image_rows;
    source.pitch=source.xres=source.xvirtres=width;
    for(y=top;y<bottom;y+=rows) {
        rows=(int)bottom-y;if(rows>IMAGE_ROWS)rows=IMAGE_ROWS;
        GdCheckCursor(psd,p->drawable->x+left,p->drawable->y+y,
                      p->drawable->x+(int)right-1,p->drawable->y+y+rows-1);
        for(yy=0;yy<rows;yy++) {
            unsigned short *out=image_rows+(unsigned long)yy*width;
            sy=r->sy+(unsigned long)(y+yy-(long)r->y)*r->sh/r->height;
            row=(unsigned long)sy*s->width;
            sx=r->sx+(unsigned long)(left-(long)r->x)*r->sw/r->width;
            acc=(unsigned long)(left-(long)r->x)*r->sw%r->width;
            for(x=left;x<right;x++) {
                at=row+sx;colour=tinted?tint:s->data[at];
                a=s->flags==NXR_IMAGE_ALPHA?s->data[pixels+at]:255;
                if(a<255) {
                    int px=p->drawable->x+x,py=p->drawable->y+y+yy;
                    /* Child windows may extend beyond their parent surface.
                     * Never read background pixels that the GC clips away. */
                    colour=GdClipPoint(psd,px,py)?ri_pixel_over565(colour,a,psd->ReadPixel(psd,px,py)):0;
                }
                *out++=colour;
                acc+=r->sw;while(acc>=r->width){acc-=r->width;sx++;}
            }
        }
        source.yres=source.yvirtres=rows;source.size=(unsigned long)width*rows;
        GdBlit(psd,p->drawable->x+left,p->drawable->y+y,width,rows,&source,0,0,MWROP_COPY);
        GdFixCursor(psd);
    }
}
static unsigned draw(struct image *s, struct nxr_image_req *q) {
    struct fde_pixels source;
    struct fde_rect *rects = (void *)(q + 1);
    GR_GC *gc = GsFindGC(q->gc);
    GR_WINDOW *w = GsFindWindow(q->target);
    GR_PIXMAP *p = w ? NULL : GsFindPixmap(q->target);
    struct paint paint;
    unsigned i, cw, ch;
    unsigned short tint;
    unsigned long pixels = (unsigned long)s->width * s->height, work = 0;
    if (!gc || gc->owner != curclient || (gc->mode & GR_MODE_DRAWMASK) != GR_MODE_COPY ||
        (!w && !p) || (w && w->owner != curclient) || (p && (p->owner != curclient || nxr_internal_pixmap(p))) ||
        q->flags & ~NXR_IMAGE_TINT)
        return NXR_IMAGE_INVALID;
    source.width = s->width; source.height = s->height;
    source.pixels = s->flags == NXR_IMAGE_MASK_ONLY ? NULL : s->data;
    source.mask = s->flags && s->flags != NXR_IMAGE_ALPHA ? s->data + (s->flags == NXR_IMAGE_MASK_ONLY ? 0 : pixels) : NULL;
    cw = w ? w->width : p->width; ch = w ? w->height : p->height;
    if (!cw || !ch || cw > DRAW_WIDTH || ch > DRAW_HEIGHT) return NXR_IMAGE_INVALID;
    for (i = 0; i < q->count; ++i) {
        long left=rects[i].x,top=rects[i].y;
        long right=left+rects[i].width,bottom=top+rects[i].height;
        if (!fde_raster_valid(&source, rects + i)) return NXR_IMAGE_INVALID;
        if(left<0)left=0;if(top<0)top=0;
        if(right>cw)right=cw;if(bottom>ch)bottom=ch;
        /* Only visible destination pixels consume raster work. A large scaled
         * image clipped to a small VM window must not fail the work budget. */
        if(right>left&&bottom>top)work+=(unsigned long)(right-left)*(bottom-top);
        if (work > (unsigned long)DRAW_WIDTH * DRAW_HEIGHT) return NXR_IMAGE_INVALID;
    }
    if (s->received != s->cells) return NXR_IMAGE_INCOMPLETE;
    nxr_draw_target(NULL);
    if (GsPrepareDrawing(q->target, q->gc, &paint.drawable) == GR_DRAW_TYPE_NONE)
        return NXR_IMAGE_INVALID;
    tint = gr_foreground;
    for (i = 0; i < q->count; ++i) {
        if(!s->flags||s->flags == NXR_IMAGE_ALPHA)
            colour_draw(s,rects+i,cw,ch,&paint,q->flags & NXR_IMAGE_TINT,tint);
        else fde_raster(&source, rects + i, cw, ch, q->flags & NXR_IMAGE_TINT, tint, span, &paint);
    }
    /* Span colours are private to this draw, never a change to the user's GC. */
    gc->changed = GR_TRUE;
    return NXR_IMAGE_OK;
}
void nxr_image_dispatch(int fd, void *packet) {
    struct nxr_image_req *q = packet;
    struct nxr_image_reply reply;
    struct image *s = NULL;
    unsigned n,i;
    short type = NXR_IMAGE;
    reply.id = q->id; reply.status = NXR_IMAGE_INVALID;
    curclient = GsFindClient(fd);
    if (!curclient) return;
    if (q->op == NXR_IMAGE_CREATE) reply.status = create(fd, q, &reply.id);
    else {
        for (n = 0; n < IMAGE_LIMIT; ++n)
            if (images[n].id && images[n].id == q->id && images[n].owner == fd) s = images + n;
        if (s) {
            if (q->op == NXR_IMAGE_DESTROY) {release(s); reply.status = NXR_IMAGE_OK;}
            else if (q->op == NXR_IMAGE_DRAW) reply.status = draw(s, q);
            else if (q->op == NXR_IMAGE_UPLOAD && q->offset == s->received &&
                     q->count <= s->cells - s->received) {
                const unsigned short *input=(const void *)(q+1);
                for(i=0;i<q->count;i++)
                    if(s->flags == NXR_IMAGE_ALPHA && s->received+i >= s->cells/2 && input[i]>255)break;
                if(i==q->count) {
                    memcpy(s->data + s->received, input, q->count * sizeof(unsigned short));
                    s->received += q->count; reply.status = NXR_IMAGE_OK;
                }
            }
        }
    }
    nxr_write(fd, &type, sizeof(type)); nxr_write(fd, &reply, sizeof(reply));
}
