/* Checked guest crypto facade; the kernel stages all device requests. */
#include <vm_crypto.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
static void erase(void *p,unsigned n) { volatile unsigned char *v=p;while(n--)*v++=0; }
int rfl_crypto_errno(void) { return errno; }
long rfl_crypto_call(unsigned op,unsigned parameter,const unsigned char *a,unsigned long na,
    const unsigned char *b,unsigned long nb,const unsigned char *c,unsigned long nc,
    unsigned char *out,unsigned long capacity) {
    struct vm_crypto_request request;int fd,status,saved;unsigned i;
    if(na>512||nb>512||nc>512||capacity>512){errno=EINVAL;return -1;}
    memset(&request,0,sizeof(request));
    request.version=1;request.operation=op;request.curve=parameter;
    request.a_len=na;request.b_len=nb;request.c_len=nc;request.out_len=capacity;
    memcpy(request.a,a,na);memcpy(request.b,b,nb);memcpy(request.c,c,nc);
    fd=open("/dev/crypto",O_RDWR);
    if(fd<0){erase(&request,sizeof(request));return -1;}
    status=ioctl(fd,RIPPLE_CRYPTO_IOCTL,&request);saved=errno;close(fd);errno=saved;
    if(status<0){erase(&request,sizeof(request));return -1;}
    if(request.out_len>capacity){erase(&request,sizeof(request));errno=EIO;return -1;}
    for(i=0;i<request.out_len;i++)if(request.out[i]>255){erase(&request,sizeof(request));errno=EIO;return -1;}
    status=request.out_len;memcpy(out,request.out,status);erase(&request,sizeof(request));return status;
}
int rfl_crypto_random(unsigned char *out,unsigned long count) {
    int fd,saved;long n;unsigned long at=0;
    if(count>4096UL){errno=EINVAL;return -1;}
    fd=open("/dev/urandom",O_RDONLY);if(fd<0)return -1;
    while(at<count){
        n=read(fd,out+at,count-at);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){saved=n<0?errno:EIO;erase(out,at);close(fd);errno=saved;return -1;}
        at+=n;
    }
    close(fd);return 0;
}
