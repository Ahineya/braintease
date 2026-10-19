/* SPDX-License-Identifier: GPL-2.0-only */
/* Nano-X frontend for NetSurf's genuine DOM/CSS/layout/browser core. */
#include <netsurf_profile.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <strings.h>
#include "../../../libraries/image/nano.h"
#include "../image/content.h"
#include <unistd.h>
#include <nano-X.h>
#include "../../../ports/nanox/ripple/nx_clipboard.h"
#include "netsurf/clipboard.h"
#include "address.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "utils/utf8.h"
#include "utils/log.h"
#include "netsurf/netsurf.h"
#include "netsurf/window.h"
#include "netsurf/misc.h"
#include "netsurf/fetch.h"
#include "netsurf/layout.h"
#include "netsurf/plotters.h"
#include "netsurf/bitmap.h"
#include "netsurf/browser_window.h"
#include "netsurf/keypress.h"
#include "desktop/browser_history.h"
#include "../http/fetch.h"
#include <c16-cache.h>
#define TOOLBAR 64
#define STATUS 22
#define SCROLLBAR 14
#define HOME "http://wiby.me/"
#define RANDOM_URL "http://wiby.me/surprise/"
static GR_WINDOW_ID wid;
static GR_GC_ID gc;
static bool running = true, tracing, dirty = true, chrome_dirty = true, editing, select_address;
static int width = 600, height = 420;
static char address[1024], status_text[256];
static struct rect damage;
struct gui_window { struct browser_window *bw; int sx, sy; };
static struct gui_window window;
static bool caret;
static int caret_x, caret_y, caret_height;
static unsigned paints;
static bool loaded, images_enabled, menu_open;
static int menu_selection;
static unsigned long progress_stamp;
static char transfer_text[160];
static int view_width(void) { return width > SCROLLBAR ? width-SCROLLBAR : 1; }
static int view_height(void) { return height > TOOLBAR+STATUS ? height-TOOLBAR-STATUS : 1; }
static void copy_text(char *dest, size_t capacity, const char *text)
{
    size_t n = strlen(text ? text : "");
    if (n >= capacity) n = capacity-1;
    memcpy(dest,text ? text : "",n); dest[n]=0;
}
#include "font.inc"
#include "common.inc"
#include "plot.inc"
static struct rect viewport(void)
{
    struct rect r = {0, TOOLBAR, view_width(), TOOLBAR+view_height()}; return r;
}
static void full_damage(void) { damage=viewport(); dirty=true; }
static nserror invalidate(struct gui_window *gw, const struct rect *area)
{
    struct rect r, limit=viewport();
    if (!area) { full_damage(); return NSERROR_OK; }
    r=*area; r.x0-=gw->sx; r.x1-=gw->sx; r.y0+=TOOLBAR-gw->sy; r.y1+=TOOLBAR-gw->sy;
    if (!intersect(&r,&limit)) return NSERROR_OK;
    if (!dirty) damage=r;
    else {
        if(r.x0<damage.x0)damage.x0=r.x0;
        if(r.y0<damage.y0)damage.y0=r.y0;
        if(r.x1>damage.x1)damage.x1=r.x1;
        if(r.y1>damage.y1)damage.y1=r.y1;
    }
    dirty=true; return NSERROR_OK;
}
static void scroll_to(int x, int y)
{
    int ew=view_width(), eh=view_height(), mx, my;
    if(window.bw) browser_window_get_extents(window.bw,true,&ew,&eh);
    mx=ew-view_width(); my=eh-view_height(); if(mx<0)mx=0; if(my<0)my=0;
    if(x<0)x=0; if(y<0)y=0; if(x>mx)x=mx; if(y>my)y=my;
    if(x!=window.sx || y!=window.sy) {
        window.sx=x; window.sy=y; full_damage(); chrome_dirty=true;
        if(tracing) fprintf(stderr,"NETSURF_GUI_SCROLL %d %d\n",x,y);
    }
}
static bool get_scroll(struct gui_window *gw, int *x, int *y) { *x=gw->sx; *y=gw->sy; return true; }
static nserror set_scroll(struct gui_window *gw, const struct rect *r)
{
    int x=gw->sx, y=gw->sy;
    if(r->x0==r->x1 && r->y0==r->y1) { x=r->x0; y=r->y0; }
    else {
        if(r->x0<x)x=r->x0; else if(r->x1>x+view_width())x=r->x1-view_width();
        if(r->y0<y)y=r->y0; else if(r->y1>y+view_height())y=r->y1-view_height();
    }
    scroll_to(x,y); return NSERROR_OK;
}
static nserror dimensions(struct gui_window *gw, int *w, int *h)
{ *w=view_width(); *h=view_height(); return NSERROR_OK; }
static void set_status(struct gui_window *gw, const char *s)
{ if(strcmp(status_text,s?s:"")!=0) { copy_text(status_text,sizeof(status_text),s); chrome_dirty=true; } }
static void set_title(struct gui_window *gw, const char *s)
{
    GR_WM_PROPERTIES props;
    char title[128];
    snprintf(title,sizeof(title),"NetSurf: %s",s);
    memset(&props,0,sizeof(props)); props.flags=GR_WM_FLAGS_TITLE; props.title=title;
    GrSetWMProperties(wid,&props);
}
static nserror set_url(struct gui_window *gw, struct nsurl *url)
{ if(!editing)copy_text(address,sizeof(address),nsurl_access(url)); chrome_dirty=true; return NSERROR_OK; }
static void damage_caret(void)
{
    struct rect r={caret_x,caret_y,caret_x+1,caret_y+caret_height};
    if(caret)invalidate(&window,&r);
}
static void place_caret(struct gui_window *gw, int x, int y, int h, const struct rect *clip)
{ damage_caret(); caret=true; caret_x=x; caret_y=y; caret_height=h; damage_caret(); }
static nserror window_event(struct gui_window *gw, enum gui_window_event event)
{
    if(event==GW_EVENT_START_THROBBER)loaded=false;
    if(event==GW_EVENT_NEW_CONTENT) { gw->sx=gw->sy=0; caret=false; loaded=false; full_damage(); }
    if(event==GW_EVENT_REMOVE_CARET) { damage_caret(); caret=false; }
    if(event==GW_EVENT_UPDATE_EXTENT)scroll_to(gw->sx,gw->sy);
    chrome_dirty=true; return NSERROR_OK;
}
static struct gui_window *window_create(struct browser_window *bw, struct gui_window *existing, gui_window_create_flags flags)
{
    if(window.bw)return NULL; /* A bounded single-window profile for now. */
    window.bw=bw;
    wid=GrNewWindowEx(GR_WM_PROPS_APPWINDOW,"NetSurf",GR_ROOT_WINDOW_ID,24,36,width,height,GR_RGB(255,255,255));
    if(!wid) { window.bw=NULL; return NULL; }
    GrSelectEvents(wid,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_UPDATE|GR_EVENT_MASK_CLOSE_REQ|
        GR_EVENT_MASK_KEY_DOWN|GR_EVENT_MASK_BUTTON_DOWN|GR_EVENT_MASK_BUTTON_UP|
        GR_EVENT_MASK_MOUSE_POSITION|GR_EVENT_MASK_MOUSE_EXIT);
    GrMapWindow(wid); GrSetFocus(wid); full_damage();
    if(tracing)fprintf(stderr,"NETSURF_GUI_READY %u\n",wid);
    return &window;
}
static void window_destroy(struct gui_window *gw)
{ gw->bw=NULL; if(wid)GrDestroyWindow(wid); wid=0; running=false; }
static nserror launch_url(struct nsurl *url)
{ set_status(&window,"Unsupported address scheme. Use HTTP, HTTPS, local files or data URLs."); return NSERROR_NO_FETCH_HANDLER; }
static struct gui_misc_table misc_table={.schedule=schedule,.launch_url=launch_url};
static struct gui_window_table window_table={
    .create=window_create,.destroy=window_destroy,.invalidate=invalidate,.get_scroll=get_scroll,
    .set_scroll=set_scroll,.get_dimensions=dimensions,.event=window_event,
    .set_status=set_status,.set_title=set_title,.set_url=set_url,.place_caret=place_caret,
};
static void clipboard_get(char **text,size_t *length) {
    *text=nx_clipboard_get(wid,length);
    if(tracing)fprintf(stderr,"NETSURF_CLIPBOARD_READ %lu\n",(unsigned long)*length);
}
static int copy_to_clipboard(const char *text,size_t length) {
    int result=nx_clipboard_set(wid,text,length);
    if(result<0)set_status(&window,"Unable to copy text to the clipboard.");
    else if(tracing)fprintf(stderr,"NETSURF_CLIPBOARD_WRITE %lu\n",(unsigned long)length);
    return result;
}
static void clipboard_set(const char *text,size_t length,nsclipboard_styles styles[],int count) {
    (void)styles;(void)count;copy_to_clipboard(text,length);
}
static struct gui_clipboard_table clipboard_table={.get=clipboard_get,.set=clipboard_set};
static struct netsurf_table tables={.misc=&misc_table,.window=&window_table,.fetch=&fetch_table,
    .layout=&layout_table,.bitmap=&bitmap_table,.llcache=&c16_cache_table,.clipboard=&clipboard_table};
