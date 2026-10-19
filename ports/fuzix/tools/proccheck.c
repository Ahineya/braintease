/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fill the real kernel table, including timeout slots above bit 31. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/select.h>
#include "resources.h"
#include "process-limits.h"
#include "process-report.h"
static int marker=7;
static struct ripple_procinfo snapshot[REPORT_SLOTS];
static void check(int ok,const char *what) {
    if(!ok){fprintf(stderr,"PROCCHECK_FAILED %s errno %d\n",what,errno);exit(1);}
}
int main(void) {
    struct ripple_sysinfo before,full,after;
    struct ripple_procinfo p;
    int proc,gate[2],ids[RIPPLE_PROCESS_SLOTS],pid,status,count,round,i;
    char tokens[RIPPLE_PROCESS_SLOTS];
    proc=open("/dev/proc",O_RDONLY);check(proc>=0,"proc open");
    check(!ioctl(proc,RIPPLE_SYSINFO,&before)&&before.slots==RIPPLE_PROCESS_SLOTS,"64-slot kernel");
    for(round=0;round<2;round++) {
        check(!pipe(gate),"gate");count=0;
        while(count<RIPPLE_PROCESS_SLOTS) {
            pid=fork();if(pid<0)break;
            if(!pid) {
                struct timeval t;
                char token;
                close(gate[1]);
                if(marker!=7||read(gate[0],&token,1)!=1)_exit(11);
                marker=count+20;
                /* Every process must be able to arm and wake its own timer,
                 * especially slots 32..63 and after those slots are reused. */
                t.tv_sec=0;t.tv_usec=20000;
                if(select(0,NULL,NULL,NULL,&t)!=0)_exit(12);
                _exit(marker==count+20?0:13);
            }
            ids[count++]=pid;
        }
        check(errno==EAGAIN&&count==before.slots-before.processes,"process exhaustion");
        check(!ioctl(proc,RIPPLE_SYSINFO,&full)&&full.processes==RIPPLE_PROCESS_SLOTS,"all slots occupied");
        check(!process_snapshot(proc,&full,snapshot)&&snapshot[RIPPLE_PROCESS_SLOTS-1].pid,"userspace snapshot capacity");
        memset(&p,0,sizeof(p));p.slot=RIPPLE_PROCESS_SLOTS-1;
        check(!ioctl(proc,RIPPLE_PROCINFO,&p)&&p.version==RIPPLE_PROCINFO_VERSION&&p.pid&&p.state,"last slot reporting");
        printf("PROCCHECK_FULL %u processes; last slot %u PID %u\n",full.processes,p.slot,p.pid);fflush(stdout);
        close(gate[0]);memset(tokens,'x',count);
        check(write(gate[1],tokens,count)==count,"release children");close(gate[1]);
        for(i=0;i<count;i++)check(waitpid(ids[i],&status,0)==ids[i]&&WIFEXITED(status)&&!WEXITSTATUS(status),"child timeout and exit");
        check(marker==7,"private globals");
        check(!ioctl(proc,RIPPLE_SYSINFO,&after)&&after.processes==before.processes,"slot recovery");
    }
    close(proc);puts("PROCCHECK_OK 64 slots, exhaustion, high-slot select and reuse");return 0;
}
