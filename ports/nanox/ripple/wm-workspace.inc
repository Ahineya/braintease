/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "fde_nano.h"
#include "fde_workspace.h"
#include "theme.h"
struct fde_drag {int x,y,rootx,rooty,width,height;unsigned edges;};
static FDE_IMAGE wm_theme;
static int wm_theme_attempted;
static int fde_control(GR_WINDOW_INFO *i,int x,int y) {
    int close=i->width-CXCLOSEBOX-CXBORDER-2;
    if(!(i->props&GR_WM_PROPS_CAPTION)||y<CYBORDER||y>=CYBORDER+CYCAPTION)return 0;
    if((i->props&GR_WM_PROPS_CLOSEBOX)&&x>=close&&x<close+CXCLOSEBOX)return FDE_CLOSE;
    if(x>=close-24&&x<close-4&&!(i->props&GR_WM_PROPS_NORESIZE))return FDE_MAXIMIZE;
    if(x>=close-48&&x<close-28)return FDE_MINIMIZE;
    return 0;
}
static void fde_wm_redraw_ncarea(win *window) {
    GR_WINDOW_INFO i,c;GR_WM_PROPERTIES p;struct fde_window_info state;
    struct fde_skin skin;const struct fde_asset *a;GR_GC_ID gc;char title[64];int active,n,close,j;
    GrGetWindowInfo(window->wid,&i);GrGetWindowInfo(window->clientid,&c);
    if(!i.wid||!c.wid)return;
    GrGetWMProperties(window->clientid,&p);
    if(!p.flags){if(p.title)free(p.title);return;}
    active=fde_window_get(window->clientid,&state)==FDE_OK&&(state.state&FDE_WINDOW_ACTIVE);
    gc=GrNewGC();if(!gc){if(p.title)free(p.title);return;}
    GrSetGCForeground(gc,FDE_COLOUR_SURFACE);
    GrFillRect(window->wid,gc,0,0,i.width,CYBORDER+CYCAPTION+1);
    GrFillRect(window->wid,gc,0,i.height-CYBORDER,i.width,CYBORDER);
    GrFillRect(window->wid,gc,0,0,CXBORDER,i.height);
    GrFillRect(window->wid,gc,i.width-CXBORDER,0,CXBORDER,i.height);
    if(!wm_theme_attempted){wm_theme_attempted=1;wm_theme=fde_image_load_file("/usr/share/fusede/themes/slate/theme.fdi");}
    if(wm_theme){a=&fde_theme_assets[active?FDE_ASSET_WINDOW_ACTIVE:FDE_ASSET_WINDOW_INACTIVE];skin=a->skin;skin.centre=0;
        fde_skin_paint(wm_theme,window->wid,gc,a->x,a->y,&skin,0,0,i.width,i.height);}
    GrSetGCUseBackground(gc,GR_FALSE);GrSetGCForeground(gc,active?FDE_COLOUR_TEXT:FDE_COLOUR_MUTED);
    if(i.props&GR_WM_PROPS_CAPTION){
        n=(i.width-100)/8;if(n<0)n=0;if(n>63)n=63;
        if(p.title){strncpy(title,p.title,n);title[n]=0;GrText(window->wid,gc,10,6,title,-1,GR_TFASCII|GR_TFTOP);}
        close=i.width-CXCLOSEBOX-CXBORDER-2;
        if(i.props&GR_WM_PROPS_CLOSEBOX){GrLine(window->wid,gc,close+6,10,close+13,17);GrLine(window->wid,gc,close+6,17,close+13,10);}
        GrLine(window->wid,gc,close-41,17,close-34,17);
        if(!(c.props&GR_WM_PROPS_NORESIZE)){
            if(fde_window_get(window->clientid,&state)==FDE_OK&&(state.state&FDE_WINDOW_MAXIMIZED)){
                GrRect(window->wid,gc,close-18,10,8,7);GrRect(window->wid,gc,close-16,8,8,7);
            }else GrRect(window->wid,gc,close-18,9,9,9);
        }
    }
    if(!(c.props&GR_WM_PROPS_NORESIZE))for(j=0;j<3;j++)
        GrLine(window->wid,gc,i.width-4-j*3,i.height-2,i.width-2,i.height-4-j*3);
    GrDestroyGC(gc);if(p.title)free(p.title);GrFlush();
}
static unsigned fde_edges(GR_WINDOW_INFO *i,int x,int y) {
    unsigned e=0;
    if(x<CXBORDER)e|=1;if(x>=i->width-CXBORDER)e|=2;
    if(y<CYBORDER)e|=4;if(y>=i->height-CYBORDER)e|=8;
    if(x>=i->width-14&&y>=i->height-14)e=2|8;
    return e;
}
static void fde_wm_container_buttondown(win *window,GR_EVENT_BUTTON *e) {
    GR_WINDOW_INFO i,c;struct fde_window_info state;struct fde_drag *d;unsigned edges;
    if(!(e->changebuttons&GR_BUTTON_L)||window->active||window->sizing)return;
    GrGetWindowInfo(window->wid,&i);GrGetWindowInfo(window->clientid,&c);
    window->close=fde_control(&i,e->x,e->y);if(window->close)return;
    fde_window_action(window->clientid,FDE_ACTIVATE);
    if(fde_window_get(window->clientid,&state)==FDE_OK&&(state.state&FDE_WINDOW_MAXIMIZED))return;
    edges=fde_edges(&i,e->x,e->y);
    if(c.props&GR_WM_PROPS_NORESIZE)edges=0;
    if(!edges&&(!(i.props&GR_WM_PROPS_CAPTION)||e->y<CYBORDER||e->y>=CYBORDER+CYCAPTION||
            c.props&GR_WM_PROPS_NOMOVE))return;
    d=malloc(sizeof(*d));if(!d)return;
    d->x=i.x;d->y=i.y;d->width=i.width;d->height=i.height;
    d->rootx=e->rootx;d->rooty=e->rooty;d->edges=edges;
    window->data=d;window->active=!edges;window->sizing=edges!=0;
}
static void fde_drag_position(win *window,int rootx,int rooty) {
    struct fde_drag *d=window->data;struct fde_workspace_info area;GR_WINDOW_INFO c;
    long x,y,w,h,dx,dy;int dw,dh;
    if(!d)return;GrGetWindowInfo(window->clientid,&c);if(!c.wid)return;
    if(fde_workspace_get(&area)!=FDE_OK)return;
    dx=(long)rootx-d->rootx;dy=(long)rooty-d->rooty;
    x=d->x;y=d->y;w=d->width;h=d->height;
    if(!d->edges){x+=dx;y+=dy;
        /* Leave a usable portion of the caption within the work area. */
        if(x<area.x-w+80)x=area.x-w+80;if(x>area.x+(long)area.width-80)x=area.x+(long)area.width-80;
        if(y<area.y)y=area.y;if(y>area.y+(long)area.height-26)y=area.y+(long)area.height-26;
        GrMoveWindow(window->wid,x,y);
    }else{
        if(d->edges&1){x+=dx;w-=dx;}if(d->edges&2)w+=dx;
        if(d->edges&4){y+=dy;h-=dy;}if(d->edges&8)h+=dy;
        if(w<160){if(d->edges&1)x-=160-w;w=160;}if(h<80){if(d->edges&4)y-=80-h;h=80;}
        if(w>area.width)w=area.width;if(h>area.height)h=area.height;
        if(x<area.x)x=area.x;if(y<area.y)y=area.y;
        if(x+w>area.x+(long)area.width)x=area.x+(long)area.width-w;
        if(y+h>area.y+(long)area.height)y=area.y+(long)area.height-h;
        dw=CXFRAME;dh=CYFRAME+CYCAPTION+1;
        if(!(c.props&GR_WM_PROPS_APPFRAME)){dw=dh=c.props&GR_WM_PROPS_BORDER?2:0;
            if(c.props&GR_WM_PROPS_CAPTION)dh+=CYCAPTION;}
        GrResizeWindow(window->clientid,w-dw,h-dh);GrResizeWindow(window->wid,w,h);GrMoveWindow(window->wid,x,y);
    }
    GrSetFocus(window->clientid);GrFlush();
}
static void fde_wm_container_buttonup(win *window,GR_EVENT_BUTTON *e) {
    GR_WINDOW_INFO i;struct fde_window_info state;int control;
    if(!(e->changebuttons&GR_BUTTON_L))return;
    GrGetWindowInfo(window->wid,&i);control=fde_control(&i,e->x,e->y);
    if(window->close&&window->close==control){
        if(control==FDE_MAXIMIZE&&fde_window_get(window->clientid,&state)==FDE_OK&&(state.state&FDE_WINDOW_MAXIMIZED))control=FDE_RESTORE;
        fde_window_action(window->clientid,control);
    }
    window->close=0;
    /* Apply resize only on release, avoiding repeated expensive client buffer
     * allocations while the pointer is moving. Moving remains interactive. */
    if(window->active||window->sizing)fde_drag_position(window,e->rootx,e->rooty);
    free(window->data);window->data=NULL;window->active=window->sizing=0;
}
static void fde_wm_container_mousemoved(win *window,GR_EVENT_MOUSE *e) {
    GR_WINDOW_INFO i;
    if(window->active){fde_drag_position(window,e->rootx,e->rooty);return;}
    GrGetWindowInfo(window->wid,&i);
    if(!(i.props&GR_WM_PROPS_NORESIZE)&&fde_edges(&i,e->x,e->y))set_resize_cursor(window->wid);
    else GrSetWindowCursor(window->wid,0);
}
static void fde_wm_container_mouse_enter(win *window,GR_EVENT_GENERAL *e) {(void)window;(void)e;}
static void fde_wm_container_mouse_exit(win *window,GR_EVENT_GENERAL *e) {if(!window->active&&!window->sizing)GrSetWindowCursor(window->wid,0);}
