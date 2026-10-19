/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Window identity and bounded workspace mechanisms shared by NanoWM and the
 * hosted adapter. NanoWM owns decorations and publishes frame/client bindings;
 * native mode never creates guest frames or invents a macOS desktop geometry. */
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include "serv.h"
#include "server.h"
#include "workspace-wire.h"
#define WINDOWS 256
struct slot {
    GR_WINDOW_ID id, frame, transient;
    unsigned role, edge, extent, state;
    unsigned borderless_added;
    int x, y;
    unsigned width, height;
};
static struct slot slots[WINDOWS];
static unsigned watching[FD_SETSIZE];
static int manager = -1, syncing, panel_focus;
static unsigned long revision = 1, fingerprint;
static struct fde_workspace_info area;
extern GR_CLIENT *root_client;
static GR_WINDOW *find(GR_WINDOW_ID id) {
    GR_WINDOW *w;for(w=listwp;w;w=w->next)if(w->id==id)return w;return NULL;
}
static struct slot *lookup(GR_WINDOW_ID id) {
    unsigned i;for(i=0;i<WINDOWS;i++)if(slots[i].id==id)return slots+i;return NULL;
}
static struct slot *record(GR_WINDOW *w) {
    struct slot *s=lookup(w->id);unsigned i;
    if(s)return s;
    for(i=0;i<WINDOWS;i++)if(!slots[i].id){s=slots+i;memset(s,0,sizeof(*s));s->id=w->id;return s;}
    return NULL;
}
static GR_WINDOW *outer(struct slot *s) {return find(s->frame?s->frame:s->id);}
static int listed(struct slot *s) {
    GR_WINDOW *w=find(s->id);
    return w&&w->output&&(s->frame||w->parent==rootwp);
}
int nxr_workspace_focusable(GR_WINDOW *w) {
    struct slot *s;
    if(panel_focus)return 1;
#ifdef NXR_HOSTED
    return 1; /* An ordinary native panel window follows macOS focus policy. */
#else
    for(;w&&w!=rootwp;w=w->parent){s=lookup(w->id);if(s&&s->role==FDE_ROLE_PANEL)return 0;}
    return 1;
#endif
}
static int active(GR_WINDOW *w) {
    GR_WINDOW *f=focuswp;while(f&&f!=rootwp){if(f==w)return 1;f=f->parent;}return 0;
}
static void info(struct slot *s,struct fde_window_info *out) {
    GR_WINDOW *w=find(s->id),*p=outer(s);
    memset(out,0,sizeof(*out));if(!w||!p)return;
    out->id=s->id;out->transient=s->transient;out->role=s->role;
    out->pid=w->owner?w->owner->processid:0;out->state=s->state;
    if(w->mapped&&p->mapped)out->state|=FDE_WINDOW_MAPPED;
    if(active(w))out->state|=FDE_WINDOW_ACTIVE;
    if(s->frame)out->state|=FDE_WINDOW_DECORATED;
    out->x=p->x;out->y=p->y;out->width=w->width;out->height=w->height;
    if(w->title)strncpy(out->title,w->title,63);
}
/* Set both geometries while preserving the client/frame offset. Resize the
 * client first; a failed hosted resize leaves its accepted geometry intact. */
