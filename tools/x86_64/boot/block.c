/* Modern virtio 1.x PCI block device, split queue, one outstanding request.
 * Coherent x86 DMA uses native static buffers under the identity map. Guest
 * C16 memory is NEVER submitted to the device. Completion is polled with a
 * TSC deadline; the timer ISR remains independent of storage and scheduling.
 * Spec: docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html sections 2/3/4.1/5.2. */
#include "block.h"
#include "pci.h"

#define QSIZE 8
typedef struct { uint64_t address; uint32_t length; uint16_t flags, next; } Desc;
static struct {
    Desc desc[QSIZE];
    struct { uint16_t flags, index, ring[QSIZE], unused; } available;
    struct { uint16_t flags, index; struct { uint32_t id, length; } ring[QSIZE]; } used;
} queue __attribute__((aligned(4096)));
static struct { uint32_t type, reserved; uint64_t sector; } request __attribute__((aligned(16)));
static uint8_t buffer[4096] __attribute__((aligned(4096)));
static volatile uint8_t result;
static volatile uint8_t *common, *device;
static volatile uint16_t *notify;
static uint64_t capacity, reads, writes, flushes;
static uint16_t submitted, completed;
static unsigned busy, broken, readonly;
#ifdef BOOT_TEST
static void block_selfcheck(void);
#endif
_Static_assert(sizeof(Desc) == 16, "virtqueue descriptor");
_Static_assert(__builtin_offsetof(typeof(queue), used) % 4 == 0, "used ring alignment");

static uint16_t c16(unsigned off) { return *(volatile uint16_t *)(common + off); }
static uint32_t c32(unsigned off) { return *(volatile uint32_t *)(common + off); }
static void w16(unsigned off, uint16_t v) { *(volatile uint16_t *)(common + off) = v; }
static void w32(unsigned off, uint32_t v) { *(volatile uint32_t *)(common + off) = v; }
static void w64(unsigned off, uint64_t v) { w32(off, v); w32(off + 4, v >> 32); }
static void barrier(void) { __asm__ volatile("mfence" : : : "memory"); }
static _Noreturn void invalid(const char *why, uint64_t detail) {
    if (common) common[20] |= 128; /* FAILED */
    fail(why, detail);
}

uint64_t block_init(void) {
    int found = pci_find(0x1af4, 0x1042); /* Modern virtio block PCI ID. */
    if (found < 0) fail("modern virtio root disk missing", 0);
    unsigned bdf = found;
    uint64_t bars[6], sizes[6];
    pci_bars(bdf, bars, sizes);
    if (!(pci_read(bdf, 4) & (1U << 20))) fail("PCI capabilities missing", bdf);
    uint64_t addresses[5] = {0}, lengths[5] = {0};
    uint32_t multiplier = 0;
    uint8_t seen[256] = {0};
    for (unsigned cap = pci_read(bdf, 52) & 255; cap;) {
        if (cap < 64 || cap > 252 || (cap & 3) || seen[cap]) fail("PCI capability chain", cap);
        seen[cap] = 1;
        uint32_t header = pci_read(bdf, cap);
        unsigned id = header & 255, next = header >> 8 & 255;
        if (id == 0x11) { /* Disable MSI-X; completion uses polling. */
            pci_write16(bdf, cap + 2, (header >> 16 & ~0x8000U) | 0x4000);
        } else if (id == 5) pci_write16(bdf, cap + 2, header >> 16 & ~1U);
        else if (id == 9) {
            unsigned len = header >> 16 & 255, type = header >> 24;
            if (type >= 1 && type <= 4 && !addresses[type]) {
                if (len < 16 || cap + len > 256 || (type == 2 && len < 20))
                    fail("virtio capability length", cap);
                unsigned bar = pci_read(bdf, cap + 4) & 255;
                if (bar < 6) {
                    uint64_t off = pci_read(bdf, cap + 8), n = pci_read(bdf, cap + 12);
                    if (!bars[bar] || off > sizes[bar] || n > sizes[bar] - off ||
                        off > UINT64_MAX - bars[bar] || n > UINT64_MAX - bars[bar] - off) {
                        serial("VIRTIO_BAR base="); hex(bars[bar]); serial(" size="); hex(sizes[bar]);
                        serial(" offset="); hex(off); serial(" length="); hex(n); serial("\n");
                        fail("virtio capability BAR bounds", cap);
                    }
                    addresses[type] = bars[bar] + off; lengths[type] = n;
                    if (type == 2) multiplier = pci_read(bdf, cap + 16);
                }
            }
        }
        cap = next;
    }
    if (!addresses[1] || lengths[1] < 56 || (addresses[1] & 3) ||
        !addresses[2] || lengths[2] < 2 || !addresses[4] || lengths[4] < 8 || (addresses[4] & 3))
        fail("virtio required MMIO regions", bdf);
    pci_write16(bdf, 4, (pci_read(bdf, 4) | 6 | 0x400) & 65535); /* MMIO, DMA, no INTx. */
    common = device_map(addresses[1], 56); device = device_map(addresses[4], 8);
    common[20] = 0;
    uint64_t deadline = rdtsc() + boot.tsc_hz;
    while (common[20]) if (rdtsc() >= deadline) invalid("virtio reset timeout", bdf);
    common[20] = 1; common[20] = 3; /* ACKNOWLEDGE, DRIVER */
    w32(0, 1); uint32_t high = c32(4);
    w32(0, 0); uint32_t low = c32(4);
    if (!(high & 1) || !(low & (1U << 9))) invalid("virtio VERSION_1/FLUSH required", low);
    readonly = !!(low & (1U << 5));
    w32(8, 0); w32(12, (1U << 9) | (low & (1U << 5)));
    w32(8, 1); w32(12, 1); /* Only VERSION_1, FLUSH and optional RO. */
    common[20] = 11; /* FEATURES_OK */
    if (!(common[20] & 8)) invalid("virtio features rejected", common[20]);
    if (readonly) invalid("root disk is read-only", bdf);
    w16(16, 65535); w16(22, 0);
    if (c16(24) < QSIZE || c16(28)) invalid("virtio queue unavailable", c16(24));
    w16(24, QSIZE); w16(26, 65535);
    memset(&queue, 0, sizeof(queue)); queue.available.flags = 1; /* NO_INTERRUPT */
    w64(32, (uintptr_t)queue.desc); w64(40, (uintptr_t)&queue.available); w64(48, (uintptr_t)&queue.used);
    uint64_t notify_offset = (uint64_t)c16(30) * multiplier;
    if (notify_offset > lengths[2] - 2 || ((addresses[2] + notify_offset) & 1))
        invalid("virtio notify bounds", notify_offset);
    notify = device_map(addresses[2] + notify_offset, 2);
    w16(28, 1);
    /* Configuration generation protects the two 32-bit capacity reads. */
    for (unsigned attempt = 0;; ++attempt) {
        uint8_t generation = common[21];
        uint64_t sectors = *(volatile uint32_t *)device;
        sectors |= (uint64_t)*(volatile uint32_t *)(device + 4) << 32;
        if (generation == common[21]) {
            if (!sectors || sectors > 0x7fffffffULL / 512) invalid("virtio root capacity", sectors);
            capacity = sectors * 512; break;
        }
        if (attempt == 99) invalid("virtio capacity unstable", attempt);
    }
    barrier(); common[20] = 15; /* DRIVER_OK */
    serial("VIRTIO_BLK_OK pci="); hex(bdf); serial(" bytes="); hex(capacity);
    serial(readonly ? " readonly\n" : " writable\n");
#ifdef BOOT_TEST
    block_selfcheck();
#endif
    return capacity;
}

