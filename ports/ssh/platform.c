/* Guest-owned protocol and keys; only entropy and stateless crypto use devices. */
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <wolfssl/wolfcrypt/types.h>
#include <utime.h>
/* The guest's compact printf does not implement %lld. Preserve all 64 SFTP
 * size bits without depending on that optional formatter capability. */
int riplix_ssh_size_string(char *out,unsigned capacity,word32 low,word32 high) {
    char digits[20];unsigned count=0,width,i;
    if(!high) {
        do {digits[count++]='0'+low%10;low/=10;}while(low);
    } else {
        word64 value=((word64)high<<32)|low;
        do {digits[count++]='0'+(unsigned)(value%10);value/=10;}while(value);
    }
    width=count<8?8:count;
    if(!out||capacity<=width){errno=ERANGE;return -1;}
    for(i=0;i<width-count;i++)out[i]=' ';
    while(count)out[i++]=digits[--count];
    out[i]=0;return i;
}
int riplix_ssh_settime(const char *path,word32 atime,word32 mtime) {
    struct utimbuf times;
    if(atime>2147483647UL||mtime>2147483647UL){errno=EINVAL;return -1;}
    times.actime=atime;times.modtime=mtime;return utime(path,&times);
}
int riplix_ssh_truncate(const char *path,word64 size) {
    int fd,r,saved;
    if(size>2147483647ULL){errno=EFBIG;return -1;}
    fd=open(path,O_WRONLY);if(fd<0)return -1;
    r=ftruncate(fd,(off_t)size);saved=errno;
    if(close(fd)<0&&r==0)return -1;
    errno=saved;return r;
}
static int seek_file(int fd,const word32 *offset) {
    if(offset[1]||offset[0]>2147483647UL){errno=EFBIG;return -1;}
    return lseek(fd,(off_t)offset[0],SEEK_SET)<0?-1:0;
}
int32_t riplix_ssh_pread(int fd,byte *data,word32 size,const word32 *offset) {
    ssize_t n;unsigned i;
    if(seek_file(fd,offset))return -1;
    do {n=read(fd,data,size>16384UL?16384:(unsigned)size);}while(n<0&&errno==EINTR);
    if(n>0)for(i=0;i<(unsigned)n;i++)if(data[i]>255){errno=EIO;return -1;}
    return n;
}
int32_t riplix_ssh_pwrite(int fd,byte *data,word32 size,const word32 *offset) {
    ssize_t n;
    if(seek_file(fd,offset))return -1;
    do {n=write(fd,data,size>16384UL?16384:(unsigned)size);}while(n<0&&errno==EINTR);
    return n;
}
int32_t riplix_ssh_append(int fd,byte *data,word32 size) {
    ssize_t n;
    do {n=write(fd,data,size>16384UL?16384:(unsigned)size);}while(n<0&&errno==EINTR);
    return n;
}

int ripple_ssh_seed(unsigned char *output, unsigned long count) {
    int fd=open("/dev/urandom",O_RDONLY), saved;
    ssize_t n;
    if(fd<0)return RNG_FAILURE_E;
    while(count) {
        n=read(fd,output,count>256?256:count);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){saved=errno;close(fd);errno=saved;return RNG_FAILURE_E;}
        output+=n;count-=n;
    }
    return close(fd)<0?RNG_FAILURE_E:0;
}
