/* Retain document-space RGB565 tiles in Nano-X pixmaps. Only visible missing
 * tiles are rasterized. A separate viewport backing makes scroll copies safe
 * even when the native window is covered, or a menu/caret overlays its pixels. */
#include <dom/core/document.h>
#include "netsurf/content.h"
#include "content/handlers/html/box.h"
#include "content/handlers/html/html_save.h"
#define RENDER_TILE 256
#define RENDER_TILES 64
struct render_tile { GR_DRAW_ID id; int x,y; bool valid; unsigned long used; };
static struct render_tile render_tiles[RENDER_TILES];
static GR_DRAW_ID render_backing;
static int render_width,render_height,render_x,render_y;
static bool render_valid,render_scan=true,render_scroll_safe;
static unsigned long render_clock;
static unsigned render_hits,render_misses;
static unsigned long render_pixels,render_copied;

static void render_invalidate(const struct rect *area)
{
    unsigned i;
    render_scan=true;
    for(i=0;i<RENDER_TILES;i++) {
        struct render_tile *t=render_tiles+i;
        if(!area || (area->x0<(long)t->x+RENDER_TILE&&area->x1>t->x&&
                     area->y0<(long)t->y+RENDER_TILE&&area->y1>t->y))t->valid=false;
    }
    if(!area || (area->x0<render_x+render_width&&area->x1>render_x&&
                  area->y0<render_y+render_height&&area->y1>render_y))render_valid=false;
}
static void render_clear(void)
{
    unsigned i;
    for(i=0;i<RENDER_TILES;i++) {
        if(render_tiles[i].id)GrDestroyWindow(render_tiles[i].id);
        memset(render_tiles+i,0,sizeof(render_tiles[i]));
    }
    if(render_backing)GrDestroyWindow(render_backing);
    render_backing=0;render_valid=false;render_scan=true;
}
static bool render_safe(void)
{
    struct hlcache_handle *content=browser_window_get_content(window.bw);
    struct box *root,*box;unsigned long visited=0;
    if(!content)return false;
    if(content_get_type(content)==CONTENT_TEXTPLAIN)return true;
    if(content_get_type(content)!=CONTENT_HTML)return false;
    root=box=html_get_box_tree(content);
    if(!root)return false;
    /* Single-threaded engine: inspect the computed box tree only between
     * callbacks. Fall back on viewport-dependent or embedded content. */
    while(box) {
        if(++visited>100000UL)return false;
        if(box->iframe || (box->object&&content_get_type(box->object)==CONTENT_HTML))return false;
        if(box->style && (css_computed_position(box->style)==CSS_POSITION_FIXED ||
                         css_computed_background_attachment(box->style)==CSS_BACKGROUND_ATTACHMENT_FIXED))return false;
        if(box->children){box=box->children;continue;}
        while(box!=root&&!box->next)box=box->parent;
        if(box==root)break;
        box=box->next;
    }
    return true;
}
static struct render_tile *render_slot(int x,int y)
{
    unsigned i;struct render_tile *slot=NULL;
    for(i=0;i<RENDER_TILES;i++) {
        struct render_tile *t=render_tiles+i;
        if(t->id&&t->x==x&&t->y==y){slot=t;break;}
        if(!slot || !t->id || (slot->id&&t->used<slot->used))slot=t;
    }
    if(!slot)return NULL;
    if(!slot->id)slot->id=GrNewPixmap(RENDER_TILE,RENDER_TILE,NULL);
    if(!slot->id) {
        /* Under memory pressure reuse a live slot rather than abandoning the
         * backing store. Already copied pixels do not depend on the tile. */
        for(i=0;i<RENDER_TILES;i++)if(render_tiles[i].id) {slot=render_tiles+i;break;}
        if(!slot->id)return NULL;
    }
    if(slot->x!=x||slot->y!=y)slot->valid=false;
    slot->x=x;slot->y=y;slot->used=++render_clock;
    return slot;
}
static struct render_tile *render_tile_at(int x,int y,struct plot_context *stats)
{
    struct render_tile *slot=render_slot(x,y);
    if(!slot)return NULL;
    if(slot->valid){render_hits++;return slot;}
    {
        struct plot_context p;
        struct redraw_context redraw={.interactive=true,.background_images=true,.plot=&plotters,.priv=&p};
        p.limit.x0=p.limit.y0=0;p.limit.x1=p.limit.y1=RENDER_TILE;p.clip=p.limit;
        p.text_runs=p.rectangles=0;draw_target=slot->id;
        GrSetGCRegion(gc,0);GrSetGCForeground(gc,GR_RGB(255,255,255));
        GrFillRect(draw_target,gc,0,0,RENDER_TILE,RENDER_TILE);
        if(!browser_window_redraw(window.bw,-x,-y,&p.limit,&redraw)){draw_target=wid;return NULL;}
        draw_target=wid;slot->valid=true;render_misses++;
        render_pixels+=(unsigned long)RENDER_TILE*RENDER_TILE;
        stats->text_runs+=p.text_runs;stats->rectangles+=p.rectangles;
    }
    return slot;
}
static bool render_region(int x,int y,int w,int h,struct plot_context *stats)
{
    long tx,ty;int dx,dy,l,t,r,b;
    if(w<=0||h<=0)return true;
    for(ty=((long)window.sy+y)/RENDER_TILE*RENDER_TILE;ty<(long)window.sy+y+h;ty+=RENDER_TILE)
        for(tx=((long)window.sx+x)/RENDER_TILE*RENDER_TILE;tx<(long)window.sx+x+w;tx+=RENDER_TILE) {
            struct render_tile *tile=render_tile_at((int)tx,(int)ty,stats);
            if(!tile)return false;
            dx=tx-window.sx;dy=ty-window.sy;
            l=dx<x?x:dx;t=dy<y?y:dy;
            r=dx+RENDER_TILE;if(r>x+w)r=x+w;
            b=dy+RENDER_TILE;if(b>y+h)b=y+h;
            GrSetGCRegion(gc,0);
            GrCopyArea(render_backing,gc,l,t,r-l,b-t,tile->id,l-dx,t-dy,MWROP_COPY);
        }
    return true;
}
static bool render_paint(struct plot_context *stats)
{
    int w=view_width(),h=view_height(),dx,dy,ax,ay;
    bool shifted,ok=true;uint64_t start,end;
    if(!render_enabled)return false;
    if(render_scan){render_scroll_safe=render_safe();render_scan=false;}
    if(!render_scroll_safe){render_valid=false;return false;}
    nsu_getmonotonic_ms(&start);
    render_hits=render_misses=0;render_pixels=render_copied=0;
    if(!render_backing) {
        render_backing=GrNewPixmap(w,h,NULL);render_width=w;render_height=h;render_valid=false;
        if(!render_backing){render_enabled=false;return false;}
    }
    dx=window.sx-render_x;dy=window.sy-render_y;ax=abs(dx);ay=abs(dy);
    shifted=dx||dy;
    if(render_valid&&shifted&&ax<w&&ay<h) {
        GrSetGCRegion(gc,0);
        GrCopyArea(render_backing,gc,dx<0?-dx:0,dy<0?-dy:0,w-ax,h-ay,
                   render_backing,dx>0?dx:0,dy>0?dy:0,MWROP_COPY);
        render_copied=(unsigned long)(w-ax)*(h-ay);
        if(ay)ok=render_region(0,dy>0?h-ay:0,w,ay,stats);
        if(ok&&ax)ok=render_region(dx>0?w-ax:0,dy<0?ay:0,ax,h-ay,stats);
    } else if(render_valid&&!shifted) {
        /* Exposure/menu dismissal changes no document pixels. Content damage
         * already marks the backing invalid in render_invalidate(). */
        ok=true;
    } else ok=render_region(0,0,w,h,stats);
    draw_target=wid;
    if(!ok){render_valid=false;damage=viewport();return false;}
    GrSetGCRegion(gc,0);
    GrCopyArea(wid,gc,0,TOOLBAR,w,h,render_backing,0,0,MWROP_COPY);
    render_x=window.sx;render_y=window.sy;render_valid=true;
    nsu_getmonotonic_ms(&end);
    if(tracing)fprintf(stderr,"NETSURF_RENDER hit=%u miss=%u raster=%lu reused=%lu ms=%lu\n",
        render_hits,render_misses,render_pixels,render_copied,(unsigned long)(end-start));
    return true;
}
