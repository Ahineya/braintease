/* SPDX-License-Identifier: GPL-2.0-only */
#include "user.h"
static unsigned marker=7, failures;
static void check(int good) { if (!good) { out("N"); ++failures; } else out("Y"); }
static long fork_child(void) { return call(32,0,0,0,0); }
static void leave(unsigned code) { call(0,code,0,0,0); for (;;) {} }
int main(void) {
    unsigned local=17;
    int status,fd,i;
    long pid;
    char bytes[64];
    char *argv[3];
    char *environment[2];
    if (open("/dev/tty1",2)!=0) return 1;
    call(17,0,0,0,0); call(17,0,0,0,0);
    out("Process checks: ");
    fd=open("/BOOT",0);
    pid=fork_child();
    if (!pid) {
        marker=77; local=99;
        if (call(18,0,0,0,0)!=2 || call(19,0,0,0,0)!=1 ||
            fork_child()!=-1 || ripple_error!=11 || read(fd,bytes,6)!=6) leave(99);
        leave(7);
    }
    check(pid==2 && marker==7 && local==17);
    check(call(55,pid,(long)&status,0,0)==pid && status==(7<<8));
    check(read(fd,bytes,sizeof(bytes))==29 && close(fd)==0);
    check(call(55,pid,(long)&status,0,0)==-1 && ripple_error==10);
    for (i=0;i<4;++i) {
        pid=fork_child();
        if (!pid) leave(i);
        if (pid<0 || call(55,pid,(long)&status,0,0)!=pid || status!=(i<<8)) break;
    }
    check(i==4 && marker==7 && local==17);
    pid=fork_child();
    if (!pid) {
        argv[0]="argcheck"; argv[1]="hello"; argv[2]=0;
        call(23,(long)"/bin/argcheck",(long)argv,0,0);
        leave(98);
    }
    check(call(55,pid,(long)&status,0,0)==pid && status==(11<<8));
    check(marker==7 && local==17 && call(18,0,0,0,0)==1);
    pid=fork_child();
    if (!pid) {
        argv[0]="failure"; argv[1]=0;
        environment[0]="X=1"; environment[1]=0;
        if (call(23,(long)"/missing",(long)argv,0,0)!=-1 || ripple_error!=2) leave(90);
        if (call(23,(long)"/bin/bad",(long)argv,0,0)!=-1 || ripple_error!=8) leave(91);
        if (call(23,(long)"/bin/noexec",(long)argv,0,0)!=-1 || ripple_error!=13) leave(92);
        if (call(23,(long)"/bin/argcheck",0x10000UL,0,0)!=-1 || ripple_error!=14) leave(93);
        if (call(23,(long)"/bin/argcheck",(long)argv,(long)environment,0)!=-1 || ripple_error!=44) leave(94);
        for (i=0;i<12;++i)
            if (call(23,(long)"/bin/bad",(long)argv,0,0)!=-1 || ripple_error!=8) leave(95);
        leave(4);
    }
    check(call(55,pid,(long)&status,0,0)==pid && status==(4<<8));
    check(open("/BOOT",0)==3);
    close(3);
    out(failures?"\nProcess FAILED\n":"\nProcess OK\n");
    return failures!=0;
}
