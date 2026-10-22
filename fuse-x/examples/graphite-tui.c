/* SPDX-License-Identifier: GPL-2.0-only */
/* TUI acceptance fixture: launcher -> gterm -> app -> helper.
 * Keeps an independent helper alive without requiring a terminal input driver
 * to implement shell script stdin semantics. All processes share the PTY group.
 * --white instead creates a real GX backdrop for panel blending/input checks. */
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <gx.h>
#include <gx-application.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
static int white_fill(int fd, struct gx_window *window) {
    struct gx_fill_rgb888 fill;
    struct gx_handle present;
    memset(&fill,0,sizeof(fill));fill.version=GX_VERSION;fill.surface=window->surface;
    fill.width=window->width;fill.height=window->height;fill.colour=0xffffffUL;
    if(ioctl(fd,GX_FILL_RGB888,&fill)<0)return -1;
    present.version=GX_VERSION;present.handle=window->window;
    return ioctl(fd,GX_PRESENT,&present);
}
static int white_backdrop(void) {
    struct gx_window window;
    struct gx_session_record app;
    struct gx_handle destroy;
    int fd=open("/dev/gfx",O_RDWR|O_CLOEXEC),result=1;
    if(fd<0)return 1;
    if(!gx_application_describe(&app,"org.riplix.PanelBackdrop","Panel backdrop","folder"))
        (void)gx_application_register(fd,&app);
    memset(&window,0,sizeof(window));window.version=GX_VERSION;
    window.width=1000;window.height=800;window.format=GX_XRGB8888;
    strcpy(window.title,"White backdrop");
    if(ioctl(fd,GX_CREATE,&window)<0){close(fd);return 1;}
    if(white_fill(fd,&window)<0)goto done;
    printf("WHITE_READY window=%lu\n",(unsigned long)window.window);fflush(stdout);
    for(;;) {
        struct gx_event event;
        memset(&event,0,sizeof(event));event.version=GX_VERSION;event.window=window.window;
        if(ioctl(fd,GX_EVENT,&event)<0)break;
        if(event.type==GX_CLOSE){result=0;break;}
        if(event.type==GX_RESIZE) {
            struct gx_configure accepted;
            memset(&accepted,0,sizeof(accepted));accepted.version=GX_VERSION;accepted.window=window.window;
            accepted.width=event.x;accepted.height=event.y;
            if(ioctl(fd,GX_CONFIGURE,&accepted)<0)break;
            window.width=accepted.width;window.height=accepted.height;
            if(white_fill(fd,&window)<0)break;
            printf("WHITE_RESIZE width=%u height=%u\n",window.width,window.height);fflush(stdout);
        } else if(event.type==GX_BUTTON) {
            printf("WHITE_BUTTON down=%u x=%ld y=%ld\n",(event.flags&GX_DOWN)!=0,event.x,event.y);fflush(stdout);
        } else if(!event.type) {
            fd_set reads;
            struct timeval delay;
            FD_ZERO(&reads);FD_SET(fd,&reads);delay.tv_sec=1;delay.tv_usec=0;
            if(select(fd+1,&reads,NULL,NULL,&delay)<0&&errno!=EINTR)break;
        }
    }
done:
    destroy.version=GX_VERSION;destroy.handle=window.window;
    (void)ioctl(fd,GX_DESTROY,&destroy);close(fd);return result;
}
int main(int argc,char **argv) {
    int channel[2],child,status;
    if(argc>1&&!strcmp(argv[1],"--white"))return white_backdrop();
    if(argc>2&&!strcmp(argv[1],"--record")) {
        FILE *file=fopen(argv[2],"w");int i;
        if(!file)return 1;
        for(i=0;i<argc;i++)fprintf(file,"ARG%d=<%s>\n",i,argv[i]);
        if(fclose(file))return 1;
    }
    if(pipe(channel))return 1;
    child=fork();
    if(child<0)return 1;
    if(!child) {
        char byte;
        close(channel[1]);
        puts("COMMANDER_FIXTURE_READY");fflush(stdout);
        for(;;) {
            int count=read(channel[0],&byte,1);
            if(count<0&&errno==EINTR)continue;
            if(count<=0)_exit(0);
        }
    }
    close(channel[0]);
    while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 1;
    return 0;
}