static int geometry(struct slot *s,int x,int y,unsigned width,unsigned height) {
    GR_WINDOW *w=find(s->id),*p=outer(s);int dx,dy,dw,dh;
    if(!w||!p)return FDE_INVALID;
    dx=w->x-p->x;dy=w->y-p->y;dw=p->width-w->width;dh=p->height-w->height;
    if(width<64||height<48||(long)width<=dw||(long)height<=dh)return FDE_INVALID;
    GrResizeWindow(w->id,width-dw,height-dh);
    if(w->width!=width-dw||w->height!=height-dh)return FDE_FAILED;
    if(p!=w)GrResizeWindow(p->id,width,height);
    GrMoveWindow(p->id,x,y);
    if(p!=w&&(w->x-p->x!=dx||w->y-p->y!=dy))GrMoveWindow(w->id,dx,dy);
    return FDE_OK;
}
static void workarea(void) {
    unsigned i,left=0,right=0,top=0,bottom=0;
    struct slot *s;GR_WINDOW *w;
    memset(&area,0,sizeof(area));
    area.capabilities=FDE_CAP_WINDOWS|FDE_CAP_ACTIVATE|FDE_CAP_MINIMIZE;
#ifdef NXR_HOSTED
    area.capabilities|=FDE_CAP_HOSTED;
#else
    if(manager>=0)area.capabilities|=FDE_CAP_MAXIMIZE|FDE_CAP_PANELS;
    for(i=0;i<WINDOWS;i++)if((s=slots+i)->id&&s->role==FDE_ROLE_PANEL&&
            (w=find(s->id))&&w->mapped){
        switch(s->edge){case FDE_EDGE_TOP:top=s->extent;break;case FDE_EDGE_BOTTOM:bottom=s->extent;break;
        case FDE_EDGE_LEFT:left=s->extent;break;case FDE_EDGE_RIGHT:right=s->extent;break;}
    }
#endif
    area.x=left;area.y=top;
#ifndef NXR_HOSTED
    area.width=rootwp->width-left-right;area.height=rootwp->height-top-bottom;
#endif
}
static void panels(void) {
#ifndef NXR_HOSTED
    unsigned i;struct slot *s;GR_WINDOW *w,*p;int x,y;unsigned width,height;
    for(i=0;i<WINDOWS;i++)if((s=slots+i)->id&&s->role==FDE_ROLE_PANEL&&(w=find(s->id))&&w->mapped){
        x=y=0;width=rootwp->width;height=s->extent;
        if(s->edge==FDE_EDGE_BOTTOM)y=rootwp->height-height;
        if(s->edge==FDE_EDGE_LEFT||s->edge==FDE_EDGE_RIGHT){width=s->extent;height=rootwp->height;
            if(s->edge==FDE_EDGE_RIGHT)x=rootwp->width-width;}
        /* Panels can be thinner than ordinary application minimums. */
        GrResizeWindow(w->id,width,height);GrMoveWindow(w->id,x,y);
        /* Preserve the normal-window order; raise only when a normal window
         * has crossed above this panel, not once per idle/render iteration. */
        for(p=rootwp->children;p&&p!=w;p=p->siblings){struct slot *above=lookup(p->id);
            if(p->realized&&(!above||above->role!=FDE_ROLE_PANEL)){GrRaiseWindow(w->id);break;}}
    }
#endif
}
static void notify(void) {
    GR_CLIENT *c;GR_EVENT_LIST *queued;GR_EVENT *e;
    for(c=root_client;c;c=c->next)if(c->id>=0&&c->id<FD_SETSIZE&&watching[c->id]){
        for(queued=c->eventhead;queued;queued=queued->next)if(queued->event.type==FDE_EVENT_WORKSPACE)break;
        if(!queued&&(e=GsAllocEvent(c))){e->type=FDE_EVENT_WORKSPACE;e->general.wid=GR_ROOT_WINDOW_ID;}
    }
}
void nxr_workspace_sync(void) {
    unsigned i,j;GR_WINDOW *w,*p;struct slot *s;struct fde_window_info out;
    unsigned long hash=5381;unsigned char *bytes;
    if(syncing||!rootwp)return;syncing=1;
    for(i=0;i<WINDOWS;i++)if(slots[i].id){s=slots+i;
        if(!find(s->id)){memset(s,0,sizeof(*s));continue;}
        if(s->frame&&!find(s->frame))s->frame=0;
        if(s->transient&&!find(s->transient))s->transient=0;
    }
    for(w=listwp;w;w=w->next)if(w!=rootwp&&w->parent==rootwp&&w->output&&
        (!w->owner||w->owner->id!=manager))record(w);
    workarea();panels();
#ifndef NXR_HOSTED
    for(i=0;i<WINDOWS;i++)if((s=slots+i)->id&&(s->state&FDE_WINDOW_MAXIMIZED))
        geometry(s,area.x,area.y,area.width,area.height);
    for(i=0;i<WINDOWS;i++)if((s=slots+i)->id&&s->frame&&!(s->state&FDE_WINDOW_MAXIMIZED)&&(p=outer(s))){
        int x=p->x,y=p->y;
        if(x<area.x-(long)p->width+80)x=area.x-(long)p->width+80;
        if(x>area.x+(long)area.width-80)x=area.x+(long)area.width-80;
        if(y<area.y)y=area.y;
        if(y>area.y+(long)area.height-26)y=area.y+(long)area.height-26;
        if(x!=p->x||y!=p->y)GrMoveWindow(p->id,x,y);
    }
#endif
    for(i=0;i<WINDOWS;i++)if(slots[i].id&&listed(slots+i)){
        info(slots+i,&out);++area.count;bytes=(void *)&out;
        for(j=0;j<sizeof(out);j++)hash=(hash*33)^bytes[j];
    }
    /* Stack changes matter to observers even when focus didn't change. */
    for(p=rootwp->children;p;p=p->siblings)hash=(hash*33)^p->id;
    hash=(hash*33)^area.x;hash=(hash*33)^area.y;hash=(hash*33)^area.width;hash=(hash*33)^area.height;
    hash=(hash*33)^area.capabilities;
    if(hash!=fingerprint){fingerprint=hash;++revision;notify();}
    area.revision=revision;syncing=0;
}
/* Called before the manager's resource destruction: detach applications from
 * its frames so a WM crash/restart cannot recursively destroy other clients. */
