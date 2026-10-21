/* NVMe 1.x NVM command set, firmware-assigned PCI BAR, one namespace, one
 * polled I/O queue. Native coherent DMA buffers never contain guest pointers.
 * Reference: NVM Express 1.4c sections 3, 4, 5 (Identify/Create Queue), 6.
 * First integration targets disposable raw root images, not the Ubuntu disk. */
#include "block.h"
#include "pci.h"
#include "dma.h"
#ifdef BOOT_ROOT_PARTITION
#include "root-window.h"
#include "root-identity.h"
static RootWindow root_window;
#endif
#ifdef BOOT_NVME_PROBE
#include "gpt.h"
#include "nvme.h"
#endif
#define QSIZE 8
static struct Queue {
    uint32_t sq[1024];
    volatile uint32_t cq[1024];
    unsigned tail, head, phase, id;
} admin __attribute__((aligned(4096))), io __attribute__((aligned(4096)));
static uint8_t buffer[4096] __attribute__((aligned(4096)));
static volatile uint32_t *registers;
static uint64_t capacity, namespace_bytes, reads, writes, flushes;
static unsigned stride, nsid, sector_size, busy, broken, readonly;
static uint16_t next_cid;
#ifdef BOOT_NVME_PROBE
static char model[41], probe_status[192] = "NVMe probe pending; RAM root; disk writes disabled.";
static unsigned controller_bdf;
#endif
#if defined(BOOT_TEST) && !defined(BOOT_NVME_PROBE)
static void block_selfcheck(void);
#endif
static void barrier(void) { __asm__ volatile("mfence" : : : "memory"); }
static _Noreturn void invalid(const char *why, uint64_t detail) { fail(why, detail); }
static uint32_t reg(unsigned off) { return registers[off / 4]; }
static void putreg(unsigned off, uint32_t value) { registers[off / 4] = value; }
static void putreg64(unsigned off, uint64_t value) { putreg(off, value); putreg(off + 4, value >> 32); }
static uint64_t le64(unsigned off) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)buffer[off + i] << (8 * i);
    return value;
}
static void ready(unsigned value, uint64_t interval) {
    uint64_t end = rdtsc() + interval;
    for (;;) {
        uint32_t state = reg(0x1c);
        if (state & 2) invalid("NVMe controller fatal", state);
        if ((state & 1) == value) return;
        if (rdtsc() >= end) invalid("NVMe ready timeout", state);
        __asm__ volatile("pause");
    }
}
static int command(struct Queue *q, uint32_t cmd[16]) {
#ifdef BOOT_ROOT_PARTITION
    /* Final write boundary, independent of FILE/RMW callers. Discovery can
     * read the namespace but cannot write/flush before identity validation. */
    if (q == &io) {
        unsigned opcode = cmd[0] & 255;
        uint64_t lba = cmd[10] | (uint64_t)cmd[11] << 32;
        uint64_t count = (uint64_t)((cmd[12] & 65535) + 1) * sector_size;
        if ((opcode != 0 && opcode != 1 && opcode != 2) ||
            (opcode == 0 && !root_window.ready) ||
            (opcode == 1 && (lba > UINT64_MAX / sector_size ||
                             !root_window_contains(&root_window, lba * sector_size, count)))) return -1;
    }
#endif
#ifdef BOOT_NVME_PROBE
    /* Defense at the final doorbell boundary. There is no Write, Flush,
     * Format, DSM, Sanitize or namespace-management path in a probe build. */
    unsigned opcode = cmd[0] & 255;
    if (q == &io ? opcode != 2 :
        !(opcode == 6 || opcode == 5 || opcode == 1 ||
          (opcode == 9 && cmd[10] == 7 && !cmd[11]))) return -1;
#endif
    if (busy) invalid("reentrant block request", q->id);
    if (broken) return -1;
    busy = 1;
    unsigned cid = ++next_cid;
    cmd[0] = (cmd[0] & 65535) | cid << 16;
    memcpy(q->sq + q->tail * 16, cmd, 64);
    q->tail = (q->tail + 1) % QSIZE;
    barrier(); putreg(0x1000 + 2 * q->id * stride, q->tail);
    uint64_t end = rdtsc() + 5 * boot.tsc_hz;
    volatile uint32_t *entry = q->cq + q->head * 4;
    while ((entry[3] >> 16 & 1) != q->phase) {
        if (rdtsc() >= end || (reg(0x1c) & 2)) {
            /* No reset/retry: DMA buffers remain resident and never reused. */
            broken = 1; busy = 0;
            serial("BLOCK_IO_ERROR nvme_timeout_or_fatal\n"); return -1;
        }
        __asm__ volatile("pause");
    }
    barrier();
    uint32_t completion = entry[3], sq = entry[2];
    if ((completion & 65535) != cid || sq >> 16 != q->id || (sq & 65535) != q->tail)
        invalid("NVMe completion queue corruption", completion);
    if (++q->head == QSIZE) { q->head = 0; q->phase ^= 1; }
    barrier(); putreg(0x1000 + (2 * q->id + 1) * stride, q->head);
    busy = 0;
    if (completion >> 17) {
        serial("BLOCK_IO_ERROR nvme_status="); hex(completion >> 17); serial("\n"); return -1;
    }
    return 0;
}
static void identify(unsigned cns, unsigned namespace) {
    uint32_t cmd[16] = {0}; cmd[0] = 6; cmd[1] = namespace;
    cmd[6] = (uintptr_t)buffer; cmd[7] = (uintptr_t)buffer >> 32; cmd[10] = cns;
    memset(buffer, 0, sizeof(buffer));
    if (command(&admin, cmd)) invalid("NVMe identify failed", cns);
}
static uint64_t initialize(unsigned bdf) {
    /* The caller checked DMA handoff first. Keep bus mastering off through
     * BAR inspection and controller reset; enable only for our own queues. */
    uint16_t pci_command = pci_read(bdf, 4);
    pci_write16(bdf, 4, (pci_command | 2 | 0x400) & ~4U);
    uint64_t bars[6], sizes[6]; pci_bars(bdf, bars, sizes);
    if (!bars[0] || sizes[0] < 0x1010 || (bars[0] & 4095)) invalid("NVMe register BAR", bars[0]);
    unsigned pcie = 0;
    /* Polling only. Disable inherited MSI/MSI-X and legacy INTx. */
    if (pci_read(bdf, 4) & (1 << 20)) {
        uint8_t seen[256] = {0};
        for (unsigned cap = pci_read(bdf, 52) & 255; cap;) {
            if (cap < 64 || cap > 252 || (cap & 3) || seen[cap]) invalid("PCI capability chain", cap);
            seen[cap] = 1; uint32_t header = pci_read(bdf, cap);
            if ((header & 255) == 0x11) pci_write16(bdf, cap + 2, (header >> 16 & ~0x8000U) | 0x4000);
            else if ((header & 255) == 5) pci_write16(bdf, cap + 2, header >> 16 & ~1U);
            else if ((header & 255) == 0x10) {
                if (pcie++ || cap > 244) invalid("short/duplicate PCIe capability", cap);
                /* Require coherent, ordered DMA even if firmware allowed the
                 * endpoint to use PCIe No Snoop / Relaxed Ordering. */
                uint16_t control = pci_read(bdf, cap + 8);
                pci_write16(bdf, cap + 8, control & ~0x8810U);
                if (pci_read(bdf, cap + 8) & 0x810) invalid("PCIe DMA attributes readback", cap);
                serial("NVME_PCIE_COHERENT no_snoop=off relaxed_order=off\n");
            }
            cap = header >> 8 & 255;
        }
    }
    pci_write16(bdf, 4, (pci_read(bdf, 4) | 2 | 0x400) & ~4U);
    if (!pcie) invalid("NVMe PCIe capability missing", bdf);
    registers = device_map(bars[0], 4096);
    uint64_t cap = reg(0) | (uint64_t)reg(4) << 32;
    if ((cap & 65535) + 1 < QSIZE || !(cap & (1ULL << 37)) || (cap >> 48 & 15))
        invalid("NVMe unsupported capabilities", cap);
    stride = 4U << (cap >> 32 & 15);
    if (0x1000ULL + 3 * stride + 4 > sizes[0]) invalid("NVMe doorbell BAR bounds", stride);
    registers = device_map(bars[0], 0x1000 + 3 * stride + 4);
    uint64_t timeout = ((cap >> 24 & 255) + 1) * (boot.tsc_hz / 2);
    if (timeout > 30 * boot.tsc_hz) timeout = 30 * boot.tsc_hz;
    /* After ExitBootServices, with only our CPU active. Never called from
     * Linux; the physical probe is a distinct compile-time read-only profile. */
    putreg(0xc, UINT32_MAX); /* INTMS */
    putreg(0x14, reg(0x14) & ~1U); ready(0, timeout);
    memset(&admin, 0, sizeof(admin)); memset(&io, 0, sizeof(io));
    admin.phase = io.phase = 1; io.id = 1;
    putreg(0x24, (QSIZE - 1) | (QSIZE - 1) << 16);
    putreg64(0x28, (uintptr_t)admin.sq); putreg64(0x30, (uintptr_t)admin.cq);
    pci_write16(bdf, 4, (pci_read(bdf, 4) | 6 | 0x400) & 65535);
    barrier(); putreg(0x14, 1 | 6 << 16 | 4 << 20); ready(1, timeout);
    identify(1, 0);
#ifdef BOOT_NVME_PROBE
    for (unsigned i = 0; i < 40; ++i) model[i] = buffer[24 + i] >= 32 && buffer[24 + i] <= 126 ? buffer[24 + i] : '?';
    for (unsigned i = 40; i && model[i - 1] == ' '; --i) model[i - 1] = 0;
#endif
    if ((buffer[512] & 15) > 6 || buffer[512] >> 4 < 6 ||
        (buffer[513] & 15) > 4 || buffer[513] >> 4 < 4)
        invalid("NVMe queue entry sizes", buffer[512]);
    /* Every data request fits one aligned 4K PRP page. MDTS=0 is unlimited;
     * every other legal MDTS permits at least our 4K transfer. */
    identify(2, 0); nsid = (unsigned)le64(0);
    if (!nsid || nsid == UINT32_MAX || le64(0) >> 32 || le64(8))
        invalid("NVMe requires one active namespace", nsid);
    identify(0, nsid);
    unsigned format = buffer[26] & 15;
    if ((buffer[26] & ~15U) || format > buffer[25] || (buffer[29] & 7))
        invalid("NVMe unsupported namespace format", buffer[26]);
    unsigned entry = 128 + format * 4, shift = buffer[entry + 2];
    if (buffer[entry] || buffer[entry + 1] || shift < 9 || shift > 12)
        invalid("NVMe LBA size/metadata", shift);
    sector_size = 1U << shift;
    uint64_t sectors = le64(0);
    if (!sectors || !le64(8) || sectors > UINT64_MAX / sector_size)
        invalid("NVMe namespace capacity", sectors);
    capacity = namespace_bytes = sectors * sector_size;
    readonly = buffer[99] & 1;
    serial("NVME_IDENTIFY_OK pci="); hex(bdf); serial(" nsid="); hex(nsid);
    serial(" bytes="); hex(capacity); serial(" lba_bytes="); hex(sector_size); serial("\n");
#ifdef BOOT_NVME_PROBE
    readonly = 1;
#else
    if (readonly) invalid("root disk is read-only", bdf);
    /* The current C16 FILE/root format exposes a bounded raw image, not an
     * entire laptop drive. A later partition adapter will bound this window. */
#ifndef BOOT_ROOT_PARTITION
    if (capacity > 0x7fffffff) invalid("NVMe raw root exceeds C16 stream limit", capacity);
#endif
#endif
    uint32_t cmd[16] = {0}; cmd[0] = 9; cmd[10] = 7; /* Set Number of Queues: one pair. */
    if (command(&admin, cmd)) invalid("NVMe queue count", 0);
    memset(cmd, 0, sizeof(cmd)); cmd[0] = 5; /* Create I/O CQ, physically contiguous, no IRQ. */
    cmd[6] = (uintptr_t)io.cq; cmd[7] = (uintptr_t)io.cq >> 32;
    cmd[10] = 1 | (QSIZE - 1) << 16; cmd[11] = 1;
    if (command(&admin, cmd)) invalid("NVMe create completion queue", 1);
    memset(cmd, 0, sizeof(cmd)); cmd[0] = 1; /* Create I/O SQ, associated CQ=1. */
    cmd[6] = (uintptr_t)io.sq; cmd[7] = (uintptr_t)io.sq >> 32;
    cmd[10] = 1 | (QSIZE - 1) << 16; cmd[11] = 1 | 1 << 16;
    if (command(&admin, cmd)) invalid("NVMe create submission queue", 1);
    serial("NVME_BLK_OK polled_native_dma queue_depth=8\n");
    return capacity;
}
static int namespace_transfer(uint64_t offset, void *data, size_t bytes, unsigned write);
#ifdef BOOT_ROOT_PARTITION
static int partition_read(void *ctx, uint64_t offset, void *data, size_t bytes) {
    (void)ctx; return namespace_transfer(offset, data, bytes, 0);
}
#endif
uint64_t block_init(void) {
#ifdef BOOT_NVME_PROBE
    invalid("persistent root unavailable in read-only probe", 0);
#else
    int found = pci_find_class(0x010802);
    if (found < 0) invalid("NVMe root disk missing", 0);
    const char *error = dma_identity_check((unsigned)found);
    if (error) invalid(error, found);
    initialize((unsigned)found);
#ifdef BOOT_ROOT_PARTITION
#ifdef BOOT_TEST
    unsigned before_cid = next_cid;
    uint32_t denied[16] = {0};
    denied[0] = 1; denied[1] = nsid;
    if (!command(&io, denied)) invalid("partition write before selection", 0);
    denied[0] = 0;
    if (!command(&io, denied) || next_cid != before_cid) invalid("partition flush before selection", 0);
#endif
    error = root_window_open(partition_read, 0, namespace_bytes, sector_size, &root_identity, &root_window);
    if (error) invalid(error, writes);
    capacity = root_window.bytes;
    serial("ROOT_PARTITION_OK offset="); hex(root_window.offset); serial(" bytes="); hex(capacity);
    serial(" identity=exact write_window=bounded\n");
#ifdef BOOT_TEST
    before_cid = next_cid;
    denied[0] = 1;
    uint64_t lba = root_window.offset / sector_size - 1;
    denied[10] = lba; denied[11] = lba >> 32;
    if (!command(&io, denied)) invalid("partition write below window", lba);
    lba = (root_window.offset + root_window.bytes) / sector_size;
    denied[10] = lba; denied[11] = lba >> 32;
    if (!command(&io, denied) || next_cid != before_cid || writes || flushes)
        invalid("partition write above window", lba);
    serial("ROOT_PARTITION_GUARD_OK writes_flush_blocked_before_doorbell\n");
#endif
#endif
#ifdef BOOT_TEST
    block_selfcheck();
#endif
    return capacity;
#endif
}
static int submit(unsigned type, uint64_t sector, unsigned bytes) {
#ifdef BOOT_NVME_PROBE
    if (type != 0) return -1;
#endif
    uint32_t cmd[16] = {0}; cmd[0] = type == 0 ? 2 : type == 1 ? 1 : 0; cmd[1] = nsid;
    if (bytes) {
        if (bytes > sizeof(buffer) || bytes % sector_size) invalid("NVMe transfer alignment", bytes);
        cmd[6] = (uintptr_t)buffer; cmd[7] = (uintptr_t)buffer >> 32;
        cmd[10] = sector; cmd[11] = sector >> 32; cmd[12] = bytes / sector_size - 1;
    }
    if (command(&io, cmd)) return -1;
    if (type == 0) ++reads; else if (type == 1) ++writes; else ++flushes;
    return 0;
}
static int namespace_transfer(uint64_t offset, void *data, size_t bytes, unsigned write) {
    if (broken || offset > namespace_bytes || bytes > namespace_bytes - offset || write > 1 || (write && readonly)) return -1;
    uint8_t *at = data;
    while (bytes) {
        unsigned skip = offset % sector_size;
        size_t n = bytes < sizeof(buffer) - skip ? bytes : sizeof(buffer) - skip;
        unsigned transfer = (skip + n + sector_size - 1) & ~(sector_size - 1);
        if (!write || skip || n != transfer)
            if (submit(0, offset / sector_size, transfer)) return -1;
        if (write) {
            memcpy(buffer + skip, at, n);
            if (submit(1, offset / sector_size, transfer)) return -1;
        } else memcpy(at, buffer + skip, n);
        at += n; offset += n; bytes -= n;
    }
    return 0;
}
int block_transfer(uint64_t offset, void *data, size_t bytes, unsigned write) {
    if (offset > capacity || bytes > capacity - offset) return -1;
#ifdef BOOT_ROOT_PARTITION
    uint64_t translated;
    if (root_window_translate(&root_window, offset, bytes, &translated)) return -1;
    offset = translated;
#endif
    return namespace_transfer(offset, data, bytes, write);
}
int block_flush(void) {
#ifdef BOOT_NVME_PROBE
    return -1;
#else
    return submit(4, 0, 0);
#endif
}
void block_report(void) {
    serial("BLOCK_IO_OK reads="); hex(reads); serial(" writes="); hex(writes);
    serial(" flushes="); hex(flushes); serial("\n");
}
#ifndef BOOT_NVME_PROBE
#include "block-check.inc"
#endif

