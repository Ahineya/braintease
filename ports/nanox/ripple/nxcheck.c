/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include "nano-X.h"
#include "nxproto.h"
#include "nx_clipboard.h"
static int raw_peer(void){
    struct sockaddr_un a;int fd=socket(AF_UNIX,SOCK_STREAM,0);
    if(fd<0)return -1;memset(&a,0,sizeof(a));a.sun_family=AF_UNIX;strcpy(a.sun_path,GR_NAMED_SOCKET);
    if(connect(fd,(struct sockaddr *)&a,sizeof(a))<0){close(fd);return -1;}return fd;
}
static int rejected(void *packet,unsigned n){
    int fd=raw_peer(),got;char c;fd_set reads;struct timeval t;
    if(fd<0)return 0;
    if(((nxReq *)packet)->reqType!=GrNumOpen){
        nxOpenReq open;unsigned long ack;
        memset(&open,0,sizeof(open));open.reqType=GrNumOpen;open.length=sizeof(open);open.c16_magic=0x4e583102UL;
        if(write(fd,&open,sizeof(open))!=sizeof(open)||read(fd,&ack,sizeof(ack))!=sizeof(ack)||ack!=open.c16_magic){close(fd);return 0;}
    }
    if(write(fd,packet,n)!=n){close(fd);return 0;}
    FD_ZERO(&reads);FD_SET(fd,&reads);t.tv_sec=2;t.tv_usec=0;
    got=select(fd+1,&reads,NULL,NULL,&t);
    if(got>0)got=read(fd,&c,1)==0;else got=0;
    close(fd);return got;
}
/* A query must transmit pending requests without publishing half a repaint.
 * Another client forces presentation; the harness controls both commit/exit. */
