#include <vm_bulk.h>
#include <mmio.h>

unsigned short vm_memory_info(unsigned long *ram_cells, unsigned long *code_instructions) {
    volatile unsigned short descriptor[8];
    unsigned long address = (unsigned long)descriptor;
    unsigned short i, status;
    if (mmio_read(MMIO_DEVICE_VERSION) != VM_DEVICE_ABI_VERSION)
        return VM_DEVICE_UNSUPPORTED;
    for (i = 0; i < 8; i++) descriptor[i] = 0;
    descriptor[0] = VM_DEVICE_ABI_VERSION; descriptor[1] = 8;
    mmio_write(MMIO_DEVICE_DESC_BANK, (unsigned short)(address >> 16));
    mmio_write(MMIO_DEVICE_DESC_OFF, (unsigned short)address);
    mmio_write(MMIO_DEVICE_COMMAND, VM_DEVICE_MEMORY_INFO);
    status = mmio_read(MMIO_DEVICE_STATUS);
    if (status == VM_DEVICE_OK) {
        *ram_cells = descriptor[2] | ((unsigned long)descriptor[3] << 16);
        *code_instructions = descriptor[4] | ((unsigned long)descriptor[5] << 16);
    }
    return status;
}

static unsigned short transfer(unsigned short command, unsigned short source0,
    unsigned short source1, unsigned short bank, unsigned short offset,
    unsigned long count, unsigned short format) {
    volatile unsigned short descriptor[12];
    unsigned long address = (unsigned long)descriptor;
    /* Hosts predating DEVICE_* read the reserved status cell as zero. Do not
     * mistake that for a successful transfer which never actually happened. */
    if (mmio_read(MMIO_DEVICE_VERSION) != VM_DEVICE_ABI_VERSION)
        return VM_DEVICE_UNSUPPORTED;
    descriptor[0] = VM_DEVICE_ABI_VERSION; descriptor[1] = 12;
    descriptor[2] = source0; descriptor[3] = source1;
    descriptor[4] = bank; descriptor[5] = offset;
    descriptor[6] = (unsigned short)count; descriptor[7] = (unsigned short)(count >> 16);
    descriptor[8] = format;
    descriptor[9] = 0; descriptor[10] = 0; descriptor[11] = 0;
    mmio_write(MMIO_DEVICE_DESC_BANK, (unsigned short)(address >> 16));
    mmio_write(MMIO_DEVICE_DESC_OFF, (unsigned short)address);
    mmio_write(MMIO_DEVICE_COMMAND, command);
    return mmio_read(MMIO_DEVICE_STATUS);
}
unsigned short vm_bulk_read(unsigned long disk_offset, unsigned short bank,
    unsigned short offset, unsigned long count, unsigned short format) {
    return transfer(VM_DEVICE_STORAGE_READ, (unsigned short)disk_offset,
        (unsigned short)(disk_offset >> 16), bank, offset, count, format);
}
unsigned short vm_bulk_write(unsigned long disk_offset, unsigned short bank,
    unsigned short offset, unsigned long count, unsigned short format) {
    return transfer(VM_DEVICE_STORAGE_WRITE, (unsigned short)disk_offset,
        (unsigned short)(disk_offset >> 16), bank, offset, count, format);
}
unsigned short vm_bulk_copy(unsigned short source_bank, unsigned short source_offset,
    unsigned short bank, unsigned short offset, unsigned long count, unsigned short format) {
    return transfer(VM_DEVICE_MEMORY_COPY, source_bank, source_offset, bank, offset, count, format);
}
unsigned short vm_bulk_fill(unsigned short bank, unsigned short offset,
    unsigned long count, unsigned short value) {
    return transfer(VM_DEVICE_MEMORY_FILL, value, 0, bank, offset, count, VM_BULK_WORDS);
}
