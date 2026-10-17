/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Opt-in pixel probe: all 24 converted masks over a patterned background,
 * a clipped right edge, and a foreground child. Never installed normally. */
#include <stdio.h>
#define MWINCLUDECOLORS
#include "nano-X.h"
#define None 0
#define Pixmap GR_WINDOW_ID
#include "nxroach.h"
int main(void) {
    GR_WINDOW_ID window,child;
    GR_GC_ID gc;
    GR_EVENT e;
    GR_SCREEN_INFO s;
    int i,x,y;
    if(GrOpen()<0)return 1;
    window=GrNewWindow(GR_ROOT_WINDOW_ID,16,16,322,224,0,GR_RGB(32,64,96),0);
    child=GrNewWindow(window,52,52,60,60,0,GR_RGB(255,0,0),0);
    gc=GrNewGC();if(!window||!child||!gc)return 1;
    GrSelectEvents(window,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_KEY_DOWN|GR_EVENT_MASK_CLOSE_REQ);
    GrMapWindow(child);GrMapWindow(window);GrSetFocus(window);
    for(;;) {
        GrGetNextEvent(&e);
        if(e.type==GR_EVENT_TYPE_CLOSE_REQ||(e.type==GR_EVENT_TYPE_KEY_DOWN&&e.keystroke.ch=='q'))break;
        if(e.type==GR_EVENT_TYPE_EXPOSURE) {
            for(y=0;y<224;y+=8)for(x=0;x<322;x+=8) {
                GrSetGCForeground(gc,((x/8+y/8)&1)?GR_RGB(32,64,96):GR_RGB(64,96,128));
                GrFillRect(window,gc,x,y,8,8);
            }
            GrSetGCForeground(gc,GR_RGB(0,0,0));GrSetGCUseBackground(gc,GR_FALSE);
            for(i=0;i<24;i++)GrBitmap(window,gc,(i%6)*56,(i/6)*56,roachPix[i].width,roachPix[i].height,roachPix[i].roachBits);
            GrFlush();GrGetScreenInfo(&s);puts("NXROACH_ATLAS_READY");
        }
    }
    GrClose();return 0;
}
