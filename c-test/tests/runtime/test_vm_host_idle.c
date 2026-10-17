#include <stdio.h>
#include <vm_devices.h>

/* Exercise the archived guest helpers, including their normal inline MMIO
 * path. Real elapsed time is observed, never inferred from the request. */
int main(void) {
    VmDeviceCaps caps;
    VmClock before = {0}, after = {0};
    VmHostTime start = {0}, end = {0};
    unsigned short first, last;
    unsigned long elapsed;

    if (vm_devices_caps(&caps) == VM_DEVICE_OK &&
        (caps.bits & VM_CAP_HOST_IDLE)) putchar('Y'); else putchar('N');
    if (vm_get_time(&before) == VM_DEVICE_OK &&
        vm_host_idle(0) == VM_DEVICE_OK) putchar('Y'); else putchar('N');
    if (vm_host_idle(11) == VM_DEVICE_BAD_DESCRIPTOR &&
        vm_host_idle(65535U) == VM_DEVICE_BAD_DESCRIPTOR) putchar('Y'); else putchar('N');

    first = vm_host_monotonic(&start);
    if (vm_host_idle(2) == VM_DEVICE_OK) putchar('Y'); else putchar('N');
    last = vm_host_monotonic(&end);
    elapsed = (end.seconds_low - start.seconds_low) * 1000UL +
              end.nanoseconds / 1000000UL - start.nanoseconds / 1000000UL;
    if (first == VM_DEVICE_OK && last == VM_DEVICE_OK && elapsed >= 2)
        putchar('Y'); else putchar('N');
    if (vm_get_time(&after) == VM_DEVICE_OK && before.tick == after.tick &&
        before.ms == after.ms && before.hz == after.hz) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