#include "ui.inc"

int main(int argc, char **argv)
{
    struct browser_window *bw=NULL;
    nsurl *url=NULL;
    const char *initial=HOME,*ca_file=NULL,*cache_path=c16_cache_default_path();
    bool clear_cache=false;
    int i, result=1;
    bool registered=false, options=false, initialized=false, opened=false;
    for(i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--trace"))tracing=true;
        else if(!strcmp(argv[i],"--cacert")&&i+1<argc)ca_file=argv[++i];
        else if(!strcmp(argv[i],"--cache-dir")&&i+1<argc)cache_path=argv[++i];
        else if(!strcmp(argv[i],"--no-disk-cache"))cache_path=NULL;
        else if(!strcmp(argv[i],"--clear-cache"))clear_cache=true;
        else if(!strcmp(argv[i],"--images"))images_enabled=true;
        else if(!strcmp(argv[i],"--no-images"))images_enabled=false;
        else if(!strcmp(argv[i],"--no-javascript")) { /* Reserved safe policy; no JS engine is linked. */ }
        else if(!strcmp(argv[i],"--javascript")) { fputs("netsurf: JavaScript is not available in this build\n",stderr); return 2; }
        else if(!strcmp(argv[i],"--help")) { puts("netsurf [--images|--no-images] [--no-javascript] [--cache-dir DIR|--no-disk-cache] [--clear-cache] [--trace] [--cacert FILE] [URL | file:///path | /absolute/path]\nCtrl+L: address  Alt+Left/Right: history  F5: reload  Ctrl+Q: quit\nImages and JavaScript are off by default. Options: Alt+O or toolbar menu."); return 0; }
        else if(argv[i][0]=='-') { fprintf(stderr,"netsurf: unknown option %s\n",argv[i]); return 2; }
        else initial=argv[i];
    }
    if(GrOpen()<0) { fputs("netsurf: cannot connect to Nano-X (startx first)\n",stderr); return 1; }
    opened=true; gc=GrNewGC();
    if(!nf_load("/usr/share/netsurf/web.nfa")) { fputs("netsurf: missing or invalid /usr/share/netsurf/web.nfa; reinstall the browser data\n",stderr);goto cleanup; }
    if(!gc)goto cleanup;
    GrSetGCUseBackground(gc,GR_FALSE);
    if(netsurf_register(&tables)!=NSERROR_OK)goto cleanup;
    registered=true;
    if(nslog_init(NULL,&argc,argv)!=NSERROR_OK)goto cleanup;
    if(nsoption_init(NULL,NULL,NULL)!=NSERROR_OK)goto cleanup;
    options=true;
    { bitmap_fmt_t format={.layout=BITMAP_LAYOUT_R8G8B8A8,.pma=false};bitmap_set_format(&format); }
    nsoption_set_bool(foreground_images,images_enabled); nsoption_set_bool(background_images,images_enabled);
    nsoption_set_bool(enable_javascript,false);
    nsoption_set_bool(incremental_reflow,true);
    if(netsurf_init(cache_path)!=NSERROR_OK)goto cleanup;
    initialized=true;
    if(clear_cache)c16_cache_clear();
    if(netsurf_images_register()!=NSERROR_OK)goto cleanup;
    netsurf_images_dispose(ri_nano_forget);
    if(netsurf_http_register(ca_file)!=NSERROR_OK)goto cleanup;
    if(browser_address(initial,address,sizeof(address))) { fputs("netsurf: invalid or overlong address\n",stderr);goto cleanup; }
    if(nsurl_create(address,&url)!=NSERROR_OK)goto cleanup;
    if(browser_window_create(BW_CREATE_HISTORY,url,NULL,NULL,&bw)!=NSERROR_OK)goto cleanup;
    while(running) {
        GR_EVENT event;
        unsigned events;
        poll_callbacks();
        if(!running)break;
        update_transfer();
        paint();
        if(!loaded&&browser_window_has_content(bw)&&!browser_window_stop_available(bw)) {
            loaded=true;
            if(tracing) { fprintf(stderr,"NETSURF_GUI_LOADED %s\nNETSURF_GUI_DOCUMENT %s\n",browser_window_get_title(bw),nsurl_access(browser_window_access_url(bw))); fflush(stderr); }
        }
        GrGetNextEventTimeout(&event,next_timeout());
        /* A trackpad burst should scroll to its final position before one
         * redraw, rather than painting the whole viewport for every pulse.
         * Keep the batch finite so fetch/timer callbacks still make progress. */
        for(events=0;events<16&&event.type!=GR_EVENT_TYPE_NONE;events++) {
            handle_event(&event);
            if(!running||events==15)break;
            GrCheckNextEvent(&event);
        }
    }
    result=0;
cleanup:
    if(url)nsurl_unref(url);
    if(bw)browser_window_destroy(bw);
    if(initialized)netsurf_exit();
    nf_atlas_clear();
    ri_nano_clear();
    free(nf_data);nf_data=NULL;
    if(options)nsoption_finalise(NULL,NULL);
    if(registered)nslog_finalise();
    if(opened) { if(gc)GrDestroyGC(gc); if(clip_region)GrDestroyRegion(clip_region); GrClose(); }
    if(result)fputs("netsurf: frontend initialization failed\n",stderr);
    return result;
}
