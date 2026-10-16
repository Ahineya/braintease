#ifndef VM_BULK_H
#define VM_BULK_H
#include <vm_devices.h>

#define VM_CAP_STORAGE_BULK 0x0200U
#define VM_CAP_MEMORY_BULK 0x0400U
#define VM_CAP_CODE_BULK 0x0800U
#define VM_DEVICE_STORAGE_READ 0x0300U
#define VM_DEVICE_STORAGE_WRITE 0x0301U
#define VM_DEVICE_MEMORY_COPY 0x0302U
#define VM_DEVICE_MEMORY_FILL 0x0303U
#define VM_BULK_WORDS 0
#define VM_BULK_OCTETS 1
#define VM_BULK_CODE_WORDS 2
#define VM_BULK_CODE_OCTETS 3
#define VM_BULK_MAX_CELLS 32768U
#define VM_BULK_MAX_INSTRUCTIONS 8192U

/* Positional, synchronous operations; the legacy storage cursor is unchanged.
 * Count is in destination cells, or instructions for CODE formats. Disk offsets
 * are octets. Failures leave RAM/code/disk contents unchanged. Disk writes
 * update the shared cache; storage_commit_all() remains the durability barrier.
 *
 * These are raw architectural bank/offset pairs, not tagged C pointers. A C
 * pointer cast to unsigned long resolves GP/SB tags; split that result for RAM.
 * Callers must serialize DEVICE_* mailbox access (including interrupt handlers).
 * Descriptors live on the calling stack until submission returns. */
unsigned short vm_bulk_read(unsigned long disk_offset, unsigned short bank,
    unsigned short offset, unsigned long count, unsigned short format);
unsigned short vm_bulk_write(unsigned long disk_offset, unsigned short bank,
    unsigned short offset, unsigned long count, unsigned short format);
unsigned short vm_bulk_copy(unsigned short source_bank, unsigned short source_offset,
    unsigned short bank, unsigned short offset, unsigned long count, unsigned short format);
unsigned short vm_bulk_fill(unsigned short bank, unsigned short offset,
    unsigned long count, unsigned short value);
#endif
