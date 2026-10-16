#ifndef RCC_IRQ_H
#define RCC_IRQ_H
#include <mmio.h>
#include <mmio_constants.h>

/* CPU-owned return state. Assembly entry code must preserve any other live
 * registers before calling C. IRET, after that restoration, is the only return. */
typedef struct irq_resume {
    unsigned pc;
    unsigned pcb;
    unsigned enable;
    unsigned sb;
    unsigned sp;
    unsigned fp;
} irq_resume;

unsigned irq_abi_version(void);
void irq_get_resume(irq_resume *state);
void irq_set_resume(const irq_resume *state);
/* top==0 disables automatic stack switching. Each handler frame must fit. */
void irq_set_stack(unsigned bank, unsigned top);
/* Period in executed guest instructions; zero disables. Separate from game time. */
void irq_set_timer(unsigned long period);
void irq_clear_pending(unsigned bits);
#endif
