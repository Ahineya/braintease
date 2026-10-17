/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <syscalls.h>
#include "resources.h"
static int value=7;

static int infofd;
static struct ripple_sysinfo info(void) {
    struct ripple_sysinfo r;
    if(ioctl(infofd,RIPPLE_SYSINFO,&r)||r.version!=1) {puts("mmucheck FAILED sysinfo");exit(30);}
    return r;
}
static int reaped(int pid,int code) {
    int status;
    if(waitpid(pid,&status,0)!=pid || status!=(code<<8)) {
        printf("mmucheck: child %d status %d expected %d\n",pid,status,code<<8);return 0;
    }
    return 1;
}
static void heap_edges(void) {
    char *base, *old, *a, *b, *larger;
    unsigned long i;
    struct ripple_sysinfo before, grown, shrunk;
    int fd;
    base=sbrk(0);
    if(sbrk((512U-(unsigned long)base%65536UL%512U)%512U)==(void *)-1)_exit(31);
    base=sbrk(0); before=info();
    if(sbrk(160000L)==(void *)-1)_exit(32);
    grown=info();
    if(grown.used_kib<=before.used_kib)_exit(33);
    memset(base,0xabcd,160000UL);
    if(brk(base+17))_exit(34);
    shrunk=info();
    if(shrunk.used_kib>=grown.used_kib)_exit(35);
    if(brk(base+160000UL))_exit(36);
    for(i=0;i<17;i++)if((unsigned char)base[i]!=0xabcdU)_exit(37);
    for(i=17;i<160000UL;i++)if(base[i])_exit(38);
    old=sbrk(0); errno=0;
    if(sbrk(2000000L)!=(void *)-1||errno!=ENOMEM||sbrk(0)!=old)_exit(39);
    fd=open("/dev/zero",O_RDONLY);
    errno=0;
    if(fd<0||read(fd,(void *)0x180000UL,16)!=-1||errno!=EFAULT)_exit(40);
    close(fd);
    if(brk(base))_exit(41);
    shrunk=info();
    if(shrunk.mapped_kib!=before.mapped_kib)_exit(42);
    a=malloc(180000UL); b=malloc(70000UL);
    if(!a||!b)_exit(78);
    memset(a,0xdead,180000UL);memset(b,0xbeef,70000UL);
    larger=realloc(a,400000UL);
    if(!larger)_exit(79);
    for(i=0;i<180000UL;i++)if((unsigned char)larger[i]!=0xdeadU)_exit(80);
    errno=0;
    if(realloc(larger,2000000UL)!=NULL||errno!=ENOMEM)_exit(81);
    for(i=0;i<70000UL;i++)if((unsigned char)b[i]!=0xbeefU)_exit(82);
    free(b);free(larger);
    a=calloc(300000UL,1);
    if(!a)_exit(83);
    for(i=0;i<300000UL;i++)if(a[i])_exit(84);
    free(a);
    _exit(0);
}
static void capacity(void) {
    int gate[2], children[32], count=0, pid, status, i;
    char c;
    struct ripple_sysinfo before=info(), after;
    if(pipe(gate))exit(43);
    while(count<32) {
        pid=fork();
        if(pid<0)break;
        if(!pid) {
            int zero = open("/dev/zero", O_RDONLY);
            int null = open("/dev/null", O_WRONLY);
            if(zero<0 || null<0)_exit(76);
            close(gate[1]);
            if(read(gate[0],&c,1)!=1)_exit(44);
            close(zero);close(null);
            _exit(0);
        }
        children[count++]=pid;
    }
    if(errno!=EAGAIN || count!=before.slots-before.processes)exit(45);
    after=info();
    if(!after.shared_kib||after.mapped_kib<=after.used_kib)exit(85);
    close(gate[0]);
    for(i=0;i<count;i++)if(write(gate[1],"x",1)!=1)exit(46);
    close(gate[1]);
    for(i=0;i<count;i++)if(!reaped(children[i],0))exit(47);
    after=info();
    if(after.used_kib!=before.used_kib||after.processes!=before.processes)exit(48);
    pid=fork();if(pid<0)exit(49);if(!pid)_exit(0);
    if(!reaped(pid,0))exit(50);
    printf("mmucheck: process exhaustion (%d children) and slot recovery passed\n",count);
}
/* Fill the physical pool using independent large heaps. Each worker retains
 * its heap until the parent releases the gate, then exit must reclaim it. */
