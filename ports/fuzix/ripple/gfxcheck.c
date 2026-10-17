/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test executable, installed only on the graphics integration test disk. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <gx.h>
#include "pty-limits.h"
extern long call(unsigned,long,long,long,long);
static int fd;
static struct gx_window w;
static void check(int ok,unsigned line){if(!ok){printf("gfxcheck FAILED line %u errno %d\n",line,errno);exit(1);}}
#define C(x) check((x),__LINE__)
static void create(void){memset(&w,0,sizeof(w));w.version=GX_VERSION;w.width=640;w.height=480;w.format=GX_RGB565;C(ioctl(fd,GX_CREATE,&w)==0);}
static void destroy(void){struct gx_handle h={GX_VERSION,0};h.handle=w.window;C(ioctl(fd,GX_DESTROY,&h)==0);}
int main(int argc,char **argv) {
    struct gx_info info;struct gx_fill f;struct gx_copy c;struct gx_upload u;struct gx_handle h;
    unsigned short stack[640],*pixels,*cross;unsigned long i;int pid,status,high,ptys[8],extra;
    char path[]="/dev/ptyp0";
    if(argc>1&&!strcmp(argv[1],"fd-child")){C(fcntl(31,F_GETFD)==-1&&errno==EBADF);return 0;}
    fd=open("/dev/gfx",O_RDWR);C(fd>=0);C(ioctl(fd,GX_INFO,&info)==0);C(info.version==1&&info.max_windows==1);
    high=fcntl(fd,F_DUPFD,31);C(high==31);C(fcntl(high,F_SETFD,FD_CLOEXEC)==0);
    extra=fcntl(high,F_DUPFD,20);C(extra==20);C(fcntl(extra,F_GETFD)==FD_CLOEXEC);close(extra);
    pid=fork();C(pid>=0);if(!pid){char *args[]={"gfxcheck","fd-child",NULL};execv("/bin/gfxcheck",args);_exit(100);}
    C(waitpid(pid,&status,0)==pid&&status==0);C(fcntl(high,F_GETFD)==FD_CLOEXEC);close(high);
    for(i=0;i<RIPPLE_PTY_COUNT;i++){path[9]='0'+i;ptys[i]=open(path,O_RDWR|O_NOCTTY);C(ptys[i]>=0);}
    extra=open(path,O_RDWR|O_NOCTTY);C(extra<0&&errno==EBUSY);
    for(i=0;i<RIPPLE_PTY_COUNT;i++)close(ptys[i]);
    create();h.version=GX_VERSION;h.handle=w.window;
    {
        struct gx_wait wait;int master,slave,pid;char byte;
        master=open("/dev/ptyp0",O_RDWR|O_NONBLOCK|O_NOCTTY);C(master>=0);
        memset(&wait,0,sizeof(wait));wait.version=GX_VERSION;wait.window=w.window;
        wait.master=master;wait.flags=GX_READY_READ;
        C(ioctl(fd,GX_WAIT,&wait)==0&&!wait.ready);
        wait.flags=GX_READY_WRITE;C(ioctl(fd,GX_WAIT,&wait)==0&&(wait.ready&GX_READY_WRITE));
        wait.master=31;C(ioctl(fd,GX_WAIT,&wait)<0&&errno==EBADF);wait.master=master;
        slave=open("/dev/ttyp0",O_RDWR|O_NOCTTY);C(slave>=0);
        pid=fork();C(pid>=0);
        if(!pid){call(37,1,0,0,0);_exit(write(slave,"r",1)==1?0:103);}
        wait.flags=GX_READY_READ;wait.timeout=20;
        C(ioctl(fd,GX_WAIT,&wait)==0&&(wait.ready&GX_READY_READ));
        C(read(master,&byte,1)==1&&byte=='r');
        C(waitpid(pid,&status,0)==pid&&status==0);
        close(slave);wait.timeout=0;
        C(ioctl(fd,GX_WAIT,&wait)==0&&(wait.ready&GX_READY_READ));close(master);
    }

    pid=fork();C(pid>=0);if(!pid){errno=0;_exit(ioctl(fd,GX_PRESENT,&h)==-1&&errno==EPERM?0:101);}
    C(waitpid(pid,&status,0)==pid&&status==0);
    memset(&f,0,sizeof(f));f.version=GX_VERSION;f.surface=w.surface;f.width=640;f.height=480;f.colour=0x001f;C(ioctl(fd,GX_FILL,&f)==0);
    pixels=malloc(70000UL*sizeof(*pixels));C(pixels!=NULL);
    /* Canonical heap pointer arithmetic crosses a virtual bank hole here. */
    cross=pixels+(64000UL-((unsigned long)pixels&65535UL))-100UL;
    for(i=0;i<640;i++){cross[i]=(unsigned short)(0x8000U+i);stack[i]=(unsigned short)(0x07e0U+i);}
    memset(&u,0,sizeof(u));u.version=GX_VERSION;u.surface=w.surface;u.width=u.stride=640;u.height=1;u.y=99;u.pixels=cross;C(ioctl(fd,GX_UPLOAD,&u)==0);
    u.y=100;u.pixels=stack;C(ioctl(fd,GX_UPLOAD,&u)==0);
    u.y=479;u.height=2;C(ioctl(fd,GX_UPLOAD,&u)<0&&errno==EINVAL);
    u.y=101;u.height=1;u.pixels=(void *)0x180000UL;C(ioctl(fd,GX_UPLOAD,&u)<0&&errno==EFAULT);
    memset(&c,0,sizeof(c));c.version=GX_VERSION;c.surface=w.surface;c.sy=99;c.dy=100;c.width=640;c.height=2;C(ioctl(fd,GX_COPY,&c)==0);
    c.sy=c.dy=101;c.sx=0;c.dx=3;c.width=637;c.height=1;C(ioctl(fd,GX_COPY,&c)==0);
    f.x=638;f.y=479;f.width=f.height=2;C(ioctl(fd,GX_FILL,&f)<0&&errno==EINVAL);
    f.x=620;f.y=470;f.width=20;f.height=10;f.colour=0xf800;C(ioctl(fd,GX_FILL,&f)==0);
    C(ioctl(fd,GX_PRESENT,&h)==0);
    if(argc>1&&!strcmp(argv[1],"frame")){puts("gfxcheck FRAME_READY");for(;;)pause();}
    destroy();C(ioctl(fd,GX_PRESENT,&h)<0);
    create();C(w.window!=h.handle);destroy();
    /* Owner process exit and successful exec release graphics without DESTROY. */
    pid=fork();C(pid>=0);if(!pid){create();_exit(0);}C(waitpid(pid,&status,0)==pid&&status==0);create();destroy();
    pid=fork();C(pid>=0);if(!pid){char *args[]={"true",NULL};create();execv("/bin/true",args);_exit(102);}
    C(waitpid(pid,&status,0)==pid&&status==0);create();destroy();
    free(pixels);close(fd);puts("gfxcheck: uploads, overlap, bounds, ownership, cleanup, high FDs and PTYs passed");return 0;
}