static int submit(unsigned type, uint64_t sector, unsigned bytes) {
    if (busy) invalid("reentrant block request", type);
    if (broken || !common || (common[20] & (64 | 128))) return -1;
    busy = 1;
    request.type = type; request.reserved = 0; request.sector = sector; result = 255;
    queue.desc[0] = (Desc){(uintptr_t)&request, sizeof(request), 1, 1};
    if (bytes) {
        queue.desc[1] = (Desc){(uintptr_t)buffer, bytes, 1 | (type == 0 ? 2 : 0), 2};
        queue.desc[2] = (Desc){(uintptr_t)&result, 1, 2, 0};
    } else queue.desc[1] = (Desc){(uintptr_t)&result, 1, 2, 0};
    queue.available.ring[submitted % QSIZE] = 0;
    barrier(); queue.available.index = ++submitted; barrier(); *notify = 0;
    uint64_t deadline = rdtsc() + 5 * boot.tsc_hz;
    volatile uint16_t *used_index = &queue.used.index;
    while (*used_index == completed) {
        if (rdtsc() >= deadline || (common[20] & (64 | 128))) {
            /* Keep all DMA storage resident after timeout. Never reuse it. */
            broken = 1; busy = 0;
            serial("BLOCK_IO_ERROR timeout_or_reset\n"); return -1;
        }
        __asm__ volatile("pause");
    }
    barrier();
    uint32_t id = queue.used.ring[completed % QSIZE].id;
    uint32_t length = queue.used.ring[completed % QSIZE].length;
    if (*used_index != (uint16_t)(completed + 1) || id != 0)
        invalid("virtio used ring corruption", id);
    ++completed; busy = 0;
    if (result || length != (type == 0 ? bytes + 1 : 1)) {
        serial("BLOCK_IO_ERROR status="); hex(result); serial(" length="); hex(length); serial("\n");
        return -1;
    }
    if (type == 0) ++reads; else if (type == 1) ++writes; else ++flushes;
    return 0;
}

int block_transfer(uint64_t offset, void *data, size_t bytes, unsigned write) {
    if (broken || offset > capacity || bytes > capacity - offset || write > 1 || (write && readonly)) return -1;
    uint8_t *at = data;
    while (bytes) {
        unsigned skip = offset % 512;
        size_t n = bytes < sizeof(buffer) - skip ? bytes : sizeof(buffer) - skip;
        unsigned transfer = (skip + n + 511) & ~511U;
        if (!write || skip || n != transfer)
            if (submit(0, offset / 512, transfer)) return -1;
        if (write) {
            memcpy(buffer + skip, at, n);
            if (submit(1, offset / 512, transfer)) return -1;
        } else memcpy(at, buffer + skip, n);
        at += n; offset += n; bytes -= n;
    }
    return 0;
}
int block_flush(void) { return submit(4, 0, 0); }
void block_report(void) {
    serial("BLOCK_IO_OK reads="); hex(reads); serial(" writes="); hex(writes);
    serial(" flushes="); hex(flushes); serial("\n");
}

#include "block-check.inc"
