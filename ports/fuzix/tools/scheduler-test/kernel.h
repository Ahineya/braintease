#ifndef SCHEDULER_FIXTURE_KERNEL_H
#define SCHEDULER_FIXTURE_KERNEL_H
#include <stdint.h>
#include <stddef.h>
#define CONFIG_RIPPLE_SCHED
#define PTABSIZE 64
#define MAXTICKS 50
#define P_EMPTY 0
#define P_RUNNING 1
#define P_READY 2
#define P_SLEEP 3
#define P_IOWAIT 4
#define P_STOPPED 5
#define P_FORKING 6
#define P_ZOMBIE 7
#define PFL_BATCH 1
struct p_tab { unsigned p_status,p_flags,p_priority;void *p_wait; };
typedef struct p_tab *ptptr;
typedef unsigned irqflags_t;
extern struct p_tab ptab[PTABSIZE];
extern unsigned nready,need_resched;
extern struct u_data { ptptr u_ptab; } udata;
unsigned di(void);
void irqrestore(unsigned);
void panic(char *);
void pwake(ptptr);
#endif
