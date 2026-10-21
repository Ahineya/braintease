/* Boot-CPU local APIC timer, with explicit legacy PIC/PIT regression mode.
 * Interrupts only touch native counters: never call into the C16 kernel or
 * change memory backing, continuation state, locks or the current process. */
#include "kernel.h"
#include "acpi.h"

static volatile uint64_t ticks;
static volatile unsigned spurious;
static unsigned use_apic, x2apic;
static volatile uint32_t *lapic;
extern uint8_t irq_stack[16384];

uint64_t timer_ticks(void) { return ticks; }

static uint64_t msr_read(unsigned number) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(number));
    return low | (uint64_t)high << 32;
}
static void msr_write(unsigned number, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(number), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)) : "memory");
}
static uint32_t apic_read(unsigned offset) {
    return x2apic ? (uint32_t)msr_read(0x800 + offset / 16) : lapic[offset / 4];
}
static void apic_write(unsigned offset, uint32_t value) {
    if (x2apic) msr_write(0x800 + offset / 16, value);
    else {
        lapic[offset / 4] = value;
        (void)lapic[0x20 / 4]; /* Complete the posted MMIO write. */
    }
}

void irq_dispatch(const uint64_t *frame) {
    if ((uintptr_t)frame < (uintptr_t)irq_stack ||
        (uintptr_t)(frame + 7) > (uintptr_t)(irq_stack + sizeof(irq_stack)))
        fail("IRQ outside IST2", (uintptr_t)frame);
    unsigned vector = frame[0];
    if (use_apic) {
        /* APIC spurious interrupts have no ISR bit, hence no EOI. */
        if (vector == 255) { ++spurious; return; }
        if (vector != 32) fail("unexpected APIC vector", vector);
        ++ticks;
        apic_write(0xb0, 0);
        return;
    }
    /* A disappearing request produces IRQ7/15 without an in-service bit.
     * A spurious slave request still requires EOI on the master's cascade. */
    if (vector == 39 || vector == 47) {
        uint16_t port = vector == 39 ? 0x20 : 0xa0;
        out8(port, 0x0b);
        if (!(in8(port) & 0x80)) {
            if (vector == 47) out8(0x20, 0x20);
            ++spurious;
            return;
        }
    }
    if (vector >= 40) out8(0xa0, 0x20);
    out8(0x20, 0x20);
    if (vector != 32) fail("unexpected unmasked PIC interrupt", vector);
    ++ticks;
}

static void pic_write(uint16_t port, uint8_t value) {
    out8(port, value);
    /* Traditional I/O recovery delay, including on emulated PC hardware. */
    out8(0x80, 0);
}

static void pic_init(void) {
    pic_write(0x21, 0xff); pic_write(0xa1, 0xff);
    pic_write(0x20, 0x11); pic_write(0xa0, 0x11);
    pic_write(0x21, 32); pic_write(0xa1, 40);
    pic_write(0x21, 4); pic_write(0xa1, 2);
    pic_write(0x21, 1); pic_write(0xa1, 1);
    pic_write(0x21, 0xff); pic_write(0xa1, 0xff);
    const unsigned divisor = 11932; /* 1,193,182 Hz / 11932 = 99.9985 Hz. */
    out8(0x43, 0x34); /* Channel 0, low/high bytes, mode 2, binary. */
    out8(0x40, divisor & 255); out8(0x40, divisor >> 8);
    pic_write(0x21, 0xfe); /* Only the timer; keyboard/UART remain polled. */
    if (in8(0x21) != 0xfe || in8(0xa1) != 0xff) fail("PIC masks", in8(0x21));
}

