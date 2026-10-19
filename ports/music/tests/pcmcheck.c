#include <sys/pcm.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
int main(void) {
    int fd,child,other,i,status;struct pcm_format format;struct pcm_state state;
    short *allocation,*samples;unsigned long boundary;
    struct timeval wait={0,20000};fd_set ready;
    fd=open("/dev/pcm",O_WRONLY|O_NONBLOCK);if(fd<0)return 1;
    other=open("/dev/pcm",O_WRONLY);if(other>=0||errno!=EBUSY)return 2;
    format.version=1;format.channels=3;format.rate=8000UL;format.capacity=1024UL;
    if(ioctl(fd,PCM_CONFIGURE,&format)>=0||errno!=EINVAL)return 3;
    format.channels=1;if(ioctl(fd,PCM_CONFIGURE,&format)<0)return 4;
    if(write(fd,(void *)0xffffffffUL,1)>=0||errno!=EFAULT)return 14;
    allocation=malloc(128128UL);if(!allocation)return 15;
    boundary=64000UL-(unsigned)(unsigned long)allocation;if(boundary<512UL)boundary+=64000UL;
    samples=allocation+boundary-512UL;
    for(i=0;i<1024;i++)samples[i]=(short)((long)i*64L-32768L);
    if(write(fd,samples,1024)!=1024)return 5;
    if(write(fd,samples,1)>=0||errno!=EAGAIN)return 6;
    FD_ZERO(&ready);FD_SET(fd,&ready);wait.tv_sec=0;wait.tv_usec=0;
    if(select(fd+1,0,&ready,0,&wait)!=0)return 7;
    child=fork();if(child<0)return 8;
    if(!child){if(write(fd,samples,1)>=0||errno!=EPERM)_exit(1);close(fd);_exit(0);}
    if(waitpid(child,&status,0)<0||status)return 9;
    if(ioctl(fd,PCM_END,0)<0||ioctl(fd,PCM_START,0)<0)return 10;
    if(ioctl(fd,PCM_DRAIN,0)<0)return 11;
    if(ioctl(fd,PCM_STATE,&state)<0||state.position!=1024UL||state.queued||!(state.flags&PCM_DONE))return 12;
    close(fd);
    free(allocation);
    child=fork();if(child<0)return 16;
    if(!child){other=open("/dev/pcm",O_WRONLY);if(other<0)_exit(1);_exit(0);}
    if(waitpid(child,&status,0)<0||status)return 17;
    fd=open("/dev/pcm",O_WRONLY);if(fd<0)return 13;close(fd);
    puts("pcmcheck passed");return 0;
}
