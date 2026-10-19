/* ABI 2 staging: bank crossings, atomic failures, guards and fork/COW. */
#include <vm_crypto.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include "crypto-vectors.h"
#define C(x) do {if(!(x)){printf("gcmdevcheck failed line %u errno %d\n",__LINE__,errno);return 1;}}while(0)
static unsigned char before[VM_CRYPTO_GCM_HEADER+16];
int main(void) {
    unsigned char *arena=malloc(130000UL),*edge;struct vm_crypto_gcm_request *p;
    unsigned cells=VM_CRYPTO_GCM_HEADER+16,i;int fd,pid,status;
    C(arena!=NULL);
    edge=(void *)((((unsigned long)arena>>16)+1)<<16);edge-=64;
    if(edge<arena)edge+=64000UL;
    p=(void *)edge;memset(p,0,sizeof(*p));edge[-1]=0x5aa5;edge[cells]=0x5aa5;
    fd=open("/dev/crypto",O_RDWR);C(fd>=0);
    p->version=2;p->operation=VM_CRYPTO_AES_GCM_ENCRYPT;p->key_len=16;p->data_len=16;
    C(ioctl(fd,RIPPLE_CRYPTO_GCM_IOCTL,p)==0);
    C(!memcmp(p->data,gcm128_cipher,16)&&!memcmp(p->tag,gcm128_tag,16));
    C(edge[-1]==0x5aa5&&edge[cells]==0x5aa5);
    p->operation=VM_CRYPTO_AES_GCM_DECRYPT;p->tag[0]^=1;memcpy(before,p,cells);
    C(ioctl(fd,RIPPLE_CRYPTO_GCM_IOCTL,p)==-1&&errno==EACCES&&!memcmp(before,p,cells));
    p->tag[0]^=1;p->data[0]=256;memcpy(before,p,cells);
    C(ioctl(fd,RIPPLE_CRYPTO_GCM_IOCTL,p)==-1&&errno==EINVAL&&!memcmp(before,p,cells));
    p->data[0]=gcm128_cipher[0];memcpy(before,p,cells);
    pid=fork();C(pid>=0);
    if(!pid) {
        if(ioctl(fd,RIPPLE_CRYPTO_GCM_IOCTL,p))_exit(2);
        for(i=0;i<16;i++)if(p->data[i])_exit(3);
        _exit(0);
    }
    C(waitpid(pid,&status,0)==pid&&status==0&&!memcmp(before,p,cells));
    C(ioctl(fd,RIPPLE_CRYPTO_GCM_IOCTL,p)==0);
    for(i=0;i<16;i++)C(p->data[i]==0);
    C(edge[-1]==0x5aa5&&edge[cells]==0x5aa5);
    p->reserved=1;memcpy(before,p,cells);
    C(ioctl(fd,RIPPLE_CRYPTO_GCM_IOCTL,p)==-1&&errno==EINVAL&&!memcmp(before,p,cells));
    C(ioctl(fd,RIPPLE_CRYPTO_GCM_IOCTL,(void *)0)==-1&&errno==EFAULT);
    memset(p,0,sizeof(*p));p->version=2;p->operation=VM_CRYPTO_AES_GCM_ENCRYPT;
    p->key_len=32;p->aad_len=64;p->data_len=VM_CRYPTO_GCM_MAX;
    for(i=0;i<VM_CRYPTO_GCM_MAX;i++)p->data[i]=i&255;
    edge[sizeof(*p)]=0x5aa5;
    C(ioctl(fd,RIPPLE_CRYPTO_GCM_IOCTL,p)==0);p->operation=VM_CRYPTO_AES_GCM_DECRYPT;
    C(ioctl(fd,RIPPLE_CRYPTO_GCM_IOCTL,p)==0);
    for(i=0;i<VM_CRYPTO_GCM_MAX;i++)C(p->data[i]==(i&255));
    C(edge[-1]==0x5aa5&&edge[sizeof(*p)]==0x5aa5);
    p->data_len=VM_CRYPTO_GCM_MAX+1;
    C(ioctl(fd,RIPPLE_CRYPTO_GCM_IOCTL,p)==-1&&errno==EINVAL);
    close(fd);free(arena);puts("gcmdevcheck passed");return 0;
}