static uint32_t io_read(volatile uint32_t *io, unsigned reg) {
    io[0] = reg;
    __asm__ volatile("" : : : "memory");
    return io[4];
}
static void io_write(volatile uint32_t *io, unsigned reg, uint32_t value) {
    io[0] = reg;
    __asm__ volatile("" : : : "memory");
    io[4] = value;
    __asm__ volatile("" : : : "memory");
}
static void apic_init(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(d & (1 << 9))) fail("CPU lacks local APIC", d);
    uint64_t base = msr_read(0x1b);
    if (!(base & (1 << 8))) fail("entry CPU is not BSP", base);
    if ((base & 0xfffff000ULL) != acpi.lapic || acpi.lapic >= (1ULL << 32))
        fail("MADT/MSR APIC address mismatch", base);
    /* Preserve an existing x2APIC mode, and prefer it on capable real CPUs.
     * A normal xAPIC -> x2APIC transition never clears global APIC enable. */
    x2apic = !!(c & (1 << 21));
    if ((base & (1 << 10)) && !x2apic) fail("inconsistent firmware x2APIC", base);
    if (!x2apic) lapic = device_map(acpi.lapic, 4096);
    if (!(base & (1 << 11))) msr_write(0x1b, base | (1 << 11));
    msr_write(0x1b, base | (1 << 11) | (x2apic ? 1 << 10 : 0));
    use_apic = 1;
    uint32_t id = apic_read(0x20);
    if (!x2apic) id >>= 24;
    unsigned found = 0;
    for (unsigned i = 0; i < acpi.cpu_count; ++i) if (acpi.cpu_ids[i] == id) found = 1;
    if (!found) fail("BSP missing from enabled MADT CPUs", id);
    unsigned max_lvt = apic_read(0x30) >> 16 & 255;
    if (max_lvt < 3 || max_lvt > 6) fail("unsupported LAPIC LVT count", max_lvt);
    apic_write(0xf0, 0x1ff); /* Software enable, spurious vector 255. */
    apic_write(0x80, 0);    /* Accept all priorities. */
    apic_write(0x320, 1 << 16); apic_write(0x380, 0);
    apic_write(0x350, 1 << 16); apic_write(0x360, 1 << 16);
    apic_write(0x370, 1 << 16);
    if (max_lvt >= 4) apic_write(0x340, 1 << 16);
    if (max_lvt >= 5) apic_write(0x330, 1 << 16);
    if (max_lvt >= 6) apic_write(0x2f0, 1 << 16);
    /* Discard inherited in-service firmware interrupts before enabling ours. */
    for (int vector = 255; vector >= 0; --vector)
        if (apic_read(0x100 + (unsigned)(vector / 32) * 16) & (1U << (vector % 32)))
            apic_write(0xb0, 0);
    if (acpi.pcat) { pic_write(0x21, 0xff); pic_write(0xa1, 0xff); }
    uint32_t ends[ACPI_MAX_IOAPICS];
    for (unsigned i = 0; i < acpi.io_count; ++i) {
        volatile uint32_t *io = device_map(acpi.io[i].address, 4096);
        uint32_t version = io_read(io, 1), count = (version >> 16 & 255) + 1;
        if ((version & 255) < 0x11 || count > 120 || acpi.io[i].gsi > UINT32_MAX - count)
            fail("IOAPIC version/range", version);
        ends[i] = acpi.io[i].gsi + count;
        for (unsigned j = 0; j < i; ++j)
            if (acpi.io[i].gsi < ends[j] && acpi.io[j].gsi < ends[i]) fail("IOAPIC GSI overlap", i);
        for (unsigned pin = 0; pin < count; ++pin) {
            unsigned reg = 0x10 + pin * 2;
            io_write(io, reg, io_read(io, reg) | (1 << 16));
            if (!(io_read(io, reg) & (1 << 16))) fail("IOAPIC mask readback", pin);
        }
        serial("IOAPIC_MASKED base_gsi="); hex(acpi.io[i].gsi); serial(" pins="); hex(count); serial("\n");
    }
    /* Divide by 16; calibrate the down-counter over 10 ms of the loader's TSC.
     * No HPET dependency (the HP's HPET is disabled by Linux for a PC10 quirk).
     * External lines stay masked; keyboard/UART are polled, no SMP startup. */
    apic_write(0x3e0, 3);
    uint64_t before = rdtsc(), interval = boot.tsc_hz / 100;
    apic_write(0x380, UINT32_MAX);
    while (rdtsc() - before < interval) __asm__ volatile("pause");
    uint64_t elapsed = rdtsc() - before;
    uint32_t delta = UINT32_MAX - apic_read(0x390);
    apic_write(0x380, 0);
    uint64_t count = (uint64_t)delta * interval / elapsed;
    if (delta < 100 || delta >= 0x80000000U || !count || count > UINT32_MAX)
        fail("APIC timer calibration", delta);
    apic_write(0x320, 32 | (1 << 17)); /* Periodic, vector 32, unmasked. */
    apic_write(0x380, (uint32_t)count);
    serial("APIC_READY mode="); serial(x2apic ? "x2apic" : "xapic");
    serial(" bsp="); hex(id); serial(" count="); hex(count); serial("\n");
}

