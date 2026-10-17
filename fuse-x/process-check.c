/* SPDX-License-Identifier: GPL-2.0-only */
#include "user.h"
static int private_global=7;
static void require(int condition, const char *message) {
    if (!condition) { fx_puts("CHECK FAILED: "); fx_puts(message); fx_puts("\n"); fx_exit(1); }
}
void fuse_test_entry(unsigned long argument) {
    struct fx_arguments *args=(void *)argument;
    int local=11, fd, pid, status, ids[3];
    unsigned i;
    char byte;
    char *again[3];
    if (args->argc>1) fx_exit(private_global==7 ? 23 : 1);
    fd=fx_open("/tmp/context-check",O_RDWR|O_CREAT|O_TRUNC,0600);
    require(fd>=0,"open");
    require(fx_write(fd,"abcd",4)==4,"write"); fx_close(fd);
    fd=fx_open("/tmp/context-check",O_RDONLY,0);
    require(fd>=0,"reopen");
    pid=fx_fork(); require(pid>=0,"fork");
    if (!pid) {
        private_global=99; local=22;
        require(fx_read(fd,&byte,1)==1 && byte=='a',"child read");
        fx_call(37,1,0,0);
        fx_exit(private_global==99 && local==22 ? 17 : 1);
    }
    require(fx_wait(pid,&status)==pid && status==(17<<8),"wait status");
    require(private_global==7 && local==11,"private global/stack COW");
    require(fx_read(fd,&byte,1)==1 && byte=='b',"shared open-file offset"); fx_close(fd);
    for (i=0;i<3;i++) {
        ids[i]=fx_fork(); require(ids[i]>=0,"concurrent fork");
        if (!ids[i]) { fx_call(37,2,0,0); fx_exit(20+i); }
    }
    for (i=0;i<3;i++) require(fx_wait(ids[i],&status)==ids[i] && status==((20+i)<<8),"concurrent wait");
    fd=fx_open("/tmp/not-native",O_WRONLY|O_CREAT|O_TRUNC,0700);
    require(fd>=0,"bad exec fixture"); fx_write(fd,"not native",10); fx_close(fd);
    again[0]="check"; again[1]="--after-exec"; again[2]=NULL;
    require(fx_exec("/tmp/not-native",again)==-1 && fx_errno==ENOEXEC && private_global==7,"failed exec rollback");
    for (i=0;i<20;i++) {
        pid=fx_fork(); require(pid>=0,"reused process slot");
        if (!pid) { private_global=99; fx_exec("/bin/check",again); fx_exit(1); }
        require(fx_wait(pid,&status)==pid && status==(23<<8),"exec replaces frames and initializes globals");
    }
    fx_puts("Fuse-X: native process checks passed (fork/COW, wait, shared files, timers, exec and slot reuse)\n");
    fx_exit(0);
}
