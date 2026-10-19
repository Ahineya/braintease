/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Independent clients exercise private top-level surfaces and child widgets. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include "nano-X.h"
#include "clipboard-check.inc"
static unsigned resized;
static void changed(int sig){++resized;signal(sig,changed);}
static int sizes(const char *path){
    struct winsize size;unsigned cols=0,rows=0,count=65535;int fd;char text[64];
    signal(SIGWINCH,changed);
    for(;;){
        if(ioctl(0,TIOCGWINSZ,&size)<0)return 1;
        if(cols!=size.ws_col||rows!=size.ws_row||count!=resized){
            cols=size.ws_col;rows=size.ws_row;count=resized;
            fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return 1;
            write(fd,text,snprintf(text,sizeof(text),"PTY_SIZE %u %u %u\n",cols,rows,count));close(fd);
        }
        usleep(20000);
    }
}
int main(int argc,char **argv) {
    GR_WINDOW_ID w,child=0;GR_GC_ID gc;GR_EVENT e;GR_WINDOW_INFO info;GR_WM_PROPERTIES props;
    int a;
    if(argc==3&&!strcmp(argv[1],"file-check"))return file_check(argv[2]);
    if(argc==5&&!strcmp(argv[1],"paste-check"))return paste_check(argv[2],strtoul(argv[3],NULL,10),atoi(argv[4]));
    if(argc==3&&!strcmp(argv[1],"selection-check"))return selection_check(argv[2]);
    if(argc==3&&!strcmp(argv[1],"pty-size"))return sizes(argv[2]);
    if(argc<2||GrOpen()<0)return 1;
    if(!strcmp(argv[1],"clipboard"))return clipboard_check(argc,argv);
    if(!strcmp(argv[1],"find")) {
        GR_WINDOW_ID parent,*children;GR_COUNT count,i;
        if(argc!=3)return 2;
        GrQueryTree(GR_ROOT_WINDOW_ID,&parent,&children,&count);
        for(i=0;i<count;i++){
            memset(&props,0,sizeof(props));GrGetWMProperties(children[i],&props);
            if(props.title&&!strcmp(props.title,argv[2])){printf("ROOTLESS_FOUND %u\n",children[i]);free(props.title);free(children);GrClose();return 0;}
            free(props.title);
        }
        free(children);GrClose();return 1;
    }
    if(!strcmp(argv[1],"ctl")) {
        if(argc<4)return 2;w=strtoul(argv[3],NULL,10);
        if(!strcmp(argv[2],"resize")){if(argc!=6)return 2;GrResizeWindow(w,atoi(argv[4]),atoi(argv[5]));}
        else if(!strcmp(argv[2],"hide"))GrUnmapWindow(w);
        else if(!strcmp(argv[2],"show"))GrMapWindow(w);
        else if(!strcmp(argv[2],"close"))GrCloseWindow(w);
        else if(!strcmp(argv[2],"title")){if(argc!=5)return 2;memset(&props,0,sizeof(props));props.flags=GR_WM_FLAGS_TITLE;props.title=argv[4];GrSetWMProperties(w,&props);}
        else if(!strcmp(argv[2],"reparent")){if(argc!=5)return 2;GrReparentWindow(w,strtoul(argv[4],NULL,10),8,8);}
        else return 2;
        GrGetWindowInfo(w,&info);GrClose();puts("ROOTLESS_CONTROL_OK");return 0;
    }
    a=!strcmp(argv[1],"a");
    w=GrNewWindowEx(GR_WM_PROPS_APPWINDOW,a?"Rootless-A":"Rootless-B",GR_ROOT_WINDOW_ID,10,10,128,96,0);
    if(!w)return 1;
    gc=GrNewGC();GrSelectEvents(w,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_UPDATE|GR_EVENT_MASK_CLOSE_REQ|GR_EVENT_MASK_KEY_DOWN);
    if(a){child=GrNewWindow(w,8,8,20,16,0,GR_RGB(0,255,0),0);GrSelectEvents(child,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_BUTTON_DOWN);
        memset(&props,0,sizeof(props));props.flags=GR_WM_FLAGS_TITLE;props.title="Rootless-child";GrSetWMProperties(child,&props);GrMapWindow(child);}
    GrMapWindow(w);GrGetWindowInfo(w,&info);
    printf("ROOTLESS_%c_READY %u %u\n",a?'A':'B',w,child);fflush(stdout);
    for(;;) {
        GrGetNextEventTimeout(&e,50);
        if(e.type==GR_EVENT_TYPE_CLOSE_REQ&&e.general.wid==w){printf("ROOTLESS_%c_CLOSED\n",a?'A':'B');GrClose();return 0;}
        if(e.type==GR_EVENT_TYPE_KEY_DOWN){printf("ROOTLESS_%c_KEY %u\n",a?'A':'B',e.keystroke.ch);fflush(stdout);}
        if(e.type==GR_EVENT_TYPE_BUTTON_DOWN){printf("ROOTLESS_%c_BUTTON\n",a?'A':'B');fflush(stdout);}
        if(e.type==GR_EVENT_TYPE_EXPOSURE||e.type==GR_EVENT_TYPE_UPDATE){
            GrGetWindowInfo(w,&info);GrSetGCForeground(gc,a?GR_RGB(255,0,0):GR_RGB(0,0,255));GrFillRect(w,gc,0,0,info.width,info.height);
            if(child){GrGetWindowInfo(child,&info);GrSetGCForeground(gc,GR_RGB(0,255,0));GrFillRect(child,gc,0,0,info.width,info.height);}
            GrFlush();
        }
    }
}