static volatile int paint_signals;
static void paint_signal(int sig){++paint_signals;signal(sig,paint_signal);}
static int paint_check(int pulse){
    GR_WINDOW_ID w,child=0,pixmap=0;GR_GC_ID gc;GR_WM_PROPERTIES props;GR_WINDOW_INFO info;
    int pid,status;
    if(GrOpen()<0)return 1;
    signal(SIGUSR1,paint_signal);
    w=GrNewWindow(GR_ROOT_WINDOW_ID,pulse?300:20,pulse?300:20,pulse?12:640,pulse?12:480,0,0,0);
    if(!w)return 1;
    gc=GrNewGC();memset(&props,0,sizeof(props));props.flags=GR_WM_FLAGS_TITLE;
    props.title=pulse?"Paint pulse":"Paint check";GrSetWMProperties(w,&props);
    GrMapWindow(w);GrSetGCForeground(gc,GR_RGB(255,pulse?255:0,0));GrFillRect(w,gc,0,0,pulse?12:640,pulse?12:480);
    if(!pulse){
        /* Retain a third full frame (screen + window + offscreen) to exercise
         * high-bank allocation and disconnect cleanup, beyond a tiny dialog. */
        pixmap=GrNewPixmapEx(640,480,MWIF_RGB565,NULL);if(!pixmap)return 1;
        child=GrNewWindow(w,8,8,24,16,0,0,0);GrMapWindow(child);
        GrSetGCForeground(gc,GR_RGB(0,255,0));GrFillRect(child,gc,0,0,24,16);}
    GrFlush();GrGetWindowInfo(w,&info);
    if(pulse){puts("PAINTCHECK_PULSE");fflush(stdout);usleep(100000);GrClose();return 0;}
    puts("PAINTCHECK_READY");fflush(stdout);
    GrSetGCForeground(gc,GR_RGB(0,0,255));GrFillRect(w,gc,0,0,640,480);
    GrSetGCForeground(gc,GR_RGB(255,255,255));GrFillRect(child,gc,0,0,24,16);
    GrGetWindowInfo(w,&info);
    pid=fork();if(pid<0)return 1;
    if(!pid){char *args[]={"nxcheck","--pulse",NULL};execv("/bin/nxcheck",args);_exit(1);}
    if(waitpid(pid,&status,0)!=pid||status)return 1;
    puts("PAINTCHECK_WAITING");fflush(stdout);
    while(paint_signals<1)usleep(1000);
    GrFlush();GrGetWindowInfo(w,&info);puts("PAINTCHECK_COMMITTED");fflush(stdout);
    while(paint_signals<2)usleep(1000);
    GrClose();return 0;
}
static int clipboard_check(void) {
    GR_WINDOW_ID w;char *text,*snapshot,*saved;size_t length=0,saved_length=0;unsigned long i;
    if(GrOpen()<0)return 1;
    w=GrNewWindow(GR_ROOT_WINDOW_ID,0,0,8,16,0,0,0);if(!w)return 1;
    saved=nx_clipboard_get(w,&saved_length);
    text=malloc(NXR_CLIPBOARD_MAX+1);if(!text)return 1;
    for(i=0;i<NXR_CLIPBOARD_MAX;i++)text[i]='A'+i%26;
    memcpy(text+511,"Ж🙂",6);text[NXR_CLIPBOARD_MAX]=0;
    if(nx_clipboard_set(w,text,NXR_CLIPBOARD_MAX))return 1;
    snapshot=nx_clipboard_get(w,&length);
    if(!snapshot||length!=NXR_CLIPBOARD_MAX||memcmp(snapshot,text,length))return 1;
    free(snapshot);
    if(!nx_clipboard_set(w,text,NXR_CLIPBOARD_MAX+1)||!nx_clipboard_set(GR_ROOT_WINDOW_ID,"bad",3))return 1;
    snapshot=nx_clipboard_get(w,&length);
    if(!snapshot||length!=NXR_CLIPBOARD_MAX||memcmp(snapshot,text,length))return 1;
    free(snapshot);free(text);
    if(nx_clipboard_set(w,NULL,0))return 1;
    snapshot=nx_clipboard_get(w,&length);if(!snapshot||length||snapshot[0])return 1;free(snapshot);
    if(saved){if(nx_clipboard_set(w,saved,saved_length))return 1;free(saved);}
    GrDestroyWindow(w);GrClose();puts("NANOX_CLIPBOARD_OK");return 0;
}
int main(int argc,char **argv){
    int partial;nxReq bad;
    GR_WINDOW_ID w;GR_GC_ID g;GR_SCREEN_INFO s;GR_EVENT e;GR_WM_PROPERTIES p;GR_WINDOW_INFO wi;GR_TIMER_ID timer;
    if(argc==2&&!strcmp(argv[1],"--clipboard"))return clipboard_check();
    if(argc==2&&!strcmp(argv[1],"--paint"))return paint_check(0);
    if(argc==2&&!strcmp(argv[1],"--pulse"))return paint_check(1);
    if(argc==2&&!strcmp(argv[1],"--frame")){
        unsigned retry;GR_WINDOW_ID observed=0,parent=0;
        if(GrOpen()<0)return 1;
        for(retry=0;retry<40;retry++){
            w=GrGetFocus();
            observed=w;
            if(w&&w!=GR_ROOT_WINDOW_ID){
                GrGetWindowInfo(w,&wi);
                parent=wi.parent;
                if(wi.parent&&wi.parent!=GR_ROOT_WINDOW_ID){
                    GrGetWindowInfo(wi.parent,&wi);
                    if(wi.props&GR_WM_PROPS_BUFFERED){puts("NXCHECK_FRAME_BUFFERED");return 1;}
                    puts("NANOX_FRAME_DIRECT_OK");GrClose();return 0;
                }
            }
            /* Mapping and WM reparenting are asynchronous, especially while
             * a new terminal is painting its first screen of top output. */
            usleep(50000);
        }
        fprintf(stderr,"NXCHECK_FRAME_NOT_READY focus=%u parent=%u\n",observed,parent);GrClose();return 1;
    }
    if(argc==2&&!strcmp(argv[1],"--pressure")){
        GR_WINDOW_ID pixmaps[16];unsigned n=0,i;
        if(GrOpen()<0)return 1;
        while(n<16){pixmaps[n]=GrNewPixmapEx(640,480,MWIF_RGB565,NULL);if(!pixmaps[n])break;n++;}
        /* Allocation of the automatic backing store will also fail, but a
         * normal exposure-driven window must still get a valid window ID. */
        w=GrNewWindow(GR_ROOT_WINDOW_ID,0,0,640,480,0,0,0);
        if(!w){puts("NXCHECK_PRESSURE_WINDOW_FAILED");return 1;}
        GrGetWindowInfo(w,&wi);
        if(wi.props&GR_WM_PROPS_BUFFERED){puts("NXCHECK_PRESSURE_BUFFERED");return 1;}
        GrDestroyWindow(w);for(i=0;i<n;i++)GrDestroyWindow(pixmaps[i]);
        GrClose();puts("NANOX_PRESSURE_OK");return 0;
    }
    partial=raw_peer();bad.reqType=GrNumOpen;bad.hilength=0;bad.length=sizeof(nxOpenReq);
    if(partial<0||write(partial,&bad,1)!=1){puts("NXCHECK_PARTIAL_FAILED");return 1;}
    if(GrOpen()<0){puts("NXCHECK_OPEN_FAILED");return 1;}
    GrGetScreenInfo(&s);if(s.cols!=640||s.rows!=480){puts("NXCHECK_SCREEN_FAILED");return 1;}
    w=GrNewWindow(GR_ROOT_WINDOW_ID,100,100,240,120,0,GR_RGB(16,32,48),0);g=GrNewGC();
    memset(&p,0,sizeof(p));p.flags=GR_WM_FLAGS_TITLE;p.title="Nano-X client";GrSetWMProperties(w,&p);
    GrSelectEvents(w,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_TIMER|GR_EVENT_MASK_CLOSE_REQ|GR_EVENT_MASK_KEY_DOWN);
    GrMapWindow(w);GrSetFocus(w);GrGetWindowInfo(w,&wi);
    if(wi.width!=240||wi.height!=120){puts("NXCHECK_WINDOW_FAILED");return 1;}
    timer=GrCreateTimer(w,50);if(!timer){puts("NXCHECK_TIMER_FAILED");return 1;}
    for(;;){
        GrGetNextEventTimeout(&e,2000);
        if(e.type==GR_EVENT_TYPE_TIMEOUT){puts("NXCHECK_TIMEOUT");return 1;}
        if(e.type==GR_EVENT_TYPE_EXPOSURE){GrSetGCForeground(g,GR_RGB(255,200,80));GrText(w,g,16,24,"Independent Nano-X client",-1,GR_TFASCII|GR_TFTOP);GrFlush();}
        if(e.type==GR_EVENT_TYPE_TIMER){puts("NANOX_CLIENT_OK");break;}
    }
    GrDestroyTimer(timer);GrFlush();
    {GR_BITMAP bits[2]={0x8001,0x4000};GR_REGION_ID region=GrNewBitmapRegion(bits,16,2);GR_WINDOW_ID child;
     if(!region||!GrPointInRegion(region,0,0)||!GrPointInRegion(region,15,0)||!GrPointInRegion(region,1,1)||GrPointInRegion(region,2,1)){puts("NXCHECK_BITMAP_REGION_FAILED");return 1;}
     GrSetWindowRegion(w,region,GR_WINDOW_BOUNDING_MASK);GrDestroyRegion(region);
     child=GrNewWindow(w,0,0,1,1,0,GR_RGB(255,0,0),0);GrMapWindow(child);
     GrSetGCForeground(g,GR_RGB(0,255,0));GrFillRect(w,g,0,0,16,2);
     GrDestroyWindow(child);GrSetWindowRegion(w,0,GR_WINDOW_BOUNDING_MASK);GrFlush();}
    close(partial);
    bad.reqType=255;bad.hilength=0;bad.length=sizeof(bad);
    if(!rejected(&bad,sizeof(bad))){puts("NXCHECK_BAD_TYPE_FAILED");return 1;}
    bad.reqType=GrNumOpen;bad.length=5000;
    if(!rejected(&bad,sizeof(bad))){puts("NXCHECK_BAD_LENGTH_FAILED");return 1;}
    {nxOpenReq open;memset(&open,0,sizeof(open));open.reqType=GrNumOpen;open.length=sizeof(open);
     if(!rejected(&open,sizeof(open))){puts("NXCHECK_BAD_ABI_FAILED");return 1;}}
    {nxNewBitmapRegionReq bitmap;memset(&bitmap,0,sizeof(bitmap));bitmap.reqType=GrNumNewBitmapRegion;bitmap.length=sizeof(bitmap);bitmap.width=-1;bitmap.height=1;
     if(!rejected(&bitmap,sizeof(bitmap))){puts("NXCHECK_BITMAP_SIZE_FAILED");return 1;}
     bitmap.width=16;
     if(!rejected(&bitmap,sizeof(bitmap))){puts("NXCHECK_BITMAP_PAYLOAD_FAILED");return 1;}}
    {int slow=raw_peer();nxOpenReq open;nxGetScreenInfoReq burst[512];unsigned i;
     if(slow<0){puts("NXCHECK_SLOW_OPEN_FAILED");return 1;}
     memset(&open,0,sizeof(open));open.reqType=GrNumOpen;open.length=sizeof(open);open.c16_magic=0x4e583102UL;
     if(write(slow,&open,sizeof(open))!=sizeof(open)){puts("NXCHECK_SLOW_HELLO_FAILED");return 1;}
     fcntl(slow,F_SETFL,O_NONBLOCK);
     for(i=0;i<512;i++){burst[i].reqType=GrNumGetScreenInfo;burst[i].hilength=0;burst[i].length=sizeof(burst[i]);}
     write(slow,burst,sizeof(burst));
     GrGetScreenInfo(&s);close(slow);
     if(s.cols!=640){puts("NXCHECK_SLOW_READER_FAILED");return 1;}}
    GrGetScreenInfo(&s);if(s.cols!=640){puts("NXCHECK_RECOVERY_FAILED");return 1;}
    {GR_GC_ID ids[128],reused;unsigned n=0,i;
     while(n<128){ids[n]=GrNewGC();if(!ids[n])break;++n;}
     if(n!=127){puts("NXCHECK_QUOTA_FAILED");return 1;}
     for(i=0;i<n;i++)GrDestroyGC(ids[i]);
     reused=GrNewGC();if(!reused){puts("NXCHECK_QUOTA_RECOVERY_FAILED");return 1;}GrDestroyGC(reused);}
    GrDestroyWindow(w);GrDestroyGC(g);GrClose();return 0;
}
