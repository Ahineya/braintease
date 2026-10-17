/* SPDX-License-Identifier: GPL-2.0-only */
#include "user.h"
int fx_errno;
long fx_call(unsigned call, unsigned long a, unsigned long b, unsigned long c) {
    return fuse_syscall(call,a,b,c,&fx_errno);
}
int fx_open(const char *p, unsigned f, unsigned m) { return fx_call(1,(unsigned long)p,f,m); }
int fx_close(int fd) { return fx_call(2,fd,0,0); }
long fx_read(int fd, void *p, unsigned n) { return fx_call(7,fd,(unsigned long)p,n); }
long fx_write(int fd, const void *p, unsigned n) { return fx_call(8,fd,(unsigned long)p,n); }
int fx_fork(void) { return fx_call(32,0,0,0); }
int fx_wait(int pid, int *status) { return fx_call(55,pid,(unsigned long)status,0); }
int fx_exec(const char *path, char **argv) {
    char *environment[1]; environment[0]=NULL;
    return fx_call(23,(unsigned long)path,(unsigned long)argv,(unsigned long)environment);
}
void fx_exit(int status) { fx_call(0,status,0,0); for (;;) {} }
void fx_shutdown(void) { fx_call(57,A_SHUTDOWN,0,0); fx_exit(1); }
void fx_puts(const char *s) {
    unsigned left=strlen(s);
    long n;
    while (left) {
        n=fx_write(1,s,left);
        if (n<=0) return;
        s+=n; left-=n;
    }
}
void fx_number(unsigned n) {
    char digits[8]; unsigned count=0;
    do { digits[count++]='0'+n%10; n/=10; } while(n);
    while(count) fx_write(1,&digits[--count],1);
}
void fx_error(const char *name) {
    unsigned error=fx_errno;
    fx_puts(name); fx_puts(": errno "); fx_number(error); fx_puts("\n");
}
int fx_run(const char *path, char **arguments) {
    int status, pid=fx_fork();
    if (pid<0) { fx_error("fork"); return 1; }
    if (!pid) {
        fx_call(35,SIGINT,0,0); fx_call(35,SIGQUIT,0,0);
        fx_exec(path,arguments); fx_error(path); fx_exit(127);
    }
    while (fx_wait(pid,&status)<0) if (fx_errno!=EINTR) { fx_error("wait"); return 1; }
    return (status>>8)&255;
}
