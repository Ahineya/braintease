/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_NETUTIL_H
#define RIPPLE_NETUTIL_H
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <syscalls.h>
extern long call(unsigned,long,long,long,long);
static unsigned long nu_now(void) {__ktime_t t;_time(&t,1);return t.low*100UL;}
static int nu_wait(void) {return call(37,1,0,0,0)<0?-1:0;}
static int nu_expired(unsigned long start,unsigned long limit) {
    if(nu_now()-start<limit)return 0;
    errno=ETIMEDOUT;return 1;
}
static int nu_connect(int fd,const struct sockaddr *a,socklen_t size,unsigned long timeout) {
    unsigned long start=nu_now();int r;
    if(fcntl(fd,F_SETFL,O_NONBLOCK)<0)return -1;
    for(;;) {
        r=connect(fd,a,size);if(!r||errno==EISCONN)return 0;
        if(errno!=EINPROGRESS&&errno!=EALREADY)return -1;
        if(nu_expired(start,timeout)||nu_wait())return -1;
    }
}
static ssize_t nu_read(int fd,void *p,size_t n,unsigned long timeout) {
    unsigned long start=nu_now();ssize_t r;
    for(;;) {
        r=read(fd,p,n);if(r>=0||errno!=EAGAIN)return r;
        if(nu_expired(start,timeout)||nu_wait())return -1;
    }
}
static int nu_write(int fd,const void *p,size_t n,unsigned long timeout) {
    const char *s=p;ssize_t r;unsigned long start=nu_now();
    while(n) {
        r=write(fd,s,n>32767UL?32767UL:n);
        if(r>0){s+=r;n-=r;start=nu_now();continue;}
        if(r==0){errno=EIO;return -1;}
        if(errno!=EAGAIN||nu_expired(start,timeout)||nu_wait())return -1;
    }
    return 0;
}
static int nu_exact(int fd,void *p,unsigned n,unsigned long timeout) {
    char *s=p;ssize_t r;
    while(n) {
        r=nu_read(fd,s,n,timeout);if(r<0)return -1;
        if(!r){errno=EIO;return -1;}s+=r;n-=r;
    }
    return 0;
}
#endif
