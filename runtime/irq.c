#include <irq.h>

unsigned irq_abi_version(void) {
    unsigned value;
    asm volatile ("CSRR %0, 11" : "=r"(value));
    return value;
}

void irq_get_resume(irq_resume *state) {
    unsigned value;
    asm volatile ("CSRR %0, 0" : "=r"(value)); state->pc = value;
    asm volatile ("CSRR %0, 1" : "=r"(value)); state->pcb = value;
    asm volatile ("CSRR %0, 2" : "=r"(value)); state->enable = value;
    asm volatile ("CSRR %0, 7" : "=r"(value)); state->sb = value;
    asm volatile ("CSRR %0, 8" : "=r"(value)); state->sp = value;
    asm volatile ("CSRR %0, 9" : "=r"(value)); state->fp = value;
}

void irq_set_resume(const irq_resume *state) {
    asm volatile ("CSRW %0, 0" : : "r"(state->pc));
    asm volatile ("CSRW %0, 1" : : "r"(state->pcb));
    asm volatile ("CSRW %0, 2" : : "r"(state->enable));
    asm volatile ("CSRW %0, 7" : : "r"(state->sb));
    asm volatile ("CSRW %0, 8" : : "r"(state->sp));
    asm volatile ("CSRW %0, 9" : : "r"(state->fp));
}

void irq_set_stack(unsigned bank, unsigned top) {
    unsigned zero = 0;
    asm volatile ("CSRW %0, 6" : : "r"(zero));
    if (top) {
        asm volatile ("CSRW %0, 5" : : "r"(bank));
        asm volatile ("CSRW %0, 6" : : "r"(top));
    }
}

void irq_clear_pending(unsigned bits) {
    asm volatile ("CSRW %0, 10" : : "r"(bits));
}

void irq_set_timer(unsigned long period) {
    unsigned mask = irq_get_enable();
    unsigned low = (unsigned)period;
    unsigned high = (unsigned)(period >> 16);
    unsigned zero = 0;
    irq_set_enable(0);
    asm volatile ("CSRW %0, 3" : : "r"(zero));
    asm volatile ("CSRW %0, 4" : : "r"(zero));
    asm volatile ("CSRW %0, 4" : : "r"(high));
    asm volatile ("CSRW %0, 3" : : "r"(low));
    irq_clear_pending(IRQ_TIMER);
    irq_set_enable(mask);
}
