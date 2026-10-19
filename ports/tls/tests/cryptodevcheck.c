#include <vm_crypto.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#define C(x) do{if(!(x)){printf("cryptodevcheck FAILED line %u errno %d\n",__LINE__,errno);return 1;}}while(0)
static struct vm_crypto_request before;
int main(void) {
    unsigned char *arena=malloc(130000UL),*edge;
    struct vm_crypto_request *p;
    int fd,pid,status;
    C(arena!=NULL);
    edge=(void *)((((unsigned long)arena>>16)+1)<<16);edge-=8;
    if(edge<arena)edge+=64000UL;
    p=(void *)edge;memset(p,0,sizeof(*p));edge[-1]=0x5aa5;edge[sizeof(*p)]=0x5aa5;
    fd=open("/dev/crypto",O_RDWR);C(fd>=0);
    p->version=1;p->out_len=512;
    C(ioctl(fd,RIPPLE_CRYPTO_IOCTL,p)==0&&p->out_len==3&&p->out[0]==1&&p->out[1]==7);
    C(edge[-1]==0x5aa5&&edge[sizeof(*p)]==0x5aa5);
    memcpy(&before,p,sizeof(*p));
    p->reserved=1;before.reserved=1;
    C(ioctl(fd,RIPPLE_CRYPTO_IOCTL,p)==-1&&errno==EINVAL&&memcmp(&before,p,sizeof(*p))==0);
    C(ioctl(fd,RIPPLE_CRYPTO_IOCTL,(void *)0)==-1&&errno==EFAULT);
    p->reserved=0;p->operation=VM_CRYPTO_EC_PUBLIC;p->curve=256;p->a_len=32;p->a[31]=1;p->out_len=65;
    p->a[0]=256;memcpy(&before,p,sizeof(*p));
    C(ioctl(fd,RIPPLE_CRYPTO_IOCTL,p)==-1&&errno==EINVAL&&memcmp(&before,p,sizeof(*p))==0);
    p->a[0]=0;memcpy(&before,p,sizeof(*p));
    pid=fork();C(pid>=0);
    if(!pid){if(ioctl(fd,RIPPLE_CRYPTO_IOCTL,p)||p->out_len!=65||p->out[0]!=4)_exit(1);_exit(0);}
    C(waitpid(pid,&status,0)==pid&&status==0);
    C(memcmp(&before,p,sizeof(*p))==0);
    C(ioctl(fd,RIPPLE_CRYPTO_IOCTL,p)==0&&p->out_len==65&&p->out[0]==4);
    C(edge[-1]==0x5aa5&&edge[sizeof(*p)]==0x5aa5);
    close(fd);free(arena);puts("cryptodevcheck passed");return 0;
}
