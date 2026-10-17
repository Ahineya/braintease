/* SPDX-License-Identifier: GPL-2.0-only */
#include <kernel.h>
#include <kdata.h>
#include <tty.h>
#include <vm_devices.h>
#include "select-abi.h"
#include "selectwait.h"
#include "deadline.h"
#include "pty.h"

extern unsigned ripple_tty_status(unsigned);
extern unsigned ripple_socket_status(inoptr);
extern unsigned ripple_gfx_status(void);
unsigned ripple_select_active;
static uint8_t selecting[PTABSIZE];
static struct ripple_deadline_queue deadlines;
typedef char deadline_slots_fit[(PTABSIZE <= RIPPLE_DEADLINE_SLOTS) ? 1 : -1];

void ripple_select_clock(const VmHostTime *sample) {
    struct ripple_deadline_time now;
    uint32_t ready;
    unsigned slot;
    if (!deadlines.head) return;
    if (sample) {
        now.seconds_low = sample->seconds_low;
        now.seconds_high = sample->seconds_high;
        now.nanoseconds = sample->nanoseconds;
    }
    ready = ripple_deadline_poll(&deadlines, sample ? &now : NULL);
    for (slot = 0; ready; ++slot, ready >>= 1)
        if ((ready & 1) && ptab[slot].p_wait == &deadlines.slots[slot]) pwake(&ptab[slot]);
}

/* Also called before exit closes descriptors: a fatal signal may discard a
 * sleeping syscall instead of returning through its normal cleanup path. */
void ripple_select_exit(ptptr p) {
    inoptr i;unsigned n,cell=(p-ptab)>>3,bit=1U<<((p-ptab)&7);
    irqflags_t irq=di();
    ripple_deadline_cancel(&deadlines, p-ptab);
    if(selecting[p-ptab]){selecting[p-ptab]=0;--ripple_select_active;}
    for(i=i_tab;i<i_tab+ITABSIZE;i++)
        for(n=0;n<3;n++)i->c_select[n].map[cell]&=~bit;
    irqrestore(irq);
}
static unsigned status(inoptr ino) {
    unsigned dev;
    switch(getmode(ino)) {
    case MODE_R(F_PIPE):
        return (ino->c_node.i_size||!ino->c_writers?SELECT_IN:0) |
               (ino->c_node.i_size<8UL*BLKSIZE||!ino->c_readers?SELECT_OUT:0);
    case MODE_R(F_SOCK):return ripple_socket_status(ino);
    case MODE_R(F_CDEV):
        dev=ino->c_node.i_addr[0];
        if(major(dev)==2)return ripple_tty_status(minor(dev));
        if(major(dev)==3)return ripple_pty_select(minor(dev));
#ifndef CONFIG_FUSE_X
        if(major(dev)==4)return ripple_gfx_status();
#endif
        /* Null, zero, system and unknown devices complete or fail immediately. */
        return SELECT_IN|SELECT_OUT;
    default:return SELECT_IN|SELECT_OUT;
    }
}
static int clock_read(VmHostTime *t) {
    irqflags_t irq=di();unsigned result=vm_host_monotonic(t);irqrestore(irq);
    if(result==VM_DEVICE_OK)return 0;
    udata.u_error=EIO;return -1;
}
arg_t _select(void) {
    struct ripple_select_request req;
    struct ripple_fd_set want[3],ready[3];
    struct ripple_select_time duration;
    VmHostTime sample;
    struct ripple_deadline_time start;
    struct ripple_deadline_slot *timer = &deadlines.slots[udata.u_ptab-ptab];
    inoptr nodes[UFTSIZE];
    int nfds=(int)udata.u_argn,result=-1,expired=0;
    unsigned fd,n,mask,have,interest;
    irqflags_t irq;
    if((uint32_t)udata.u_argn>UFTSIZE||nfds<0||nfds>32){udata.u_error=EINVAL;return -1;}
    if(uget((void *)udata.u_argn1,&req,sizeof(req)))return -1;
    if(req.version!=RIPPLE_SELECT_VERSION){udata.u_error=EINVAL;return -1;}
    memset(want,0,sizeof(want));memset(nodes,0,sizeof(nodes));
    for(n=0;n<3;n++)if(req.sets[n]) {
        if(uget(req.sets[n],&want[n],sizeof(want[n]))||
           !valaddr_w(req.sets[n],sizeof(want[n]),sizeof(want[n])))return -1;
    }
    if(req.timeout) {
        if(uget(req.timeout,&duration,sizeof(duration)))return -1;
        if(duration.seconds>0x7fffffffUL||duration.microseconds>=1000000UL){udata.u_error=EINVAL;return -1;}
        expired=!duration.seconds&&!duration.microseconds;
        if(clock_read(&sample))return -1;
        start.seconds_low=sample.seconds_low;
        start.seconds_high=sample.seconds_high;
        start.nanoseconds=sample.nanoseconds;
    }
    for(fd=0;fd<nfds;fd++) {
        mask=1U<<(fd&15);
        if((want[0].bits[fd>>4]|want[1].bits[fd>>4]|want[2].bits[fd>>4])&mask) {
            nodes[fd]=getinode(fd);if(!nodes[fd])return -1;
        }
    }
    irq=di();
    if(req.timeout && !expired && ripple_deadline_arm(&deadlines,
        udata.u_ptab-ptab,&start,duration.seconds,duration.microseconds)) {
        irqrestore(irq);udata.u_error=EOVERFLOW;return -1;
    }
    selecting[udata.u_ptab-ptab]=1;++ripple_select_active;
    /* Register before testing, with IRQs masked through the sleep transition. */
    for(fd=0;fd<nfds;fd++)if(nodes[fd]) {
        interest=0;mask=1U<<(fd&15);
        for(n=0;n<3;n++)if(want[n].bits[fd>>4]&mask)interest|=1U<<n;
        selwait_inode(nodes[fd],interest,1);
    }
    for(;;) {
        di(); /* switchout's single-runnable fast path returns with IRQs on. */
        memset(ready,0,sizeof(ready));result=0;
        for(fd=0;fd<nfds;fd++)if(nodes[fd]) {
            have=status(nodes[fd]);mask=1U<<(fd&15);
            for(n=0;n<3;n++)if((have&(1U<<n))&&(want[n].bits[fd>>4]&mask)) {
                ready[n].bits[fd>>4]|=mask;++result;
            }
        }
        if(result||expired)break;
        if(req.timeout) {
            /* Synchronize under the same protection as the sleep transition.
             * A past deadline must not be lost between arming and sleeping. */
            sync_clock();
            if(timer->state==RIPPLE_DEADLINE_ERROR){udata.u_error=EIO;result=-1;break;}
            if(timer->state==RIPPLE_DEADLINE_EXPIRED)break;
        }
        if(psleep_flags(timer,0)){result=-1;break;}
    }
    di();
    ripple_select_exit(udata.u_ptab);
    udata.u_ptab->p_timeout=0;
    irqrestore(irq);
    if(result<0)return -1;
    for(n=0;n<3;n++)if(req.sets[n]&&uput(&ready[n],req.sets[n],sizeof(ready[n])))return -1;
    return result;
}
