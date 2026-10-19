/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual adapted event producer/dequeue code; only device hooks are stubs. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "serv.h"
#include "osdep.h"
GR_CLIENT *curclient;
GR_EVENT_LIST *eventfree;
GR_WINDOW *rootwp,*mousewp,*grabbuttonwp;
GR_COORD cursorx,cursory;
char *curfunc;
static unsigned bad;
void nxr_bad(int fd){++bad;}
MWTIMEOUT GdGetTickCount(void){return 0;}
void GrMoveCursor(GR_COORD x,GR_COORD y){}
int GdError(const char *fmt,...){return 0;}
static void pulse(int direction) {
    GsDeliverButtonEvent(GR_EVENT_TYPE_BUTTON_DOWN,direction,direction,0);
    GsDeliverButtonEvent(GR_EVENT_TYPE_BUTTON_UP,0,direction,0);
}
static unsigned count(GR_CLIENT *c) {
    GR_EVENT_LIST *q;unsigned n=0;
    for(q=c->eventhead;q;q=q->next)++n;
    return n;
}
static GR_EVENT next(GR_CLIENT *c) {
    GR_EVENT e;curclient=c;GsCheckNextEvent(&e,GR_TRUE);return e;
}
static void travel(GR_CLIENT *c,unsigned n,int direction) {
    unsigned i;GR_EVENT e;
    for(i=0;i<n;i++) {
        e=next(c);assert(e.type==GR_EVENT_TYPE_BUTTON_DOWN);
        assert(e.button.changebuttons==direction);
    }
}
int main(void) {
    GR_CLIENT c={0},other={0};GR_WINDOW w={0};GR_EVENT_CLIENT interested={0};
    GR_EVENT e;unsigned i;
    interested.client=&c;
    interested.eventmask=GR_EVENT_MASK_BUTTON_DOWN|GR_EVENT_MASK_BUTTON_UP|GR_EVENT_MASK_MOUSE_POSITION;
    w.id=2;w.eventclients=&interested;rootwp=mousewp=&w;
    for(i=0;i<100;i++)pulse(GR_BUTTON_SCROLLDN);
    printf("trackpad burst: queued=%u disconnects=%u\n",count(&c),bad);
    assert(!bad && count(&c)==1 && !grabbuttonwp);
    curclient=&c;assert(GrPeekEvent(&e));assert(GrPeekEvent(&e));
    travel(&c,100,GR_BUTTON_SCROLLDN);assert(next(&c).type==GR_EVENT_TYPE_NONE);
    for(i=0;i<100;i++) {
        cursorx=i;GsDeliverMotionEvent(GR_EVENT_TYPE_MOUSE_POSITION,0,0);
        pulse(GR_BUTTON_SCROLLDN);
    }
    assert(count(&c)==2 && !bad);
    travel(&c,100,GR_BUTTON_SCROLLDN);e=next(&c);
    assert(e.type==GR_EVENT_TYPE_MOUSE_POSITION && e.mouse.x==99);
    for(i=0;i<10;i++)pulse(GR_BUTTON_SCROLLDN);
    GsDeliverButtonEvent(GR_EVENT_TYPE_BUTTON_DOWN,GR_BUTTON_L,GR_BUTTON_L,0);
    assert(grabbuttonwp==&w);
    GsDeliverButtonEvent(GR_EVENT_TYPE_BUTTON_UP,0,GR_BUTTON_L,0);
    assert(!grabbuttonwp);
    for(i=0;i<5;i++)pulse(GR_BUTTON_SCROLLDN);
    for(i=0;i<7;i++)pulse(GR_BUTTON_SCROLLUP);
    travel(&c,10,GR_BUTTON_SCROLLDN);
    e=next(&c);assert(e.type==GR_EVENT_TYPE_BUTTON_DOWN && e.button.changebuttons==GR_BUTTON_L);
    e=next(&c);assert(e.type==GR_EVENT_TYPE_BUTTON_UP && e.button.changebuttons==GR_BUTTON_L);
    travel(&c,5,GR_BUTTON_SCROLLDN);travel(&c,7,GR_BUTTON_SCROLLUP);
    /* A real button edge arriving with a wheel pulse still grabs/releases. */
    GsDeliverButtonEvent(GR_EVENT_TYPE_BUTTON_DOWN,GR_BUTTON_L|GR_BUTTON_SCROLLDN,GR_BUTTON_L|GR_BUTTON_SCROLLDN,0);
    assert(grabbuttonwp==&w);
    GsDeliverButtonEvent(GR_EVENT_TYPE_BUTTON_UP,0,GR_BUTTON_L|GR_BUTTON_SCROLLDN,0);
    assert(!grabbuttonwp);
    e=next(&c);assert(e.button.changebuttons==(GR_BUTTON_L|GR_BUTTON_SCROLLDN));
    e=next(&c);assert(e.type==GR_EVENT_TYPE_BUTTON_UP && e.button.changebuttons==GR_BUTTON_L);
    for(i=0;i<5000;i++)pulse(GR_BUTTON_SCROLLDN);
    interested.client=&other;w.id=3;
    for(i=0;i<11;i++)pulse(GR_BUTTON_SCROLLUP);
    assert(count(&c)==1 && count(&other)==1 && !bad);
    travel(&c,4096,GR_BUTTON_SCROLLDN);travel(&other,11,GR_BUTTON_SCROLLUP);
    assert(!c.eventhead && !c.eventtail && !other.eventhead && !other.eventtail);
    while(eventfree){GR_EVENT_LIST *q=eventfree;eventfree=q->next;free(q);}
    puts("Nano-X wheel counts, motion, direction, clicks, peek, reuse, saturation and client isolation passed");
    return 0;
}
