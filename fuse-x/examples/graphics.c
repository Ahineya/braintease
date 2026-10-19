/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native GX ownership/lifecycle probe; also works as a two-window visual demo. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <gx.h>
#include "../../ports/graphics/raster-check.inc"
#define C(x) do {if(!(x)){printf("gxcheck failed line %u errno %d\n",__LINE__,errno);return 1;}}while(0)
int main(int argc,char **argv) {
    int fd=open("/dev/gfx",O_RDWR),pid,status;unsigned i;
    struct gx_info info;struct gx_window w[2];struct gx_handle h;
    struct gx_title title;
    struct gx_fill fill;struct gx_copy copy;struct gx_upload upload;struct gx_event event;struct gx_state state;
    unsigned short pixels[4]={0xf800,0x07e0,0x001f,0xffff};
    C(fd>=0);C(sizeof(event)==21&&sizeof(state)==25);
    if(argc>2&&!strcmp(argv[1],"--after-exec")) {
        h.version=GX_VERSION;h.handle=strtoul(argv[2],NULL,10);
        C(ioctl(fd,GX_PRESENT,&h)<0&&errno==EINVAL);
        puts("gxcheck exec passed");return 0;
    }
    C(ioctl(fd,GX_INFO,&info)==0&&(info.flags&GX_CAP_MULTIWINDOW)&&(info.flags&GX_CAP_RASTER));
    C(raster_checks(fd)==0);
    memset(w,0,sizeof(w));
    for(i=0;i<2;i++) {
        w[i].version=GX_VERSION;w[i].width=320;w[i].height=240;w[i].format=GX_RGB565;
        strcpy(w[i].title,i?"Fuse-X second window":"Fuse-X first window");
        C(ioctl(fd,GX_CREATE,&w[i])==0);
        memset(&fill,0,sizeof(fill));fill.version=GX_VERSION;fill.surface=w[i].surface;fill.width=320;fill.height=240;fill.colour=i?0x016b:0x1904;
        C(ioctl(fd,GX_FILL,&fill)==0);
    }
    memset(&upload,0,sizeof(upload));upload.version=GX_VERSION;upload.surface=w[0].surface;upload.width=2;upload.height=2;upload.stride=2;upload.pixels=pixels;
    memset(&title,0,sizeof(title));title.version=GX_VERSION;title.window=w[1].window;strcpy(title.title,"Fuse-X updated title");
    C(ioctl(fd,GX_TITLE,&title)==0);
    C(ioctl(fd,GX_UPLOAD,&upload)==0);
    memset(&copy,0,sizeof(copy));copy.version=GX_VERSION;copy.surface=w[0].surface;copy.width=2;copy.height=2;copy.dx=1;copy.dy=1;
    C(ioctl(fd,GX_COPY,&copy)==0);
    copy.dx=320;C(ioctl(fd,GX_COPY,&copy)<0&&errno==EINVAL);
    for(i=0;i<2;i++){h.version=GX_VERSION;h.handle=w[i].window;C(ioctl(fd,GX_PRESENT,&h)==0);}
    pid=fork();C(pid>=0);
    if(!pid){h.version=GX_VERSION;h.handle=w[0].window;_exit(ioctl(fd,GX_DESTROY,&h)<0&&errno==EPERM?0:1);}
    C(waitpid(pid,&status,0)==pid&&status==0);
    h.version=GX_VERSION;h.handle=w[0].window;C(ioctl(fd,GX_PRESENT,&h)==0);
    if(argc>1&&!strcmp(argv[1],"--exec")) {
        char number[16];char *args[]={"gxcheck","--after-exec",number,NULL};
        snprintf(number,sizeof(number),"%lu",w[0].window);execv("/bin/gxcheck",args);C(0);
    }
    if(argc>1) {
        puts("Close both windows to finish; the kernel stays running.");
        while(w[0].window||w[1].window){
            fd_set fds;struct timeval t={1,0};FD_ZERO(&fds);FD_SET(fd,&fds);C(select(fd+1,&fds,NULL,NULL,&t)>=0);
            for(i=0;i<2;i++)if(w[i].window){memset(&event,0,sizeof(event));event.version=GX_VERSION;event.window=w[i].window;C(ioctl(fd,GX_EVENT,&event)==0);if(event.type==GX_CLOSE){h.handle=w[i].window;C(ioctl(fd,GX_DESTROY,&h)==0);w[i].window=0;}}
        }
    } else {
        for(i=0;i<2;i++){h.handle=w[i].window;C(ioctl(fd,GX_DESTROY,&h)==0);C(ioctl(fd,GX_PRESENT,&h)<0&&errno==EINVAL);}
        memset(&w[0],0,sizeof(w[0]));w[0].version=GX_VERSION;w[0].format=GX_RGB565;w[0].width=16;w[0].height=16;
        C(ioctl(fd,GX_CREATE,&w[0])==0);C(w[0].window!=h.handle);
        /* Deliberately leave this window alive: exit must release it. */
    }
    close(fd);puts("gxcheck passed");return 0;
}