static void memory_worker(int gate,int report) {
    struct ripple_sysinfo r;
    char *base=sbrk(0), *old, *args[2]={"hello",NULL};
    unsigned long grown=0;
    unsigned outcome=0;
    unsigned long prior;
    int pid, status;
    char c;
    /* Warm the reporting/call stack while pages are still plentiful. */
    r=info();
    close(-1);
    for(;;) {
        r=info();prior=r.used_kib;old=sbrk(0);
        if(sbrk(64000L)==(void *)-1) {
            r=info();
            if(r.used_kib!=prior||sbrk(0)!=old)_exit(86);
            break;
        }
        grown+=64000UL;
    }
    while(sbrk(512L)!=(void *)-1)grown+=512UL;
    old=sbrk(0);
    r=info();
    if(errno!=ENOMEM)_exit(51);
    if(r.used_kib==r.pool_kib) {
        /* A failed multi-page request must not leak pages or move the break. */
        if(sbrk(64000L)!=(void *)-1||sbrk(0)!=old)_exit(52);
        r=info();
        if(r.used_kib!=r.pool_kib)_exit(53);
        outcome=1;
        /* Reserve fewer than exec's 250 pages. The child first privatizes its
         * whole stack, so testing a kernel copy failure cannot accidentally
         * fault on a fresh userspace call-frame write instead. */
        if(grown<98304UL||sbrk(-98304L)==(void *)-1)_exit(54);
        pid=fork();
        if(pid<0)_exit(55);
        if(!pid) {
            int zero = open("/dev/zero", O_RDONLY);
            unsigned off;
            for(off=0;off<64000U;off+=512U) {
                volatile unsigned char *cell=(void *)(0x1f0000UL+off);
                unsigned char saved=*cell;
                *cell=saved;
            }
            if(zero<0 || read(zero,&c,1)!=1)_exit(73);
            close(-1); /* Privatize libc's errno/error cells before exhaustion. */
            r=info();
            while(sbrk(512L)!=(void *)-1) {}
            r=info();
            if(r.used_kib!=r.pool_kib)_exit(74);
            /* Kernel copy-out must return ENOMEM, without writing a shared
             * page or taking a fatal supervisor fault. */
            errno=0;
            if(read(zero,base,1)!=-1 || errno!=ENOMEM)_exit(75);
            close(zero);
            execve("/bin/hello",args,args+1);
            _exit(errno==ENOMEM ? 0 : 56);
        }
        if(!reaped(pid,0))_exit(57);
    }
    if(write(report,&outcome,sizeof(outcome))!=sizeof(outcome))_exit(58);
    close(report);
    if(read(gate,&c,1)!=1)_exit(59);
    /* Touch a high character bit at every bank boundary before exiting. */
    if(!outcome)for(grown=0;base+grown<(char *)sbrk(0);grown+=64000UL)base[grown]=0xbeef;
    _exit(0);
}
static void exhaustion(void) {
    int gate[2], report[2], children[12], count=0, pid, i;
    unsigned outcome=0;
    struct ripple_sysinfo before=info(), after;
    if(pipe(gate))exit(60);
    while(count<12&&!outcome) {
        if(pipe(report))exit(60);
        pid=fork();if(pid<0)exit(61);
        if(!pid) {
            close(gate[1]);close(report[0]);
            memory_worker(gate[0],report[1]);
        }
        children[count++]=pid;
        close(report[1]);
        if(read(report[0],&outcome,sizeof(outcome))!=sizeof(outcome)) {
            int status;waitpid(pid,&status,0);printf("mmucheck FAILED worker status %d\n",status);exit(62);
        }
        close(report[0]);
    }
    if(!outcome)exit(63);
    close(gate[0]);
    for(i=0;i<count;i++)if(write(gate[1],"x",1)!=1)exit(64);
    close(gate[1]);
    for(i=0;i<count;i++)if(!reaped(children[i],0))exit(65);
    after=info();
    if(after.used_kib!=before.used_kib||after.processes!=before.processes)exit(66);
    pid=fork();if(pid<0)exit(67);
    if(!pid) {
        char *p=malloc(300000UL);
        if(!p)_exit(68);
        memset(p,0x9876,300000UL);free(p);_exit(0);
    }
    if(!reaped(pid,0))exit(69);
    puts("mmucheck: physical exhaustion, failed exec, and page recovery passed");
}
static void stress(void) {
    int pid, round;
    infofd=open("/dev/proc",O_RDONLY);
    if(infofd<0)exit(70);
    for(round=0;round<2;round++) {
        pid=fork();if(pid<0)exit(71);if(!pid)heap_edges();
        if(!reaped(pid,0)) {puts("mmucheck FAILED heap edges");exit(72);}
        capacity();exhaustion();
    }
    close(infofd);
    puts("mmucheck: shrink/regrow, ENOMEM rollback, and repeated cleanup passed");
}

