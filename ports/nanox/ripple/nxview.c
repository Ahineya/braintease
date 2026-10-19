/* SPDX-License-Identifier: MIT */
/* Independent local image viewer; shared codecs and retained Nano-X images. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "nano-X.h"
#include "../../../libraries/image/nano.h"
#include "nxview-model.h"

#define BAR 28
#define STATUS 24
#define PATH_SIZE 256
static const RiLimits limits={4096,4096,262144UL,1048576UL,2097152UL};
static const char *buttons[]={"Open","Prev","Next","Fit","1:1","-","+"};
static GR_WINDOW_ID window;
static GR_GC_ID gc;
static RiImage *image;
static struct image_view view;
static int width,height,dirty=1,stopping,connected,trace,dragging,drag_x,drag_y,drag_px,drag_py;
static int picker=-1,picker_pid,picker_bad;
static unsigned picker_count;
static char picker_path[PATH_SIZE],current[PATH_SIZE],message[160];
static char **paths;
static int path_count,path_index=-1;

static void report(const char *text)
{
    snprintf(message,sizeof(message),"%s",text);dirty=1;
    fprintf(stderr,"nxview: %s\n",text);
}
static void title(void)
{
    GR_WM_PROPERTIES p;char text[PATH_SIZE+24];const char *name=strrchr(current,'/');
    snprintf(text,sizeof(text),"Image Viewer%s%s",current[0]?": ":"",name?name+1:current);
    memset(&p,0,sizeof(p));p.flags=GR_WM_FLAGS_TITLE;p.title=text;GrSetWMProperties(window,&p);
}
static void rectangle(int x,int y,int w,int h,GR_COLOR colour)
{
    if(w<1||h<1)return;
    GrSetGCForeground(gc,colour);GrFillRect(window,gc,x,y,w,h);
}
static void label(int x,int y,const char *text)
{
    GrSetGCForeground(gc,GR_RGB(225,231,238));
    GrText(window,gc,x,y,text,-1,GR_TFASCII|GR_TFTOP);
}
static void layout(void)
{
    view.vw=width;view.vh=height-BAR-STATUS;view_layout(&view);dirty=1;
}
static void paint(void)
{
    int i,x,y,w,result;GR_RECT rect;GR_REGION_ID region;
    struct RiClip clip;char text[PATH_SIZE+80];
    rectangle(0,0,width,height,GR_RGB(26,30,36));
    rectangle(0,0,width,BAR,GR_RGB(44,51,61));
    for(i=0;i<7;i++){
        x=(long)i*width/7;w=(long)(i+1)*width/7-x;
        rectangle(x+2,3,w-4,BAR-6,view.fit&&i==3?GR_RGB(65,101,136):GR_RGB(58,67,79));
        label(x+4,6,buttons[i]);
    }
    if(image&&height>BAR+STATUS){
        clip.x0=0;clip.y0=BAR;clip.x1=width;clip.y1=height-STATUS;
        rect.x=0;rect.y=BAR;rect.width=width;rect.height=clip.y1-BAR;
        region=GrNewRegion();
        if(region){
            GrUnionRectWithRegion(region,&rect);GrSetGCRegion(gc,region);GrDestroyRegion(region);
            x=view.dw>view.vw?-view.px:(view.vw-view.dw)/2;
            y=BAR+(view.dh>view.vh?-view.py:(view.vh-view.dh)/2);
            /* Only paint checkerboard behind visible image pixels. */
            if(!image->opaque){
                int left=x>0?x:0,top=y>BAR?y:BAR,right=x+view.dw,bottom=y+view.dh;
                if(right>width)right=width;if(bottom>clip.y1)bottom=clip.y1;
                rectangle(left,top,right-left,bottom-top,GR_RGB(112,120,128));
                for(i=top;i<bottom;i+=16)for(w=left;w<right;w+=16)if(((i-top)/16+(w-left)/16)&1)
                    rectangle(w,i,right-w<16?right-w:16,bottom-i<16?bottom-i:16,GR_RGB(150,158,166));
            }
            result=ri_nano_draw_cached(image,window,gc,x,y,view.dw,view.dh,&clip,0);
            GrSetGCRegion(gc,0);
            if(result)snprintf(message,sizeof(message),"Drawing failed: %s",ri_image_error(result));
        }else snprintf(message,sizeof(message),"Not enough drawing resources");
    }else if(!image&&height>BAR+STATUS+32){
        label(12,BAR+12,"Open a PNG, JPEG, GIF or BMP image.");
        label(12,BAR+32,"O: open   F: fit   1: actual size");
        label(12,BAR+52,"+/-: zoom   Drag/arrows/wheel: pan");
    }
    rectangle(0,height-STATUS,width,STATUS,GR_RGB(44,51,61));
    if(message[0])snprintf(text,sizeof(text),"%s",message);
    else if(image)snprintf(text,sizeof(text),"%u x %u  %ld%% %s  %s",image->width,image->height,
        (long)view.dw*100/image->width,view.fit?"Fit":"",current);
    else snprintf(text,sizeof(text),"Open: O / Ctrl+O   Close: Q / Esc");
    label(6,height-STATUS+4,text);GrFlush();dirty=0;
    if(trace)fprintf(stderr,"NXVIEW_FRAME %d %d %d %d %d %d\n",width,height,view.dw,view.dh,view.px,view.py);
}
static int load_image(const char *path)
{
    struct stat st;int fd,n,result;size_t at=0,size;RiByte *data;RiImage *replacement=NULL;
    if(strlen(path)>=sizeof(current)){report("Path is too long");return -1;}
    /* Opening a FIFO must not block before the regular-file check. */
    fd=open(path,O_RDONLY|O_NONBLOCK);
    if(fd<0){report(strerror(errno));return -1;}
    if(fstat(fd,&st)<0){report(strerror(errno));close(fd);return -1;}
    if(!S_ISREG(st.st_mode)){close(fd);report("Please select a regular image file");return -1;}
    if(st.st_size<2||(unsigned long)st.st_size>limits.input){close(fd);report("Image file must be between 2 octets and 1 MiB");return -1;}
    size=st.st_size;data=malloc(size*sizeof(*data));
    if(!data){close(fd);report("Not enough memory to read image");return -1;}
    strcpy(message,"Loading image...");paint();
    while(at<size){
        n=read(fd,data+at,size-at>512?512:size-at);
        if(n>0)at+=n;
        else if(n<0&&errno==EINTR)continue;
        else break;
    }
    close(fd);
    result=at==size?ri_image_decode(data,size,&limits,&replacement):RI_CORRUPT;
    free(data);
    if(result){report(ri_image_error(result));return -1;}
    ri_nano_forget(image);ri_image_free(image);image=replacement;
    strcpy(current,path);message[0]=0;dragging=0;
    view.iw=image->width;view.ih=image->height;view.fit=1;layout();title();
    if(trace)fprintf(stderr,"NXVIEW_LOADED %s %u %u\n",path,image->width,image->height);
    return 0;
}
static void picker_close(void)
{
    if(picker>=0){GrUnregisterInput(picker);close(picker);picker=-1;}
}
static void picker_reap(void)
{
    if(picker_pid>0&&waitpid(picker_pid,NULL,WNOHANG)==picker_pid)picker_pid=0;
}
static void open_picker(void)
{
    int fds[2],pid,i;char directory[PATH_SIZE],*slash;
    picker_reap();if(picker>=0||picker_pid>0){GrBell();return;}
    if(pipe(fds)<0){report(strerror(errno));return;}
    if(fcntl(fds[0],F_SETFL,O_NONBLOCK)<0){close(fds[0]);close(fds[1]);report("Cannot open file selector pipe");return;}
    strcpy(directory,current);slash=strrchr(directory,'/');
    if(slash){if(slash==directory)slash[1]=0;else *slash=0;}else directory[0]=0;
    pid=fork();
    if(!pid){
        char *args[]={"/bin/nxselect",NULL};
        if(dup2(fds[1],1)<0)_exit(126);
        for(i=3;i<32;i++)close(i);
        if(directory[0]&&chdir(directory)<0){perror("nxview: image directory");_exit(126);}
        execv(args[0],args);perror("nxview: nxselect");_exit(127);
    }
    close(fds[1]);
    if(pid<0){close(fds[0]);report("Cannot launch file selector");return;}
    picker_pid=pid;picker=fds[0];picker_count=picker_bad=0;GrRegisterInput(picker);
    if(trace)fprintf(stderr,"NXVIEW_PICKER\n");
}
static void picker_input(void)
{
    char buf[64];int i,n,complete=0;
    while((n=read(picker,buf,sizeof(buf)))>0){
        for(i=0;i<n;i++){
            if(buf[i]=='\n'){complete=1;break;}
            if(buf[i]!='\r'){
                if(picker_count+1<sizeof(picker_path))picker_path[picker_count++]=buf[i];else picker_bad=1;
            }
        }
        if(complete)break;
    }
    if(!complete&&n<0&&(errno==EAGAIN||errno==EINTR))return;
    picker_close();picker_path[picker_count]=0;
    if(complete&&!picker_bad&&!strcmp(picker_path,"[]")){}
    else if(complete&&!picker_bad&&picker_path[0]=='/'){
        if(!load_image(picker_path))path_index=-1;
    }else report("File selector did not return a valid path");
    picker_reap();GrSetFocus(window);
    if(trace)fprintf(stderr,"NXVIEW_PICKER_DONE\n");
}
static void action(int which)
{
    int next;
    if(which==0){open_picker();return;}
    if(which==1||which==2){
        next=path_index+(which==1?-1:1);
        if(path_index<0)next=which==1?path_count-1:0;
        if(next>=0&&next<path_count){path_index=next;load_image(paths[next]);}
        else GrBell();
        return;
    }
    if(!image)return;
    dragging=0;
    if(which==3){view.fit=1;view_layout(&view);}
    if(which==4){view.fit=0;view.zoom=100;view.px=view.py=0;view_layout(&view);}
    if(which==5||which==6)view_zoom(&view,which==6?1:-1);
    dirty=1;
}
static void event(GR_EVENT *e)
{
    unsigned k;
    if(e->type==GR_EVENT_TYPE_EXPOSURE)dirty=1;
    else if(e->type==GR_EVENT_TYPE_CLOSE_REQ)stopping=1;
    else if(e->type==GR_EVENT_TYPE_FDINPUT&&e->fdinput.fd==picker)picker_input();
    else if(e->type==GR_EVENT_TYPE_UPDATE&&e->update.utype==GR_UPDATE_SIZE){
        width=e->update.width;height=e->update.height;dragging=0;layout();
    }else if(e->type==GR_EVENT_TYPE_KEY_DOWN){
        k=e->keystroke.ch;
        if(k=='q'||k=='Q'||k==MWKEY_ESCAPE)stopping=1;
        else if(k=='o'||k=='O'||k==15)action(0);
        else if(k=='['||k==MWKEY_PAGEUP)action(1);
        else if(k==']'||k==MWKEY_PAGEDOWN)action(2);
        else if(k=='f'||k=='F'||k=='0')action(3);
        else if(k=='1')action(4);
        else if(k=='-'||k=='_')action(5);
        else if(k=='+'||k=='=')action(6);
        else{
            if(k==MWKEY_LEFT)view.px-=32;if(k==MWKEY_RIGHT)view.px+=32;
            if(k==MWKEY_UP)view.py-=32;if(k==MWKEY_DOWN)view.py+=32;
            view_clamp(&view);dirty=1;
        }
    }else if(e->type==GR_EVENT_TYPE_BUTTON_DOWN){
        if(e->button.changebuttons&(GR_BUTTON_SCROLLUP|GR_BUTTON_SCROLLDN)){
            int direction=e->button.changebuttons&GR_BUTTON_SCROLLUP?-1:1;
            view.py+=direction*48;view_clamp(&view);dirty=1;
        }else if(e->button.changebuttons&GR_BUTTON_L){
            if(e->button.y<BAR){if(width>0)action((long)e->button.x*7/width);}
            else if(e->button.y<height-STATUS&&image){dragging=1;drag_x=e->button.x;drag_y=e->button.y;drag_px=view.px;drag_py=view.py;}
        }
    }else if(e->type==GR_EVENT_TYPE_BUTTON_UP)dragging=0;
    else if(e->type==GR_EVENT_TYPE_MOUSE_POSITION&&dragging){
        view.px=view_bound((long)drag_px+drag_x-e->mouse.x,view.dw>view.vw?view.dw-view.vw:0);
        view.py=view_bound((long)drag_py+drag_y-e->mouse.y,view.dh>view.vh?view.dh-view.vh:0);dirty=1;
    }
}
static void cleanup(void)
{
    picker_close();picker_reap();
    if(picker_pid>0){kill(picker_pid,SIGTERM);while(waitpid(picker_pid,NULL,0)<0&&errno==EINTR){}}
    if(connected){ri_nano_clear();ri_image_free(image);image=NULL;GrClose();connected=0;}
    if(trace)fprintf(stderr,"NXVIEW_EXIT\n");
}
int main(int argc,char **argv)
{
    GR_SCREEN_INFO screen;GR_EVENT e;unsigned n;
    if(argc>1&&!strcmp(argv[1],"--help")){
        puts("usage: nxview [FILE ...]\nPNG, baseline JPEG, first-frame GIF, BMP.\nO/Ctrl+O: open; F/0: fit; 1: actual size; +/-: zoom.\nDrag/arrows/wheel: pan; [ / ] or PgUp/PgDn: previous/next argument.\nQ/Esc: close. Limits: 1 MiB file, 262144 pixels, 4096 per dimension.");return 0;
    }
    trace=getenv("NANOX_TRACE")!=NULL;paths=argv+1;path_count=argc-1;
    if(GrOpen()<0){fputs("nxview: cannot connect to Nano-X\n",stderr);return 1;}
    connected=1;atexit(cleanup);GrGetScreenInfo(&screen);
    width=screen.cols>680?640:screen.cols-48;height=screen.rows>540?480:screen.rows-80;
    if(width<240)width=240;if(height<160)height=160;
    window=GrNewWindowEx(GR_WM_PROPS_APPWINDOW,"Image Viewer",GR_ROOT_WINDOW_ID,24,32,width,height,GR_RGB(26,30,36));
    gc=GrNewGC();if(!window||!gc){report("Cannot create viewer window");return 1;}
    GrSetGCUseBackground(gc,GR_FALSE);GrSelectEvents(window,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_CLOSE_REQ|
        GR_EVENT_MASK_KEY_DOWN|GR_EVENT_MASK_UPDATE|GR_EVENT_MASK_BUTTON_DOWN|GR_EVENT_MASK_BUTTON_UP|GR_EVENT_MASK_MOUSE_POSITION);
    layout();GrMapWindow(window);GrSetFocus(window);
    if(trace)fprintf(stderr,"NXVIEW_READY %u\n",window);
    if(path_count){path_index=0;load_image(paths[0]);}
    else open_picker();
    while(!stopping){
        if(dirty)paint();
        GrGetNextEvent(&e);event(&e);
        /* Drain a bounded burst before repainting wheel/drag/exposure damage. */
        for(n=0;n<16&&!stopping&&GrPeekEvent(&e);n++){GrGetNextEvent(&e);event(&e);}
        picker_reap();
    }
    return 0;
}
