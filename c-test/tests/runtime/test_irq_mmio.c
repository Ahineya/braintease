// IRQ MMIO register R/W (no dispatch: never enable a pending bit)
#include <stdio.h>
#include <mmio.h>
#include <mmio_constants.h>

int main() {
    if (irq_get_status() == 0 && irq_get_enable() == 0 && !irq_is_busy() && irq_get_cause() == 0) {
        putchar('Y');
    } else {
        putchar('N');
    }

    irq_set_enable(0x00A5);
    if (irq_get_enable() == 0x00A5) {
        putchar('Y');
    } else {
        putchar('N');
    }
    irq_set_enable(0);
    if (irq_get_enable() == 0) {
        putchar('Y');
    } else {
        putchar('N');
    }

    irq_raise(IRQ_SW);
    if (irq_get_status() == IRQ_SW) {
        putchar('Y');
    } else {
        putchar('N');
    }
    irq_raise(0x0004);
    if (irq_get_status() == (IRQ_SW | 0x0004)) {
        putchar('Y');
    } else {
        putchar('N');
    }

    irq_set_vector(2, 0x1234);
    if (mmio_read(MMIO_IRQ_VECTOR_BANK) == 2 && mmio_read(MMIO_IRQ_VECTOR_OFF) == 0x1234) {
        putchar('Y');
    } else {
        putchar('N');
    }

    mmio_write(MMIO_IRQ_CAUSE, 99);
    if (irq_get_cause() == 0) {
        putchar('Y');
    } else {
        putchar('N');
    }

    irq_ack();
    if (!irq_is_busy() && irq_get_status() == (IRQ_SW | 0x0004)) {
        putchar('Y');
    } else {
        putchar('N');
    }

    if (mmio_read(27) == 0 && mmio_read(31) == 0) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
