/* SPDX-License-Identifier: GPL-2.0-only */
/* Fixed-capacity queues: no allocation, wide bit scans or process-table walks.
 * All membership and nready changes belong to proc_setstate, under di(). */
#include <kernel.h>
#include <kdata.h>
#include <process-state.h>
#define WAIT_BUCKETS 32U
#ifndef RIPPLE_SCHED_QUANTUM_MS
#define RIPPLE_SCHED_QUANTUM_MS 20U
#endif
#define QUANTUM_MS RIPPLE_SCHED_QUANTUM_MS
#define WAKE_MIN_MS 5U
static unsigned run_head, run_tail;
/* Slot plus one is the link encoding; zero is the empty sentinel. */
static unsigned run_next[PTABSIZE], run_prev[PTABSIZE], run_linked[PTABSIZE];
static unsigned wait_head[WAIT_BUCKETS];
static unsigned wait_next[PTABSIZE], wait_prev[PTABSIZE], wait_bucket[PTABSIZE];
static uint32_t sample_seconds, start_seconds;
static unsigned sample_ms, start_ms, budget_ms=QUANTUM_MS;
static unsigned elapsed(void) {
    uint32_t seconds=sample_seconds-start_seconds;
    if(seconds>1)return 2000;
    return (unsigned)(seconds*1000UL+sample_ms-start_ms);
}
static unsigned hash_event(void *event) {
    unsigned long v=(unsigned long)event;
    return (unsigned)(v^(v>>4)^(v>>16))&(WAIT_BUCKETS-1);
}
static void run_remove(unsigned i) {
    unsigned before,after;
    if(!run_linked[i])return;
    before=run_prev[i];after=run_next[i];
    if(before)run_next[before-1]=after;else run_head=after;
    if(after)run_prev[after-1]=before;else run_tail=before;
    run_linked[i]=run_prev[i]=run_next[i]=0;
}
static void run_append(unsigned i) {
    if(run_linked[i])return;
    run_prev[i]=run_tail;run_next[i]=0;
    if(run_tail)run_next[run_tail-1]=i+1;else run_head=i+1;
    run_tail=i+1;run_linked[i]=1;
}
static void wait_remove(unsigned i) {
    unsigned before,after,bucket=wait_bucket[i];
    if(!bucket)return;
    before=wait_prev[i];after=wait_next[i];
    if(before)wait_next[before-1]=after;else wait_head[bucket-1]=after;
    if(after)wait_prev[after-1]=before;
    wait_bucket[i]=wait_next[i]=wait_prev[i]=0;
}
void proc_wait_event(ptptr p,void *event) {
    unsigned i=p-ptab,bucket=hash_event(event),head;
    wait_remove(i);p->p_wait=event;
    head=wait_head[bucket];
    wait_bucket[i]=bucket+1;wait_prev[i]=0;wait_next[i]=head;
    if(head)wait_prev[head-1]=i+1;
    wait_head[bucket]=i+1;
}
void proc_setstate(ptptr p,unsigned state) {
    unsigned i=p-ptab,old,was_ready,is_ready;
    irqflags_t irq=di();
    old=p->p_status;
    was_ready=old==P_RUNNING||old==P_READY;
    is_ready=state==P_RUNNING||state==P_READY;
    if(was_ready&&!is_ready) {
        if(!nready)panic("scheduler count underflow");
        --nready;
    } else if(!was_ready&&is_ready) ++nready;
    if(state!=P_READY)run_remove(i);
    if(state!=P_SLEEP&&state!=P_IOWAIT) {
        wait_remove(i);p->p_wait=NULL;
    }
    p->p_status=state;
    if(state==P_READY) {
        run_append(i);
        /* FIFO order prevents repeated I/O wakeups starving CPU tasks. A
         * minimum slice bounds the cost of wake-triggered rescheduling. */
        if(!was_ready && udata.u_ptab && udata.u_ptab!=p &&
           udata.u_ptab->p_status==P_RUNNING && elapsed()>=WAKE_MIN_MS)
            need_resched=1;
    }
    irqrestore(irq);
}
void proc_wake_event(void *event) {
    unsigned link=wait_head[hash_event(event)],next;
    while(link) {
        ptptr p=&ptab[link-1];
        next=wait_next[link-1];
        if(p->p_wait==event)pwake(p);
        link=next;
    }
}
ptptr proc_pick(void) {
    ptptr p;
    while(run_head) {
        p=&ptab[run_head-1];
        if(p->p_status!=P_READY)panic("scheduler stale queue");
        if(p->p_flags&PFL_BATCH) {
            p->p_flags&=~PFL_BATCH;
            run_remove(p-ptab);run_append(p-ptab);
        } else return p;
    }
    return NULL;
}
void proc_sched_sample(uint32_t seconds,unsigned milliseconds) {
    sample_seconds=seconds;sample_ms=milliseconds;
    if(nready>1 && elapsed()>=budget_ms)need_resched=1;
}
void proc_sched_switch(void) {
    unsigned priority=udata.u_ptab->p_priority;
    start_seconds=sample_seconds;start_ms=sample_ms;
    budget_ms=(unsigned)((uint32_t)QUANTUM_MS*priority/MAXTICKS);
    if(budget_ms<WAKE_MIN_MS)budget_ms=WAKE_MIN_MS;
    if(budget_ms>200)budget_ms=200;
    need_resched=0;
}
