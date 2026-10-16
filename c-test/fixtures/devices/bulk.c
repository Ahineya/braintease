/* Shared C helpers exercise real compiler pointers, stack descriptors, RAM,
 * storage cache coherence, and a tail call into uploaded Harvard code. */
#include <vm_bulk.h>
#include <mmio.h>
extern unsigned bulk_call(unsigned bank, unsigned offset);
static unsigned short buffer[8];
static unsigned short code[8] = {14, 5, 1234, 0, 20, 0, 4, 3};
static void check(int ok) { mmio_write(MMIO_TTY_OUT, ok ? 'Y' : 'N'); }
int main(void) {
    unsigned short local[8];
    unsigned long g = (unsigned long)buffer, s = (unsigned long)local;
    unsigned long c = (unsigned long)code;
    unsigned gb = (unsigned)(g >> 16), go = (unsigned)g;
    unsigned sb = (unsigned)(s >> 16), so = (unsigned)s;
    check(vm_bulk_fill(gb, go, 8, 0xabcdU) == VM_DEVICE_OK && buffer[7] == 0xabcdU);
    check(vm_bulk_copy(gb, go, sb, so, 8, VM_BULK_WORDS) == VM_DEVICE_OK && local[0] == 0xabcdU && local[7] == 0xabcdU);
    check(vm_bulk_write(65534UL, gb, go, 8, VM_BULK_WORDS) == VM_DEVICE_OK);
    check(storage_read_at(0, 32767) == 0xabcdU && storage_read_at(1, 0) == 0xabcdU);
    storage_write_at(1, 0, 0x1234U);
    check(vm_bulk_read(65534UL, sb, so, 8, VM_BULK_WORDS) == VM_DEVICE_OK && local[0] == 0xabcdU && local[1] == 0x1234U);
    check(vm_bulk_read(65535UL, gb, go, 3, VM_BULK_OCTETS) == VM_DEVICE_OK && buffer[0] == 0xabU && buffer[1] == 0x34U && buffer[2] == 0x12U);
    check(vm_bulk_copy((unsigned)(c >> 16), (unsigned)c, 12, 0, 2, VM_BULK_CODE_WORDS) == VM_DEVICE_OK);
    check(bulk_call(12, 0) == 1234U);
    mmio_write(MMIO_TTY_OUT, '\n');
    return 0;
}
