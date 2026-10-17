/* SPDX-License-Identifier: GPL-2.0-or-later */
/* First Nano-X engine application, using the actual upstream Gd APIs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/select.h>
#include "screen.h"

static PSD display;
static PMWFONT font;
static volatile int stopping;
static void cleanup(void){if(font){GdDestroyFont(font);font=NULL;}if(display){GdCloseScreen(display);display=NULL;}}
static void stop(int sig){stopping=1;}
static void failed(unsigned line){fprintf(stderr,"nxdraw FAILED line %u\n",line);exit(1);}
#define C(test) do{if(!(test))failed(__LINE__);}while(0)
static void colour(unsigned value){GdSetForegroundPixelVal(display,value);}
static void box(int x,int y,int w,int h,unsigned value){colour(value);GdFillRect(display,x,y,w,h);}
static void clip(int x,int y,int right,int bottom){MWCLIPREGION *r=GdAllocRectRegion(x,y,right,bottom);C(r!=NULL);GdSetClipRegion(display,r);}
static void reset_clip(void){clip(0,0,640,480);}
static void text(int x,int y,const char *s,unsigned value){colour(value);GdSetUseBackground(FALSE);GdText(display,font,x,y,s,-1,MWTF_ASCII|MWTF_TOP);}
static void checks(void) {
    int x,y,w,h,baseline,count;uint32_t mask;MWRECT hole={60,60,80,80};MWCLIPREGION *r;
    unsigned short pattern[16*16];SCREENDEVICE source;
    C(sizeof(char)==1&&sizeof(int)==1&&sizeof(uint32_t)==2&&MWIMAGE_BITSPERIMAGE==16);
    C(MIN_MWCOORD==-32768&&MAX_MWCOORD==32767&&display->size==307200UL);
    box(0,0,640,480,0x1234);box(-10,-10,20,20,0xf800);
    C(display->ReadPixel(display,0,0)==0xf800&&display->ReadPixel(display,9,9)==0xf800);
    C(display->ReadPixel(display,10,10)==0x1234);
    GdSetClipRegion(display,GdAllocRegion());box(0,0,640,480,0xffff);colour(0xffff);GdPoint(display,300,300);
    C(display->ReadPixel(display,300,300)==0x1234);
    r=GdAllocRectRegion(40,40,100,100);C(r!=NULL);GdSubtractRectFromRegion(&hole,r);GdSetClipRegion(display,r);
    box(0,0,640,480,0x07e0);
    for(y=39;y<101;y++)for(x=39;x<101;x++)
        C(display->ReadPixel(display,x,y)==((x>=40&&x<100&&y>=40&&y<100&&!(x>=60&&x<80&&y>=60&&y<80))?0x07e0:0x1234));
    reset_clip();GdGetTextSize(font,"Ripple",6,&w,&h,&baseline,MWTF_ASCII);C(w==48&&h==16&&baseline==12);
    mask=0x80000000UL;count=32;GdSetDash(&mask,&count);colour(0xffff);GdLine(display,0,120,63,120,TRUE);
    C(display->ReadPixel(display,31,120)==0xffff&&display->ReadPixel(display,63,120)==0xffff);
    C(display->ReadPixel(display,30,120)==0x1234);mask=0;count=0;GdSetDash(&mask,&count);C(mask==0x80000000UL);
    box(300,280,40,20,0x2222);colour(0xf00f);GdSetMode(MWROP_XOR);
    GdFillRect(display,300,280,4,4);C(display->ReadPixel(display,301,281)==(0x2222^0xf00f));
    GdFillRect(display,300,280,4,4);C(display->ReadPixel(display,301,281)==0x2222);GdSetMode(MWROP_COPY);
    {
        static const MWIMAGEBITS bits[]={0xaaaa,0x8000,0x5555,0,0xffff,0x8000,0x8000,0x8000};
        clip(303,281,317,284);colour(0xffff);GdSetBackgroundPixelVal(display,0);GdSetUseBackground(TRUE);
        GdBitmap(display,300,280,17,4,bits);
        for(y=280;y<285;y++)for(x=300;x<319;x++) {
            unsigned wanted=0x2222;
            if(x>=303&&x<317&&y>=281&&y<284)wanted=(bits[(y-280)*2+(x-300)/16]&(0x8000U>>((x-300)%16)))?0xffff:0;
            C(display->ReadPixel(display,x,y)==wanted);
        }
        reset_clip();
    }
    source=*display;source.flags=PSF_MEMORY;source.addr=(unsigned char *)pattern;
    source.xres=source.xvirtres=source.yres=source.yvirtres=16;source.pitch=16;source.size=256;
    for(y=0;y<16;y++)for(x=0;x<16;x++)pattern[y*16+x]=0x8000+y*16+x;
    GdBlit(display,200,200,16,16,&source,0,0,MWROP_COPY);
    GdBlit(display,203,203,13,13,display,200,200,MWROP_COPY);
    for(y=0;y<13;y++)for(x=0;x<13;x++)C(display->ReadPixel(display,203+x,203+y)==pattern[y*16+x]);
    /* Multi-rectangle self-overlap must read the original source for every band. */
    reset_clip();GdBlit(display,200,200,16,16,&source,0,0,MWROP_COPY);
    r=GdAllocRectRegion(203,203,216,216);C(r!=NULL);hole.left=207;hole.right=211;hole.top=205;hole.bottom=213;
    GdSubtractRectFromRegion(&hole,r);GdSetClipRegion(display,r);
    GdBlit(display,203,203,13,13,display,200,200,MWROP_COPY);
    for(y=203;y<216;y++)for(x=203;x<216;x++)
        C(display->ReadPixel(display,x,y)==((x>=207&&x<211&&y>=205&&y<213)?pattern[(y-200)*16+x-200]:pattern[(y-203)*16+x-203]));
    reset_clip();puts("NANOX_ENGINE_OK");
}
static void scene(void) {
    int i;MWCLIPREGION *r;MWRECT hole={384,169,456,225};
    static const MWIMAGEBITS bitmap[]={0xaaaa,0x8000,0x5555,0,0xffff,0x8000,0x8000,0x8000};
    box(0,0,640,480,0x1082);box(0,0,640,64,0x18e3);
    text(24,16,"Nano-X on Ripple",0xffff);text(24,40,"Drawing engine / RGB565",0x7e7f);
    box(24,88,280,184,0x2104);text(40,104,"LINES + COLOUR",0xffff);
    for(i=0;i<8;i++) {
        static const unsigned colours[]={0xf800,0xfd20,0xffe0,0x07e0,0x07ff,0x001f,0x801f,0xffff};
        box(40+i*30,136,24,24,colours[i]);
    }
    for(i=0;i<7;i++){colour(0x0410+i*0x1000);GdLine(display,40,184+i*10,264,184+i*10,TRUE);}
    box(328,88,288,184,0x2104);text(344,104,"REGIONS + CLIPPING",0xffff);
    r=GdAllocRectRegion(344,136,600,248);C(r!=NULL);GdSubtractRectFromRegion(&hole,r);GdSetClipRegion(display,r);
    for(i=0;i<12;i++)box(328,128+i*12,288,8,(i&1)?0x05d7:0x03ef);
    text(332,170,"THIS TEXT IS CLIPPED",0xffe0);reset_clip();
    box(24,292,592,140,0x2104);text(40,308,"BITMAPS + OVERLAPPING BLITS",0xffff);
    colour(0xffe0);GdSetBackgroundPixelVal(display,0x001f);GdSetUseBackground(TRUE);
    GdBitmap(display,40,344,17,4,bitmap);
    for(i=0;i<16;i++)box(80+i*8,344,8,48,0x2000+i*0x0801);
    GdBlit(display,96,352,112,40,display,80,344,MWROP_COPY);
    text(240,344,"8x16 compiled font",0x7e7f);text(240,372,"One pixel = one C16 cell",0xffff);
    text(24,452,"Esc / Q to close",0x9cf3);
}
int main(int argc,char **argv) {
    int check=argc>1&&!strcmp(argv[1],"--check"),frame=argc>1&&!strcmp(argv[1],"--frame"),i;
    struct gx_event event;fd_set reads;
    if(argc>1&&!check&&!frame){puts("usage: nxdraw [--check|--frame]\nNano-X drawing demo. Escape or Q closes the display.");return strcmp(argv[1],"--help")?1:0;}
    atexit(cleanup);display=GdOpenScreen();if(!display){perror("nxdraw");return 1;}
    font=GdCreateFont(display,MWFONT_SYSTEM_FIXED,0,0,NULL);C(font!=NULL);
    signal(SIGINT,stop);signal(SIGTERM,stop);signal(SIGHUP,stop);
    if(check)checks();scene();C(nxr_present()==0);
    if(check)return 0;
    puts("Nano-X drawing demo ready.");fflush(stdout);
    while(!stopping) {
        for(i=0;i<32;i++) {
            C(nxr_event(&event)==0);if(!event.type)break;
            if(event.type==GX_CLOSE||(event.type==GX_KEY&&(event.flags&GX_DOWN)&&(event.code==41||event.code==20)))stopping=1;
        }
        if(stopping)break;
        FD_ZERO(&reads);FD_SET(nxr_descriptor(),&reads);
        if(select(nxr_descriptor()+1,&reads,NULL,NULL,NULL)<0&&!stopping){perror("nxdraw select");return 1;}
    }
    return 0;
}
