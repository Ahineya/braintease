#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "../../ripple/scheduler.c"
struct p_tab ptab[PTABSIZE];
struct u_data udata;
unsigned nready,need_resched,irq_state=1;
unsigned di(void){unsigned old=irq_state;irq_state=0;return old;}
void irqrestore(unsigned state){irq_state=state;}
void panic(char *s){fprintf(stderr,"panic: %s\n",s);abort();}
void pwake(ptptr p){if(p->p_status==P_SLEEP||p->p_status==P_IOWAIT)proc_setstate(p,P_READY);}
static void invariant(void) {
    unsigned seen[PTABSIZE]={0},waits[PTABSIZE]={0},i,k,previous=0,total=0;
    for(i=run_head;i;i=run_next[i-1]) {
        assert(i<=PTABSIZE&&!seen[i-1]++);
        assert(ptab[i-1].p_status==P_READY&&run_prev[i-1]==previous);
        previous=i;
    }
    assert(previous==run_tail);
    for(k=0;k<WAIT_BUCKETS;++k) {
        previous=0;
        for(i=wait_head[k];i;i=wait_next[i-1]) {
            assert(i<=PTABSIZE&&!waits[i-1]++);
            assert(wait_bucket[i-1]==k+1&&wait_prev[i-1]==previous);
            assert(ptab[i-1].p_status==P_SLEEP||ptab[i-1].p_status==P_IOWAIT);
            assert(hash_event(ptab[i-1].p_wait)==k);previous=i;
        }
    }
    for(i=0;i<PTABSIZE;++i) {
        assert(seen[i]==(ptab[i].p_status==P_READY));
        assert(!!run_linked[i]==!!seen[i]);assert(!!wait_bucket[i]==!!waits[i]);
        total+=ptab[i].p_status==P_READY||ptab[i].p_status==P_RUNNING;
    }
    assert(total==nready);
}
static void reset(void) {
    unsigned i;
    for(i=0;i<PTABSIZE;++i){proc_setstate(&ptab[i],P_EMPTY);memset(&ptab[i],0,sizeof(ptab[i]));ptab[i].p_priority=MAXTICKS;}
    udata.u_ptab=&ptab[0];need_resched=0;invariant();
}
int main(void) {
    unsigned i,j,r=12345;void *a=(void *)(uintptr_t)0x30100,*b=NULL;
    reset();proc_setstate(&ptab[0],P_RUNNING);
    for(i=1;i<PTABSIZE;++i)proc_setstate(&ptab[i],P_READY);
    for(j=0;j<3;++j)for(i=1;i<PTABSIZE;++i) {
        assert(proc_pick()==&ptab[i]);proc_setstate(&ptab[i],P_RUNNING);
        proc_setstate(&ptab[i],P_READY);proc_setstate(&ptab[i],P_READY);invariant();
    }
    /* Deliberately collide distinct event pointers. */
    for(i=1;i<10000;++i){b=(void *)(uintptr_t)(0x40000+i);if(hash_event(a)==hash_event(b))break;}
    assert(i<10000&&a!=b);
    for(i=1;i<PTABSIZE;++i) {proc_setstate(&ptab[i],i&1?P_SLEEP:P_IOWAIT);proc_wait_event(&ptab[i],i&1?a:b);}
    proc_wake_event(a);invariant();
    for(i=1;i<PTABSIZE;++i)assert(ptab[i].p_status==(i&1?P_READY:P_IOWAIT));
    proc_setstate(&ptab[2],P_STOPPED);proc_wake_event(b);assert(ptab[2].p_status==P_STOPPED);
    proc_setstate(&ptab[2],P_READY);proc_setstate(&ptab[2],P_ZOMBIE);proc_setstate(&ptab[2],P_EMPTY);invariant();
    reset();proc_setstate(&ptab[0],P_RUNNING);proc_sched_sample(UINT32_MAX,999);proc_sched_switch();
    proc_setstate(&ptab[1],P_READY);need_resched=0;
    proc_sched_sample(0,QUANTUM_MS-2);assert(!need_resched);
    proc_sched_sample(0,QUANTUM_MS-1);assert(need_resched);
    proc_sched_switch();proc_setstate(&ptab[1],P_SLEEP);proc_wait_event(&ptab[1],a);
    proc_sched_sample(0,QUANTUM_MS+3);pwake(&ptab[1]);assert(!need_resched);
    proc_setstate(&ptab[1],P_SLEEP);proc_wait_event(&ptab[1],a);
    proc_sched_sample(0,QUANTUM_MS+4);pwake(&ptab[1]);assert(need_resched);invariant();
    /* Batch tasks defer one FIFO turn; the flag cannot prevent progress. */
    reset();proc_setstate(&ptab[0],P_RUNNING);
    proc_setstate(&ptab[1],P_READY);proc_setstate(&ptab[2],P_READY);
    ptab[1].p_flags|=PFL_BATCH;
    assert(proc_pick()==&ptab[2] && !(ptab[1].p_flags&PFL_BATCH));
    proc_setstate(&ptab[2],P_SLEEP);proc_wait_event(&ptab[2],NULL);
    assert(proc_pick()==&ptab[1]);proc_wake_event(NULL);
    assert(ptab[2].p_status==P_READY);invariant();
    /* State changes must preserve an already-disabled IRQ state as well. */
    irq_state=0;proc_setstate(&ptab[2],P_STOPPED);assert(!irq_state);
    proc_setstate(&ptab[2],P_READY);assert(!irq_state);irq_state=1;
    /* Priority scaling and both clamps have exact boundary behavior. */
    for(i=0;i<3;++i) {
        unsigned budget=i==0?5:i==1?2*QUANTUM_MS:200;
        ptab[0].p_priority=i==0?0:i==1?2*MAXTICKS:65535U;
        proc_sched_sample(10+i,0);proc_sched_switch();
        proc_sched_sample(10+i,budget-1);assert(!need_resched);
        proc_sched_sample(10+i,budget);assert(need_resched);
    }
    reset();
    for(j=0;j<30000;++j) {
        r=r*1664525U+1013904223U;i=(r>>8)%PTABSIZE;
        switch(r&7) {
        case 0:case 1:proc_setstate(&ptab[i],P_READY);break;
        case 2:case 3:proc_setstate(&ptab[i],r&1?P_SLEEP:P_IOWAIT);proc_wait_event(&ptab[i],r&1?a:b);break;
        case 4:proc_wake_event(r&256?a:b);break;
        case 5:proc_setstate(&ptab[i],P_STOPPED);break;
        case 6:proc_setstate(&ptab[i],P_EMPTY);memset(&ptab[i],0,sizeof(ptab[i]));break;
        case 7:if(proc_pick())proc_setstate(proc_pick(),P_RUNNING);break;
        }
        invariant();assert(irq_state==1);
    }
    reset();puts("scheduler queues, collisions, state accounting, reuse and clock wrap passed");return 0;
}
