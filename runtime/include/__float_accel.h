#ifndef RCC_FLOAT_ACCEL_H
#define RCC_FLOAT_ACCEL_H
#include <limits.h>
#if CHAR_BIT == 16 && !defined(RCC_SOFTWARE_FLOAT)
#ifndef RIPPLE_MMU
#define VM_MATH_PHYSICAL
#define VM_MATH_STACK
#endif
#include <vm_math.h>
#ifndef RIPPLE_MMU
static unsigned __rcc_float_accel(unsigned op, unsigned long long a,
                                  unsigned long long b, unsigned long long *result) {
    volatile VmMathRequest packet;
    unsigned status, enabled;
    packet.version = VM_DEVICE_ABI_VERSION; packet.length = VM_MATH_CELLS;
    packet.operation = op; packet.reserved = 0;
    packet.a = a; packet.b = b; packet.result = 0;
    status = vm_math_stack(&packet);
    if (status == VM_DEVICE_OK) { *result = packet.result; return status; }
    packet.reserved = 0;
    enabled = mmio_read(MMIO_IRQ_ENABLE);
    mmio_write(MMIO_IRQ_ENABLE, 0);
    status = vm_math_submit((volatile unsigned short *)&packet);
    mmio_write(MMIO_IRQ_ENABLE, enabled);
    if (status != VM_DEVICE_OK) return status;
    *result = packet.result;
    return VM_DEVICE_OK;
}
#endif
#define RCC_ACCEL_RETURN(op, a, b) do { \
    unsigned long long accelerated; \
    if (__rcc_float_accel((op), (a), (b), &accelerated) == VM_DEVICE_OK) return accelerated; \
} while (0)
#else
#define RCC_ACCEL_RETURN(op, a, b) do { } while (0)
#endif


/* Floor is short enough to keep local in Riplix. Its caller also has an
 * addressable libc implementation; the scalar service remains available. */
#ifdef RIPPLE_MMU
#define RCC_ACCEL_LOCAL_RETURN(op, a, b) do { } while (0)
#else
#define RCC_ACCEL_LOCAL_RETURN(op, a, b) RCC_ACCEL_RETURN(op, a, b)
#endif

#endif
