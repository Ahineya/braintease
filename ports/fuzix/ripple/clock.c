/* SPDX-License-Identifier: GPL-2.0-only */
#include <kernel.h>
#include <kdata.h>
#include <vm_devices.h>
#include "clock-abi.h"
#define VM_MATH_PHYSICAL
#include <vm_math.h>

/* The upstream implementation remains authoritative for existing selectors;
 * only its name is changed when compiling syscall_proc.c on these ports. */
extern arg_t ripple_legacy_time(void);
arg_t _time(void) {
    VmHostTime sample;
    struct ripple_clock_sample result;
    unsigned status;
    irqflags_t irq;
    if (udata.u_argn1 == RIPPLE_TIME_MATH) {
        unsigned short packet[VM_MATH_CELLS];
        if (!valaddr_w((void *)udata.u_argn, sizeof(packet), sizeof(packet))) return -1;
        if (uget((void *)udata.u_argn, packet, sizeof(packet))) return -1;
        if (packet[0] != VM_DEVICE_ABI_VERSION) return VM_DEVICE_BAD_VERSION;
        if (packet[1] != VM_MATH_CELLS) return VM_DEVICE_BAD_DESCRIPTOR;
        irq = di(); status = vm_math_submit(packet); irqrestore(irq);
        if (status != VM_DEVICE_OK) return status;
        return uput(packet, (void *)udata.u_argn, sizeof(packet));
    }
    if (udata.u_argn1 != RIPPLE_TIME_MONOTONIC) return ripple_legacy_time();
    if (!valaddr_w((void *)udata.u_argn, sizeof(result), sizeof(result))) return -1;
    irq = di();status = vm_host_monotonic(&sample);irqrestore(irq);
    if (status != VM_DEVICE_OK) { udata.u_error = EIO; return -1; }
    result.seconds_low=sample.seconds_low; result.seconds_high=sample.seconds_high;
    result.nanoseconds=sample.nanoseconds; result.resolution_ns=sample.resolution_ns;
    return uput(&result, (void *)udata.u_argn, sizeof(result));
}
