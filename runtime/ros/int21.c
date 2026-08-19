#include "sys.h"
#include <mmio.h>
#include <mmio_constants.h>

unsigned ros_int21(unsigned ah, unsigned a1, unsigned a2, unsigned a3) {
    unsigned rv;

    (void)ah;
    (void)a1;
    (void)a2;
    (void)a3;
    irq_raise(IRQ_SW);
    __asm__("ADD %0, RV0, R0" : "=r"(rv));
    return rv;
}
