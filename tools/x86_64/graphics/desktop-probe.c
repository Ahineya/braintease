/* Native desktop diagnostics through the ordinary public session API. */
#include <gx-session.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char **argv){
    int fd=open("/dev/gfx",O_RDWR),waiting=argc==3&&!strcmp(argv[1],"--wait");
    if(fd<0){perror("desktop-probe");return 1;}
    if(argc==2&&!strcmp(argv[1],"--idle")){
        struct gx_session q;unsigned long before;
        memset(&q,0,sizeof(q));q.version=GXS_VERSION;q.operation=GXS_INFO;
        if(ioctl(fd,GX_SESSION,&q)<0){perror("GX_SESSION");close(fd);return 1;}before=q.revision;
        sleep(2);memset(&q,0,sizeof(q));q.version=GXS_VERSION;q.operation=GXS_INFO;
        if(ioctl(fd,GX_SESSION,&q)<0||q.revision!=before){fputs("desktop-probe: idle session revision changed\n",stderr);close(fd);return 1;}
        printf("GX_IDLE_OK revision=%lu\n",q.revision);close(fd);return 0;
    }
    if(argc!=1&&!waiting){puts("Usage: desktop-probe [--wait TITLE | --idle]");close(fd);return 2;}
    for(unsigned attempt=0;attempt<(waiting?120:1);attempt++){
        struct gx_session q;struct gx_session_record record;
        memset(&q,0,sizeof(q));q.version=GXS_VERSION;q.operation=GXS_NEXT_WINDOW;q.record=&record;
        for(unsigned i=0;i<32;i++){
            if(ioctl(fd,GX_SESSION,&q)<0){if(waiting)break;perror("GX_SESSION");close(fd);return 1;}
            if(!q.object)break;
            if(waiting){if(!strcmp(record.title,argv[2])&&(record.flags&GXS_VISIBLE)&&!(record.flags&GXS_MINIMIZED)){
                printf("GX_PUBLISHED %s\n",record.title);close(fd);return 0;}}
            else printf("window=%lu pid=%u flags=%u role=%u %ld,%ld %ux%u %s\n",q.object,record.pid,record.flags,record.role,record.x,record.y,record.width,record.height,record.title);
        }
        if(waiting)sleep(1);
    }
    close(fd);return waiting?1:0;
}
