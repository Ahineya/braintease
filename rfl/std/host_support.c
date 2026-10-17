/* Compile the shared guest DEVICE ABI helpers for either RFL target. Native
 * execution still uses the guest mailbox contract, not platform time bindings. */
#define RIPPLE_MMIO_NO_INLINE 1
#include "../../runtime/vm_devices.c"

unsigned short rfl_host_clock_read(unsigned short calendar, unsigned long *words) {
    VmHostTime sample;
    unsigned short status = calendar ? vm_host_realtime(&sample) : vm_host_monotonic(&sample);
    if (status == VM_DEVICE_OK) {
        words[0] = sample.seconds_low;
        words[1] = sample.seconds_high;
        words[2] = sample.nanoseconds;
        words[3] = sample.resolution_ns;
    }
    return status;
}

unsigned short rfl_host_idle(unsigned short milliseconds) {
    return vm_host_idle(milliseconds);
}