int main(int argc, char **argv) {
    unsigned long n=200000UL, i;
    unsigned char *heap=malloc(n), byte;
    int pid, status, fds[2], children[10], count;
    if (!heap) {puts("mmucheck FAILED allocation");return 1;}
    for (i=0;i<n;i++) heap[i]=(unsigned char)(i%251);
    pid=fork();
    if (pid<0) return 2;
    if (!pid) {
        for (i=0;i<n;i++) if (heap[i]!=(unsigned char)(i%251)) _exit(3);
        memset(heap,123,n); value=99;
        _exit(23);
    }
    if (waitpid(pid,&status,0)!=pid || (status>>8)!=23 || value!=7) return 4;
    for (i=0;i<n;i++) if (heap[i]!=(unsigned char)(i%251)) return 5;
    puts("mmucheck: 200000-cell heap and COW isolation passed");
    if (pipe(fds)) return 6;
    count=0;
    for (i=0;i<10;i++) {
        pid=fork();
        if (pid<0) break;
        if (!pid) {
            close(fds[1]);
            if (read(fds[0],&byte,1)!=1) _exit(7);
            heap[i]=77;
            _exit(0);
        }
        children[count++]=pid;
    }
    close(fds[0]);
    if (count!=10) {puts("mmucheck FAILED process capacity");return 8;}
    if (write(fds[1],"0123456789",10)!=10) return 9;
    close(fds[1]);
    for (i=0;i<10;i++) if (waitpid(children[i],&status,0)!=children[i] || status) return 10;
    puts("mmucheck: ten simultaneous children passed");
    pid=fork();
    if (!pid) {*(volatile unsigned *)0x30000UL=1;_exit(11);}
    if (pid<0 || waitpid(pid,&status,0)!=pid || (status&127)!=11) return 12;
    pid=fork();
    if (!pid) {*(volatile unsigned *)22UL=0;_exit(11);}
    if (pid<0 || waitpid(pid,&status,0)!=pid || (status&127)!=11) return 13;
    pid=fork();
    if (!pid) {asm volatile("HALT");_exit(11);}
    if (pid<0 || waitpid(pid,&status,0)!=pid || (status&127)!=4) return 14;
    free(heap);
    puts("mmucheck: kernel and MMIO protection passed");
    if(argc==2 && !strcmp(argv[1],"--stress"))stress();
    return 0;
}
