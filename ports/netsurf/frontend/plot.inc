/* SPDX-License-Identifier: GPL-2.0-only */
static GR_REGION_ID clip_region;
struct plot_context { struct rect limit, clip; unsigned text_runs, rectangles; };
static GR_COLOR nano_colour(colour c) { return GR_RGB(c & 255, (c >> 8) & 255, (c >> 16) & 255); }
static bool intersect(struct rect *r, const struct rect *limit)
{
    if (r->x0 < limit->x0) r->x0 = limit->x0;
    if (r->y0 < limit->y0) r->y0 = limit->y0;
    if (r->x1 > limit->x1) r->x1 = limit->x1;
    if (r->y1 > limit->y1) r->y1 = limit->y1;
    return r->x0 < r->x1 && r->y0 < r->y1;
}
static nserror plot_clip(const struct redraw_context *ctx, const struct rect *area)
{
    struct plot_context *p = ctx->priv;
    struct rect clipped = *area;
    GR_RECT r;
    if(!clip_region)clip_region=GrNewRegion();
    if(!clip_region)return NSERROR_NOMEM;
    GrSubtractRegion(clip_region,clip_region,clip_region);
    if (intersect(&clipped, &p->limit)) {
        r.x = clipped.x0; r.y = clipped.y0; r.width = clipped.x1 - clipped.x0; r.height = clipped.y1 - clipped.y0;
        GrUnionRectWithRegion(clip_region, &r);
    }
    p->clip=clipped;
    GrSetGCRegion(gc,clip_region);
    return NSERROR_OK;
}
static void stroke(const plot_style_t *style)
{ GrSetGCForeground(gc, nano_colour(style->stroke_colour)); }
/* Only audited Line/Point primitives are enabled in the private protocol. */
static void styled_line(const plot_style_t *style, int x0, int y0, int x1, int y1)
{
    long dx, dy, error, twice;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    unsigned step = 0, dash = style->stroke_type == PLOT_OP_TYPE_DOT ? 1 : 4;
    if (style->stroke_type == PLOT_OP_TYPE_SOLID) { GrLine(wid,gc,x0,y0,x1,y1); return; }
    dx = labs((long)x1-x0); dy = -labs((long)y1-y0); error = dx+dy;
    if (dx > 16384 || dy < -16384) return;
    for (;;) {
        if ((step++ / dash) % 2 == 0) GrPoint(wid,gc,x0,y0);
        if (x0 == x1 && y0 == y1) break;
        twice = 2*error;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}
static nserror plot_rectangle(const struct redraw_context *ctx, const plot_style_t *style, const struct rect *area)
{
    struct plot_context *p = ctx->priv;
    struct rect r = *area;
    if (style->fill_type != PLOT_OP_TYPE_NONE && style->fill_colour != NS_TRANSPARENT && intersect(&r, &p->limit)) {
        GrSetGCForeground(gc, nano_colour(style->fill_colour));
        GrFillRect(wid, gc, r.x0, r.y0, r.x1-r.x0, r.y1-r.y0); p->rectangles++;
    }
    if (style->stroke_type != PLOT_OP_TYPE_NONE && area->x1 > area->x0 && area->y1 > area->y0) {
        int inset, width = plot_style_fixed_to_int(style->stroke_width);
        if (width < 1) width = 1;
        if (width > 32) return NSERROR_BAD_SIZE;
        stroke(style);
        for (inset = 0; inset < width && area->x1-area->x0 > 2*inset && area->y1-area->y0 > 2*inset; inset++) {
            int l=area->x0+inset, t=area->y0+inset, r=area->x1-1-inset, b=area->y1-1-inset;
            styled_line(style,l,t,r,t); styled_line(style,r,t,r,b);
            styled_line(style,r,b,l,b); styled_line(style,l,b,l,t);
        }
    }
    return NSERROR_OK;
}
static nserror plot_line(const struct redraw_context *ctx, const plot_style_t *style, const struct rect *line)
{
    if (style->stroke_type != PLOT_OP_TYPE_NONE) {
        stroke(style); styled_line(style, line->x0, line->y0, line->x1, line->y1);
    }
    return NSERROR_OK;
}
/* Shapes are disabled in the Nano-X engine profile. Fill bounded polygons
 * with scan lines, retaining the server's GC/window clipping. */
static nserror plot_polygon(const struct redraw_context *ctx, const plot_style_t *style, const int *points, unsigned n)
{
    struct plot_context *p = ctx->priv;
    int y, i, j, count, xs[64], signs[64], swap, winding, start;
    if (n > 64) return NSERROR_BAD_SIZE;
    for (i = 0; i < (int)(n*2); i++) if (points[i] < -8192 || points[i] > 8192) return NSERROR_BAD_SIZE;
    if (style->fill_type == PLOT_OP_TYPE_NONE || style->fill_colour == NS_TRANSPARENT) return NSERROR_OK;
    GrSetGCForeground(gc, nano_colour(style->fill_colour));
    for (y = p->limit.y0; y < p->limit.y1; y++) {
        count = 0;
        for (i = 0, j = (int)n-1; i < (int)n; j = i++) {
            int x0 = points[j*2], y0 = points[j*2+1], x1 = points[i*2], y1 = points[i*2+1];
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y))
            {
                xs[count] = x0 + (long)(x1-x0)*(y-y0)/(y1-y0);
                signs[count++] = y1 > y0 ? 1 : -1;
            }
        }
        for (i = 1; i < count; i++) for (j = i; j > 0 && xs[j] < xs[j-1]; j--) {
            swap = xs[j]; xs[j] = xs[j-1]; xs[j-1] = swap;
            swap = signs[j]; signs[j] = signs[j-1]; signs[j-1] = swap;
        }
        winding = 0; start = 0;
        for (i = 0; i < count; i++) {
            if (!winding) start = xs[i];
            winding += signs[i];
            if (!winding && xs[i] > start) GrLine(wid,gc,start,y,xs[i]-1,y);
        }
    }
    return NSERROR_OK;
}
static nserror plot_disc(const struct redraw_context *ctx, const plot_style_t *style, int x, int y, int radius)
{
    int dx = radius, dy = 0, error = 1-radius;
    if (radius < 0 || radius > 4096) return NSERROR_BAD_SIZE;

    if (style->fill_type != PLOT_OP_TYPE_NONE) {
        GrSetGCForeground(gc, nano_colour(style->fill_colour));
        while (dx >= dy) {
            GrLine(wid,gc,x-dx,y+dy,x+dx,y+dy); GrLine(wid,gc,x-dx,y-dy,x+dx,y-dy);
            GrLine(wid,gc,x-dy,y+dx,x+dy,y+dx); GrLine(wid,gc,x-dy,y-dx,x+dy,y-dx);
            dy++;
            if (error < 0) error += 2*dy+1; else { dx--; error += 2*(dy-dx)+1; }
        }
    }
    if (style->stroke_type != PLOT_OP_TYPE_NONE) {
        dx=radius; dy=0; error=1-radius; stroke(style);
        while (dx >= dy) {
            GrPoint(wid,gc,x+dx,y+dy); GrPoint(wid,gc,x-dx,y+dy);
            GrPoint(wid,gc,x+dx,y-dy); GrPoint(wid,gc,x-dx,y-dy);
            GrPoint(wid,gc,x+dy,y+dx); GrPoint(wid,gc,x-dy,y+dx);
            GrPoint(wid,gc,x+dy,y-dx); GrPoint(wid,gc,x-dy,y-dx);
            dy++;
            if(error<0) error+=2*dy+1; else { dx--; error+=2*(dy-dx)+1; }
        }
    }
    return NSERROR_OK;
}
static nserror plot_arc(const struct redraw_context *ctx, const plot_style_t *s, int x, int y, int r, int a, int b)
{ return NSERROR_NOT_IMPLEMENTED; }
static nserror plot_path(const struct redraw_context *ctx, const plot_style_t *s, const float *v, unsigned n, const float t[6])
{ return NSERROR_NOT_IMPLEMENTED; }
static nserror plot_bitmap(const struct redraw_context *ctx, struct bitmap *b, int x, int y, int w, int h, colour bg, bitmap_flags_t f)
{
    struct plot_context *p=ctx->priv;
    struct RiClip clip={p->clip.x0,p->clip.y0,p->clip.x1,p->clip.y1};
    int result;
    if(!images_enabled||w<=0||h<=0)return NSERROR_OK;
    result=ri_nano_draw_cached((RiImage *)b,wid,gc,x,y,w,h,&clip,
        (f&BITMAPF_REPEAT_X?RI_REPEAT_X:0)|(f&BITMAPF_REPEAT_Y?RI_REPEAT_Y:0));
    return result==RI_OK?NSERROR_OK:result==RI_MEMORY?NSERROR_NOMEM:NSERROR_BAD_SIZE;
}
#include "font-paint.inc"
static nserror plot_text(const struct redraw_context *ctx,const plot_font_style_t *style,
                         int x,int y,const char *s,size_t n)
{ return nf_paint(ctx,style,x,y,s,n); }
static const struct plotter_table plotters = {
    .clip = plot_clip, .rectangle = plot_rectangle, .line = plot_line, .text = plot_text,
    .polygon = plot_polygon, .disc = plot_disc, .arc = plot_arc, .path = plot_path,
    .bitmap = plot_bitmap, .option_knockout = false,
};
