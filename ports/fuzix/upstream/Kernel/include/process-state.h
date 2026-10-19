/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FUZIX_PROCESS_STATE_H
#define FUZIX_PROCESS_STATE_H
/* Include after kernel.h. Other ports keep their existing accounting. */
#ifdef CONFIG_RIPPLE_SCHED
void proc_setstate(ptptr, unsigned);
void proc_wait_event(ptptr, void *);
void proc_wake_event(void *);
ptptr proc_pick(void);
void proc_sched_sample(uint32_t, unsigned);
void proc_sched_switch(void);
#define proc_ready_add() ((void)0)
#define proc_ready_remove() ((void)0)
#else
#define proc_setstate(p,state) ((p)->p_status=(state))
#define proc_ready_add() (++nready)
#define proc_ready_remove() (--nready)
#endif
#endif
