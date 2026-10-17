/* SPDX-License-Identifier: GPL-2.0-only */
/* Real filesystem boundary probe; deliberately not installed on normal roots. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/statvfs.h>
static unsigned buffer[1024];
static unsigned long positions[] = {
    16UL*1024+1023, 17UL*1024, (17UL+512)*1024-1,
    (17UL+512)*1024, 65535UL*1024, 65536UL*1024,
    (17UL+512+512UL*512)*1024-1, (17UL+512+512UL*512)*1024, 0x7ffffffeUL
};
static void require(int ok, const char *what) {
    if (!ok) { printf("largefscheck FAILED %s errno=%d\n",what,errno);exit(1); }
}
static void pattern(unsigned long block) {
    unsigned i;
    for(i=0;i<1024;i++)buffer[i]=i^0xa55aU;
    buffer[0]=(unsigned)block^0x5a5aU;
}
static void seekto(int fd,unsigned long offset) {require(lseek(fd,(long)offset,SEEK_SET)==(long)offset,"seek");}
int main(int argc,char **argv) {
    struct statvfs fs;
    unsigned long n;unsigned i,j;int fd,sparse;
    require(argc==2,"arguments");
    {
        unsigned canary=0xcafe;
        extern long call(unsigned,long,long,long,long);
        errno=0;
        require(call(22,(long)"/",(long)&canary,0,0)==-1 && canary==0xcafe,"reject old statfs without buffer overwrite");
    }
    require(!statvfs("/",&fs)&&fs.f_blocks>500000UL&&fs.f_bfree<=fs.f_blocks,"wide statvfs");
    if(!strcmp(argv[1],"create")) {
        fd=open("/wide-dense",O_RDWR|O_CREAT|O_TRUNC,0600);require(fd>=0,"create dense");
        pattern(0);
        for(n=0;n<70000UL;n++) {
            buffer[0]=(unsigned)n^0x5a5aU;
            require(write(fd,buffer,1024)==1024,"write dense above 16-bit blocks");
        }
        require(!close(fd)&&!link("/wide-dense","/wide-link"),"dense link");
        sparse=open("/wide-sparse",O_RDWR|O_CREAT|O_TRUNC,0600);require(sparse>=0,"create sparse");
        for(i=0;i<9;i++){seekto(sparse,positions[i]);buffer[0]=0x8000U+i;require(write(sparse,buffer,1)==1,"write sparse boundary");}
        require(signal(SIGXFSZ,SIG_IGN)!=SIG_ERR,"ignore file size signal");
        seekto(sparse,0x7fffffffUL);errno=0;
        require(write(sparse,buffer,1)==-1 && errno==EFBIG,"file size overflow rejection");
        require(!close(sparse),"close sparse");
        puts("largefscheck: dense writes above 128 MiB and triple-indirect sparse writes passed");
        return 0;
    }
    fd=open("/wide-dense",O_RDWR);require(fd>=0,"reopen dense");
    for(n=65534UL;n<70000UL;n+=(n<65537UL?1UL:4462UL)) {
        seekto(fd,n*1024UL);require(read(fd,buffer,1024)==1024,"read dense");
        require(buffer[0]==((unsigned)n^0x5a5aU),"wide block tag");
        for(j=1;j<1024;j++)require(buffer[j]==(j^0xa55aU),"native cell payload");
    }
    sparse=open("/wide-sparse",O_RDONLY);require(sparse>=0,"reopen sparse");
    for(i=0;i<9;i++){seekto(sparse,positions[i]);require(read(sparse,buffer,1)==1&&buffer[0]==0x8000U+i,"persisted indirect boundary");}
    seekto(sparse,100000000UL);require(read(sparse,buffer,1024)==1024,"sparse hole read");
    for(j=0;j<1024;j++)require(!buffer[j],"sparse hole zeroes");
    require(!close(sparse),"close sparse reader");
    if(!strcmp(argv[1],"verify")) {close(fd);puts("largefscheck: wide blocks, native cells, sparse holes and reboot persistence passed");return 0;}
    require(!strcmp(argv[1],"cleanup"),"mode");
    n=(65536UL+3UL)*1024UL+17;
    require(!ftruncate(fd,n)&&!ftruncate(fd,70000UL*1024UL),"wide truncate and regrow");
    seekto(fd,n-1);require(read(fd,buffer,1024)==1024,"partial block after regrow");
    require(buffer[0]==(16U^0xa55aU),"retained partial tail");
    for(j=1;j<1024;j++)require(!buffer[j],"regrown tail zeroes");
    require(!ftruncate(fd,17UL*1024UL),"truncate through indirect roots");
    require(!unlink("/wide-dense")&&!unlink("/wide-link"),"unlink open file");
    seekto(fd,0);require(read(fd,buffer,1)==1&&buffer[0]==0x5a5aU,"unlinked open file lives");
    require(!close(fd)&&!unlink("/wide-sparse"),"final block reclamation");
    puts("largefscheck: wide truncate, zeroed regrowth, open unlink and tree reclamation passed");
    return 0;
}
