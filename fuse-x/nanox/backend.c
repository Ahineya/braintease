/* SPDX-License-Identifier: GPL-2.0-or-later */
/* One private canvas per Nano-X top-level, shared by its child widgets.
 * Root coordinates are local to that top-level; macOS owns desktop placement. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "screen.h"
#include "native.h"
#define VIEWS 16
struct view {
    GR_WINDOW *window;
    PSD canvas;
    struct gx_window host;
    unsigned flags,dirty;
    int left,top,right,bottom;
    GR_WINDOW_ID focus;
    char title[64];
};
static struct view views[VIEWS];
static GR_WINDOW *pointer_top;
static unsigned next_event;
static struct view *by_window(GR_WINDOW *w) {unsigned i;for(i=0;i<VIEWS;i++)if(views[i].window==w)return views+i;return NULL;}
static GR_WINDOW *top(GR_WINDOW *w) {unsigned n=0;while(w&&w!=rootwp&&w->parent!=rootwp){if(++n>32)return NULL;w=w->parent;}return w==rootwp?NULL:w;}
void nxh_assign(GR_WINDOW *w,PSD p) {GR_WINDOW *child;w->psd=p;for(child=w->children;child;child=child->siblings)nxh_assign(child,p);}
static void offset(GR_WINDOW *w,int dx,int dy) {GR_WINDOW *child;w->x+=dx;w->y+=dy;for(child=w->children;child;child=child->siblings)offset(child,dx,dy);}
void nxh_damage(PSD canvas,int x,int y,int width,int height) {
    unsigned i;struct view *v;
    if(width<=0||height<=0)return;
    for(i=0;i<VIEWS;i++)if(views[i].window&&views[i].canvas==canvas) {
        v=views+i;
        if(x<0){width+=x;x=0;}if(y<0){height+=y;y=0;}
        if(x+width>canvas->xres)width=canvas->xres-x;if(y+height>canvas->yres)height=canvas->yres-y;
        if(width<=0||height<=0)return;
        if(!v->dirty){v->left=x;v->top=y;v->right=x+width;v->bottom=y+height;v->dirty=1;}
        else {if(x<v->left)v->left=x;if(y<v->top)v->top=y;if(x+width>v->right)v->right=x+width;if(y+height>v->bottom)v->bottom=y+height;}
        return;
    }
}
int nxh_new(GR_WINDOW *w) {
    struct view *v=NULL;unsigned i;
    if(w->parent!=rootwp){w->psd=w->parent->psd;return 1;}
    for(i=0;i<VIEWS;i++)if(!views[i].window){v=views+i;break;}
    if(!v)return 0;
    memset(v,0,sizeof(*v));
    v->canvas=GdCreatePixmap(&scrdev,w->width,w->height,MWIF_RGB565,NULL,0);if(!v->canvas)return 0;
    v->host.version=GX_VERSION;v->host.width=w->width;v->host.height=w->height;v->host.format=GX_RGB565;
    strcpy(v->host.title,"Nano-X");
    if(ioctl(nxr_descriptor(),GX_CREATE_HIDDEN,&v->host)<0){v->canvas->FreeMemGC(v->canvas);memset(v,0,sizeof(*v));return 0;}
    v->window=w;v->flags=GX_WINDOW_HIDDEN;w->psd=v->canvas;w->x=w->y=w->bordersize=0;
    nxh_damage(v->canvas,0,0,w->width,w->height);return 1;
}
void nxh_destroy(GR_WINDOW *w) {
    struct view *v=by_window(w);unsigned i;struct gx_handle h;
    for(i=0;i<VIEWS;i++)if(views[i].focus==w->id)views[i].focus=0;
    if(pointer_top==w)pointer_top=NULL;
    if(!v)return;
    h.version=GX_VERSION;h.handle=v->host.window;ioctl(nxr_descriptor(),GX_DESTROY,&h);
    v->canvas->FreeMemGC(v->canvas);memset(v,0,sizeof(*v));
}
int nxh_resize(GR_WINDOW *w,int width,int height) {
    struct view *v=by_window(w);PSD next;unsigned y,count;
    if(!v)return width>0&&height>0;
    if(width<=0||height<=0||width>1024||height>768)return 0;
    next=GdCreatePixmap(&scrdev,width,height,MWIF_RGB565,NULL,0);if(!next)return 0;
    count=width<v->canvas->xres?width:v->canvas->xres;
    for(y=0;y<height&&y<v->canvas->yres;y++)memcpy((unsigned short *)next->addr+(unsigned long)y*next->pitch,
        (unsigned short *)v->canvas->addr+(unsigned long)y*v->canvas->pitch,count);
    nxh_assign(w,next);v->canvas->FreeMemGC(v->canvas);v->canvas=next;v->dirty=0;
    nxh_damage(next,0,0,width,height);clipwp=NULL;return 1;
}
/* Called before the canonical reparent while its old tree is still valid. */
int nxh_reparent(GR_WINDOW *w,GR_WINDOW *parent) {
    struct view *v=by_window(w);
    if(parent==rootwp) {
        if(v)return 1;
        {GR_WINDOW *old=w->parent;int x=w->x,y=w->y;
         w->parent=rootwp;
         if(!nxh_new(w)){w->parent=old;return 0;}
         w->parent=old;w->x=x;w->y=y;}
        nxh_assign(w,w->psd);
    } else {
        if(v)nxh_destroy(w);
        nxh_assign(w,parent->psd);
    }
    return 1;
}
void nxh_reparented(GR_WINDOW *w) {if(w->parent==rootwp){offset(w,-w->x,-w->y);w->bordersize=0;}clipwp=NULL;}
static int configure(struct view *v) {
    struct gx_configure c;struct gx_title t;const char *title=v->window->title;
    unsigned flags=(v->window->props&GR_WM_PROPS_NORESIZE?GX_WINDOW_FIXED:0)|
        (!v->window->realized||!v->window->output?GX_WINDOW_HIDDEN:0);
    if(!title||!*title)title="Nano-X";
    memset(&t,0,sizeof(t));t.version=GX_VERSION;t.window=v->host.window;strncpy(t.title,title,63);
    if(strcmp(t.title,v->title)) {if(ioctl(nxr_descriptor(),GX_TITLE,&t)<0)return -1;strcpy(v->title,t.title);}
    if(flags!=v->flags||v->host.width!=v->window->width||v->host.height!=v->window->height) {
        c.version=GX_VERSION;c.window=v->host.window;c.width=v->window->width;c.height=v->window->height;c.flags=flags;
        if(ioctl(nxr_descriptor(),GX_CONFIGURE,&c)<0)return -1;
        v->flags=flags;v->host.width=c.width;v->host.height=c.height;
    }
    return 0;
}
int nxh_present(void) {
    unsigned i;struct view *v;struct gx_upload u;struct gx_handle h;
    for(i=0;i<VIEWS;i++)if(views[i].window) {
        v=views+i;if(configure(v)<0){fprintf(stderr,"Nano-X: native configure failed for %u (errno %d)\n",v->window->id,errno);return -1;}
        if(!v->dirty||v->flags&GX_WINDOW_HIDDEN)continue;
        memset(&u,0,sizeof(u));u.version=GX_VERSION;u.surface=v->host.surface;
        u.x=v->left;u.y=v->top;u.width=v->right-v->left;u.height=v->bottom-v->top;u.stride=v->canvas->pitch;
        u.pixels=(unsigned short *)v->canvas->addr+(unsigned long)u.y*u.stride+u.x;
        h.version=GX_VERSION;h.handle=v->host.window;
        if(ioctl(nxr_descriptor(),GX_UPLOAD,&u)<0||ioctl(nxr_descriptor(),GX_PRESENT,&h)<0){fprintf(stderr,"Nano-X: native present failed for %u (errno %d)\n",v->window->id,errno);return -1;}
        v->dirty=0;
    }
    return 0;
}
int nxh_event(struct gx_event *e) {
    unsigned i,index;
    for(i=0;i<VIEWS;i++) {
        index=(next_event+i)%VIEWS;if(!views[index].window)continue;
        memset(e,0,sizeof(*e));e->version=GX_VERSION;e->window=views[index].host.window;
        if(ioctl(nxr_descriptor(),GX_EVENT,e)<0)return -1;
        if(e->type){next_event=(index+1)%VIEWS;return 0;}
    }
    memset(e,0,sizeof(*e));return 0;
}
static GR_WINDOW *hit(GR_WINDOW *w,int x,int y,unsigned depth) {
    GR_WINDOW *child,*found;
    if(depth>32||!w->realized||x<w->x||y<w->y||x>=w->x+w->width||y>=w->y+w->height)return NULL;
    if(w->clipregion&&!GdPtInRegion(w->clipregion,x-w->x,y-w->y))return NULL;
    for(child=w->children;child;child=child->siblings)if((found=hit(child,x,y,depth+1)))return found;
    return w;
}
GR_WINDOW *nxh_mouse_window(int x,int y) {GR_WINDOW *found;if(pointer_top&&(found=hit(pointer_top,x,y,0)))return found;return rootwp;}
int nxh_input(struct gx_event *e) {
    unsigned i;struct view *v=NULL;GR_WINDOW *focus;
    for(i=0;i<VIEWS;i++)if(views[i].window&&views[i].host.window==e->window){v=views+i;break;}
    if(!v)return 0;
    curclient=v->window->owner;
    if(!v->window->realized||!v->window->output)return 0;
    if(e->type==GX_CLOSE){GrCloseWindow(v->window->id);return 0;}
    if(e->type==GX_RESIZE) {
        if(!(v->window->props&GR_WM_PROPS_NORESIZE))GrResizeWindow(v->window->id,e->x,e->y);
        /* A refused/failed resize explicitly restores the accepted geometry. */
        v->host.width=v->host.height=0;configure(v);return 0;
    }
    if(e->type==GX_FOCUS) {
        if(e->flags&GX_FOCUSED) {
            focus=v->focus?GsFindWindow(v->focus):NULL;
            if(!focus||top(focus)!=v->window)focus=v->window;
            focusfixed=GR_TRUE;GsSetFocus(focus);pointer_top=v->window;
        } else {
            nxh_release_keys();
            if(top(focuswp)==v->window){v->focus=focuswp->id;GsSetFocus(rootwp);}
            if(pointer_top==v->window)pointer_top=NULL;
        }
        return 0;
    }
    if(e->type==GX_ENTER) {
        pointer_top=e->flags&4?v->window:NULL;GsCheckMouseWindow();return 0;
    }
    pointer_top=v->window;
    if(e->type==GX_KEY&&top(focuswp)!=v->window){focusfixed=GR_TRUE;GsSetFocus(v->window);}
    return 1;
}
void nxh_finish(void) {unsigned i;for(i=0;i<VIEWS;i++)if(views[i].window)nxh_destroy(views[i].window);}
void nxh_activate(GR_WINDOW *w) {struct view *v=by_window(top(w));struct gx_handle h;
    if(!v||!w->realized)return;v->focus=w->id;h.version=GX_VERSION;h.handle=v->host.window;ioctl(nxr_descriptor(),GX_ACTIVATE,&h);}