#ifdef BOOT_NVME_PROBE
static void text(const char *s) { while (*s) console_write(*s++); }
static void number(uint64_t n) {
    char digits[21]; unsigned count = 0;
    do { digits[count++] = '0' + n % 10; n /= 10; } while (n);
    while (count) console_write(digits[--count]);
}
static char *append(char *out, const char *s) {
    while (*s && out < probe_status + sizeof(probe_status) - 1) *out++ = *s++;
    *out = 0; return out;
}
static void summary(const char *state, const char *detail) {
    char *out = append(probe_status, state);
    out = append(out, detail); append(out, ". RAM root; disk writes disabled.");
    screen_status(probe_status);
}
const char *nvme_probe_status(void) { return probe_status; }
static int probe_read(void *ctx, uint64_t offset, void *data, size_t bytes) {
    (void)ctx; return block_transfer(offset, data, bytes, 0);
}
void nvme_probe(void) {
    text("\nNVMe read-only probe: disk writes disabled\n");
    int found = pci_find_class(0x010802);
    if (found < 0) {
        serial("NVME_PROBE_SKIPPED missing_controller\n");
        summary("NVMe skipped: ", "no controller found"); return;
    }
    const char *error = dma_identity_check((unsigned)found);
    if (error) {
        text("NVMe probe skipped: "); text(error); text("\n");
        serial("NVME_PROBE_SKIPPED "); serial(error); serial("\n");
        summary("NVMe skipped: ", error); return;
    }
    controller_bdf = found; initialize(controller_bdf);
    /* Verify write/flush rejection before any media reads in this build. */
    uint8_t sentinel = 0;
    if (!block_transfer(0, &sentinel, 1, 1) || !block_flush() || writes || flushes)
        invalid("read-only NVMe guard", writes);
#ifdef BOOT_TEST
    unsigned before_cid = next_cid;
    uint32_t forbidden[16] = {0};
    forbidden[0] = 1;
    if (!command(&io, forbidden)) invalid("NVMe Write bypassed guard", 1);
    forbidden[0] = 0;
    if (!command(&io, forbidden)) invalid("NVMe Flush bypassed guard", 0);
    forbidden[0] = 0x80;
    if (!command(&admin, forbidden) || next_cid != before_cid) invalid("NVMe Format bypassed guard", 0x80);
    serial("NVME_READONLY_GUARD_OK write_flush_format_blocked_before_doorbell\n");
#endif
    text("NVMe: "); text(model); text("; "); number(capacity / (1024 * 1024));
    text(" MiB; LBA "); number(sector_size); text(" bytes\n");
    static GptInfo partitions;
    error = gpt_discover(probe_read, 0, capacity, sector_size, &partitions);
    if (error) {
        text("GPT probe stopped: "); text(error); text("\n");
        serial("NVME_PROBE_READONLY_FAILED "); serial(error); serial("\n");
        summary("NVMe/GPT stopped: ", error);
    } else {
        text("GPT: primary + backup headers/arrays verified; "); number(partitions.count); text(" partitions\n");
        for (unsigned i = 0; i < partitions.count; ++i) {
            const GptPartition *p = &partitions.partitions[i];
            text("  p"); number(p->index); text(" LBA "); number(p->first); text(".."); number(p->last);
            text("  "); text(p->name); text("\n");
            serial("GPT_PARTITION index="); hex(p->index); serial(" first="); hex(p->first);
            serial(" last="); hex(p->last); serial("\n");
        }
        serial("NVME_PROBE_READONLY_OK partitions="); hex(partitions.count); serial("\n");
        summary("NVMe/GPT verified: ", model);
    }
    if (writes || flushes) invalid("read-only NVMe I/O counters", writes);
    serial("NVME_PROBE_IO reads="); hex(reads); serial(" writes=0 flushes=0\n");
    /* Leave no autonomous DMA running during the RAM-root shell. */
    putreg(0x14, reg(0x14) & ~1U); ready(0, 30 * boot.tsc_hz);
    pci_write16(controller_bdf, 4, pci_read(controller_bdf, 4) & ~4U);
    serial("NVME_PROBE_QUIESCED bus_master=off\n");
    console_flush();
}
#endif
