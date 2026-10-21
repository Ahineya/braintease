#ifndef RIPLIX_X86_TERMINAL_SIZE_H
#define RIPLIX_X86_TERMINAL_SIZE_H
#include <stddef.h>
#include <stdint.h>

/* DEVICE_HOST_TERMINAL (11), ABI1. Each descriptor word is a C16 cell;
 * the wire order is version, length, rows, columns. The runtime must validate
 * the guest address before passing a native pointer and mark successful writes
 * dirty. Failures preserve every descriptor cell, just as the shared ABI does.
 * Status values match runtime/include/vm_devices.h without importing its C16
 * MMIO declarations into the freestanding native platform. */
static inline unsigned x64_terminal_size_reply(uint16_t *descriptor, size_t cells,
                                                unsigned rows, unsigned columns) {
    if (!descriptor || cells < 4) return 4; /* VM_DEVICE_BOUNDS */
    if (descriptor[0] != 1) return 2; /* VM_DEVICE_BAD_VERSION */
    if (descriptor[1] != 4 || descriptor[2] || descriptor[3])
        return 3; /* VM_DEVICE_BAD_DESCRIPTOR */
    if (!rows || rows > UINT16_MAX || !columns || columns > UINT16_MAX)
        return 7; /* VM_DEVICE_UNSUPPORTED: no usable console. */
    descriptor[2] = (uint16_t)rows;
    descriptor[3] = (uint16_t)columns;
    return 0;
}
#endif
