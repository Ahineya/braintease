/* SPDX-License-Identifier: GPL-2.0-only */
#include "user.h"
static int memcmp(const void *a,const void *b,size_t n) { const char *x=a,*y=b; while(n--) if(*x++!=*y++) return 1; return 0; }
static char global[64];
static unsigned failures;
static void check(int good) { out(good?"Y":"N"); if (!good) ++failures; }
int main(void) {
    struct request req;
    char local[64];
    long n;
    int fd,i;
    if (open("/dev/tty1",2)!=0) return 1;
    call(17,0,0,0,0); call(17,0,0,0,0);
    out("ABI checks: ");
    check(call(18,0,0,0,0)==1);
    check(call(65535,0,0,0,0)==-1 && ripple_error==42);
    check(open("/missing",0)==-1 && ripple_error==2);
    fd=open("/BOOT",1); check(fd==3 && close(fd)==0);
    fd=open("/tmp/created",257,0644); check(fd==3 && close(fd)==0);
    check(read(7,local,1)==-1 && ripple_error==9);
    check(write(1,(void *)0x10000UL,1)==-1 && ripple_error==14);
    check(write(1,(void *)0x33fffUL,2)==-1 && ripple_error==14);
    check(read(0,(void *)0x44000UL,1)==-1 && ripple_error==14);
    check(write(1,(void *)0,1)==-1 && ripple_error==14);
    check(ripple_syscall((struct request *)0x10000UL)==-1);
    check(ripple_syscall((struct request *)0x43fffUL)==-1);
    req.call=18; req.error=0;
    check(ripple_syscall(&req)==1 && !req.error);
    fd=open("/BOOT",0);
    n=read(fd,local,sizeof(local));
    check(fd==3 && n==35 && !memcmp(local,"FUZIX kernel running in Brainfuck.\n",35));
    check(read(fd,local,sizeof(local))==0 && close(fd)==0);
    for (i=0;i<8;++i) {
        fd=open("/BOOT",0); n=read(fd,global,sizeof(global));
        if (fd!=3 || n!=35 || close(fd)) break;
    }
    check(i==8);
    fd=open("/README",0);
    check(fd==3 && read(fd,global,sizeof(global))==64 && global[0]=='W');
    close(fd);
    fd=open("/README",0);
    req.call=7; req.arg[0]=fd;
    req.arg[1]=0xffff0000UL | ((unsigned long)global & 65535UL);
    req.arg[2]=sizeof(global); req.arg[3]=0; req.error=0;
    check(ripple_syscall(&req)==64 && !req.error && global[0]=='W');
    close(fd);
    check(call(10,(long)"/dev",0,0,0)==0 && open("tty1",0)==3);
    close(3);
    check(call(10,(long)"/BOOT",0,0,0)==-1 && ripple_error==20);
    out(failures?"\nABI FAILED\n":"\nABI OK\n");
    user_exit();
    return failures!=0;
}
