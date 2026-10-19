/* SPDX-License-Identifier: GPL-2.0-only */
#include "user.h"
static unsigned failures;
static char data[512];
static void check(int good) { out(good?"Y":"N"); if (!good) ++failures; }
int main(void) {
    int fd,i,j,good=1;
    if (open("/dev/tty1",2)!=0) return 1;
    call(17,0,0,0,0); call(17,0,0,0,0);
    out("Write checks: ");
    for (i=0;i<512;++i) data[i]=0x1200+(i&255);
    fd=open("/tmp/large",1|256|1024,0644);
    check(fd==3);
    for (i=0;i<12;++i) if (write(fd,data,sizeof(data))!=sizeof(data)) break;
    check(i==12 && close(fd)==0);
    fd=open("/tmp/large",0);
    for (i=0;i<12;++i) {
        if (read(fd,data,sizeof(data))!=sizeof(data)) { good=0; break; }
        for (j=0;j<512;++j) if (data[j]!=0x1200+(j&255)) good=0;
    }
    check(good && i==12 && read(fd,data,1)==0 && close(fd)==0);
    fd=open("/tmp/large",1|1024);
    check(fd==3 && write(fd,"new",3)==3 && close(fd)==0);
    fd=open("/tmp/large",1|4);
    check(fd==3 && write(fd,"!",1)==1 && close(fd)==0);
    fd=open("/tmp/large",0);
    check(read(fd,data,8)==4 && data[0]=='n' && data[1]=='e' && data[2]=='w' && data[3]=='!' && close(fd)==0);
    out(failures?"\nWrite FAILED\n":"\nWrite OK\n");
    return failures!=0;
}