void timer_init(void) {
    __asm__ volatile("cli" : : : "memory");
#ifdef BOOT_LEGACY_PIC
    (void)apic_init;
    if (!acpi.pcat) fail("legacy PIC not advertised", 0);
    pic_init();
#else
    /* Keep the retained legacy path compiled and warning-checked. */
    (void)pic_init;
    apic_init();
#endif
    uint64_t deadline = rdtsc() + 2 * boot.tsc_hz;
    __asm__ volatile("sti" : : : "memory");
    while (ticks < 3) {
        if (rdtsc() >= deadline) fail("timer IRQ delivery timeout", ticks);
        __asm__ volatile("pause");
    }
#ifdef BOOT_TEST
    extern void arch_irq_probe(void);
    extern uint64_t irq_registers[18];
    extern uint8_t irq_fx_before[512], irq_fx_after[512];
    /* Probe waits in HLT with every GPR, all XMM registers, x87, DF and
     * red-zone sentinels live. The test ISR deliberately destroys SIMD/x87. */
    for (unsigned round = 0; round < 8; ++round) {
        uint64_t before = ticks;
        arch_irq_probe(); /* Returns with IF clear and DF clear. */
        if (ticks <= before) fail("IRQ probe did not wake on timer", ticks);
        for (unsigned i = 0; i < 15; ++i)
            if (irq_registers[i] != 0x1234567800000001ULL + i) fail("IRQ GPR preservation", i);
        if (irq_registers[15] != irq_registers[16] ||
            (irq_registers[17] & 0x600) != 0x600)
            fail("IRQ RSP/RFLAGS preservation", irq_registers[17]);
        /* Defined FXSAVE fields: control/status/tag/opcode, x87 slots, MXCSR,
         * XMM0..15. Reserved fields and instruction/data pointers excluded. */
        for (unsigned i = 0; i < 416; ++i) {
            if ((i >= 8 && i < 24) || (i >= 28 && i < 32) ||
                (i >= 32 && i < 160 && (i - 32) % 16 >= 10)) continue;
            if (irq_fx_before[i] != irq_fx_after[i]) fail("IRQ FP preservation", i);
        }
        extern unsigned irq_redzone_ok;
        if (!irq_redzone_ok) fail("IRQ red zone preservation", round);
        __asm__ volatile("sti" : : : "memory");
    }
    __asm__ volatile("cli" : : : "memory");
    if (use_apic) __asm__ volatile("int $255" : : : "memory");
    else __asm__ volatile("int $39; int $47" : : : "memory");
    if (spurious != (use_apic ? 1U : 2U)) fail("spurious IRQ handling", spurious);
    __asm__ volatile("sti" : : : "memory");
    serial("IRQ_CONTEXT_OK gpr_x87_sse_flags_redzone_hlt_spurious\n");
#endif
    serial(use_apic ? "APIC_IRQ_OK hz=100 ticks=" : "PIT_IRQ_OK hz=100 ticks="); hex(ticks); serial("\n");
}

void timer_idle(uint64_t milliseconds) {
    console_flush();
    if (!milliseconds) return;
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    if (!(flags & 0x200)) fail("timer idle with IRQs disabled", flags);
    uint64_t deadline = rdtsc() + (boot.tsc_hz / 1000) * milliseconds;
    for (;;) {
        /* No lost wake between testing the deadline and sleeping. STI's
         * interrupt shadow makes HLT execute before any pending IRQ. */
        __asm__ volatile("cli" : : : "memory");
        if (rdtsc() >= deadline) break;
        __asm__ volatile("sti; hlt" : : : "memory");
    }
    __asm__ volatile("sti" : : : "memory");
}