void nxr_workspace_forget(int fd) {
    unsigned i;struct slot *s;GR_WINDOW *w,*p;GR_CLIENT *saved=curclient;
    if(fd>=0&&fd<FD_SETSIZE)watching[fd]=0;
    if(fd==manager){
        for(i=0;i<WINDOWS;i++)if((s=slots+i)->id&&s->frame&&(w=find(s->id))&&(p=outer(s))){
            int x=w->x,y=w->y;curclient=w->owner;
            GrReparentWindow(w->id,GR_ROOT_WINDOW_ID,x,y);s->frame=0;
            if(s->state&FDE_WINDOW_MINIMIZED)GrUnmapWindow(w->id);
        }
        manager=-1;
    }
    curclient=saved;
}
static int action(struct slot *s,unsigned op) {
    GR_WINDOW *w=find(s->id),*p=outer(s);int status;
    if(!w||!p||(s->role==FDE_ROLE_PANEL&&op!=FDE_ACTIVATE))return FDE_INVALID;
    if(op==FDE_CLOSE){GrCloseWindow(w->id);return FDE_OK;}
    if(op==FDE_MINIMIZE){s->state|=FDE_WINDOW_MINIMIZED;GrUnmapWindow(p->id);return FDE_OK;}
    if(op==FDE_MAXIMIZE){
        if(!(area.capabilities&FDE_CAP_MAXIMIZE))return FDE_UNSUPPORTED;
        if(w->props&GR_WM_PROPS_NORESIZE)return FDE_DENIED;
        if(!(s->state&FDE_WINDOW_MAXIMIZED)){
            s->x=p->x;s->y=p->y;s->width=p->width;s->height=p->height;
            status=geometry(s,area.x,area.y,area.width,area.height);if(status)return status;
            s->state|=FDE_WINDOW_MAXIMIZED;
        }
    }else if(op==FDE_RESTORE){
        if(s->state&FDE_WINDOW_MAXIMIZED){
            status=geometry(s,s->x,s->y,s->width,s->height);if(status)return status;
            s->state&=~FDE_WINDOW_MAXIMIZED;
        }
    }else if(op!=FDE_ACTIVATE)return FDE_INVALID;
    s->state&=~FDE_WINDOW_MINIMIZED;GrMapWindow(w->id);if(p!=w)GrMapWindow(p->id);
    GrRaiseWindow(p->id);panel_focus=1;GrSetFocus(w->id);panel_focus=0;return FDE_OK;
}
int nxr_workspace_key(unsigned key,unsigned modifiers) {
#ifndef NXR_HOSTED
    unsigned i;struct slot *s,*first=NULL,*next=NULL;GR_WINDOW_ID current=0;
    if(key!=MWKEY_TAB||!(modifiers&MWKMOD_ALT))return 0;
    nxr_workspace_sync();
    for(i=0;i<WINDOWS;i++)if((s=slots+i)->id&&listed(s)&&s->role!=FDE_ROLE_PANEL&&active(find(s->id)))current=s->id;
    for(i=0;i<WINDOWS;i++)if((s=slots+i)->id&&listed(s)&&s->role!=FDE_ROLE_PANEL){
        int reverse=(modifiers&MWKMOD_SHIFT)!=0;
        if(!first||(reverse?s->id>first->id:s->id<first->id))first=s;
        if((reverse?s->id<current:s->id>current)&&(!next||(reverse?s->id>next->id:s->id<next->id)))next=s;
    }
    if(next||first)action(next?next:first,FDE_ACTIVATE);
    return 1;
#else
    return 0;
#endif
}
int nxr_workspace_valid(void *packet,unsigned n) {
    struct nxr_workspace_req *q=packet;
    return n==sizeof(*q)&&q->version==NXR_WORKSPACE_VERSION&&q->op>=NXR_WS_INFO&&q->op<=NXR_WS_FRAME;
}
static unsigned role(int fd,struct nxr_workspace_req *q) {
    GR_WINDOW *w=find(q->id),*parent=find(q->other);struct slot *s,*ancestor;unsigned i;
    GR_WINDOW_ID transient;
    if(!w||w==rootwp||!w->output||q->value>FDE_ROLE_PANEL)return FDE_INVALID;
    if(!w->owner||w->owner->id!=fd)return FDE_DENIED;
    if(w->mapped)return FDE_BUSY;
    if(w->parent!=rootwp)return FDE_INVALID;
    if(q->other&&(!parent||parent==w||parent==rootwp||parent->owner!=w->owner))return FDE_INVALID;
    if(q->value!=FDE_ROLE_DIALOG&&q->other)return FDE_INVALID;
    for(i=0,transient=q->other;transient;i++){
        if(i>=32||transient==q->id)return FDE_INVALID;
        ancestor=lookup(transient);if(!ancestor||!listed(ancestor)||ancestor->role==FDE_ROLE_PANEL)return FDE_INVALID;
        transient=ancestor->transient;
    }
    if(q->value==FDE_ROLE_PANEL){
        if(q->edge<FDE_EDGE_TOP||q->edge>FDE_EDGE_RIGHT||!q->extent||q->extent>768||q->other)return FDE_INVALID;
#ifndef NXR_HOSTED
        unsigned left=0,right=0,top=0,bottom=0;
        for(i=0;i<WINDOWS;i++)if(slots[i].id&&slots[i].id!=q->id&&slots[i].role==FDE_ROLE_PANEL){
            if(slots[i].edge==q->edge)return FDE_BUSY;
            switch(slots[i].edge){case FDE_EDGE_TOP:top=slots[i].extent;break;case FDE_EDGE_BOTTOM:bottom=slots[i].extent;break;
            case FDE_EDGE_LEFT:left=slots[i].extent;break;case FDE_EDGE_RIGHT:right=slots[i].extent;break;}
        }
        switch(q->edge){case FDE_EDGE_TOP:top=q->extent;break;case FDE_EDGE_BOTTOM:bottom=q->extent;break;
        case FDE_EDGE_LEFT:left=q->extent;break;case FDE_EDGE_RIGHT:right=q->extent;break;}
        if((unsigned long)left+right+64>rootwp->width||(unsigned long)top+bottom+64>rootwp->height)return FDE_INVALID;
#endif
    }else if(q->edge||q->extent)return FDE_INVALID;
    s=record(w);if(!s)return FDE_BUSY;
    s->role=q->value;s->transient=q->other;s->edge=q->edge;s->extent=q->extent;
#ifndef NXR_HOSTED
    if(s->role==FDE_ROLE_PANEL||s->borderless_added){GR_WM_PROPERTIES props;
        memset(&props,0,sizeof(props));props.flags=GR_WM_FLAGS_PROPS|GR_WM_FLAGS_BORDERSIZE;
        props.props=w->props;
        if(s->role==FDE_ROLE_PANEL){
            if(!(w->props&GR_WM_PROPS_NODECORATE))s->borderless_added=1;
            props.props|=GR_WM_PROPS_NODECORATE;
        }else{props.props&=~GR_WM_PROPS_NODECORATE;s->borderless_added=0;}
        GrSetWMProperties(w->id,&props);}
#endif
    return FDE_OK;
}
void nxr_workspace_dispatch(int fd,void *packet) {
    struct nxr_workspace_req *q=packet;struct nxr_workspace_reply r;
    struct slot *s;GR_WINDOW *w,*p;unsigned i;short type=NXR_WORKSPACE;
    memset(&r,0,sizeof(r));curclient=GsFindClient(fd);if(!curclient)return;
    nxr_workspace_sync();r.status=FDE_INVALID;s=lookup(q->id);
    switch(q->op){
    case NXR_WS_INFO:r.status=FDE_OK;r.workspace=area;break;
    case NXR_WS_NEXT:
        s=NULL;for(i=0;i<WINDOWS;i++)if(slots[i].id>q->id&&listed(slots+i)&&(!s||slots[i].id<s->id))s=slots+i;
        if(s)info(s,&r.window);r.status=FDE_OK;break;
    case NXR_WS_GET:if(s&&listed(s)){info(s,&r.window);r.status=FDE_OK;}break;
    case NXR_WS_ROLE:r.status=role(fd,q);break;
    case NXR_WS_ACTION:if(s&&listed(s))r.status=action(s,q->value);break;
    case NXR_WS_WATCH:if(q->value<=1){watching[fd]=q->value;r.status=FDE_OK;}break;
    case NXR_WS_MANAGE:
#ifdef NXR_HOSTED
        r.status=FDE_UNSUPPORTED;
#else
        if(manager<0||manager==fd){manager=fd;r.status=FDE_OK;}else r.status=FDE_BUSY;
#endif
        break;
    case NXR_WS_FRAME:
        w=find(q->id);p=find(q->other);
        if(fd!=manager)r.status=FDE_DENIED;
        else if(w&&p&&w!=p&&p->owner==curclient&&w->owner!=curclient&&w->parent==p&&p->parent==rootwp){
            s=record(w);if(s){s->frame=p->id;r.status=FDE_OK;}}
        break;
    }
    nxr_workspace_sync();nxr_write(fd,&type,sizeof(type));nxr_write(fd,&r,sizeof(r));
}
