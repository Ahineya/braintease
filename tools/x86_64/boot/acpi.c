/* ACPI 6.5 5.2: checksummed RSDP -> XSDT/RSDT -> MADT/FADT.
 * No AML, power management, SMP startup or device IRQ routing yet. */
#include "acpi.h"
static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static int signature(const uint8_t *p, const char *s, unsigned n) {
    for (unsigned i = 0; i < n; ++i) if (p[i] != (uint8_t)s[i]) return 0;
    return 1;
}
static int checksum(const uint8_t *p, size_t n) {
    uint8_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum += p[i];
    return sum == 0;
}
static const uint8_t *table(AcpiMap map, void *ctx, uint64_t address, uint32_t *length) {
    const uint8_t *p = map(ctx, address, 36);
    if (!p) return 0;
    *length = u32(p + 4);
    if (*length < 36 || *length > 1024 * 1024) return 0;
    p = map(ctx, address, *length);
    return p && checksum(p, *length) ? p : 0;
}
static const char *madt(const uint8_t *p, uint32_t length, AcpiInfo *out) {
    if (length < 44) return "short MADT";
    out->lapic = u32(p + 36); out->pcat = u32(p + 40) & 1;
    unsigned lapic_override = 0;
    for (uint32_t at = 44; at < length;) {
        if (length - at < 2 || p[at + 1] < 2 || p[at + 1] > length - at)
            return "MADT record bounds";
        const uint8_t *r = p + at;
        unsigned type = r[0], size = r[1];
        static const uint8_t minimum[11] = {8, 12, 10, 8, 6, 12, 16, 16, 16, 16, 12};
        if (type < 11 && size < minimum[type]) return "short MADT record";
        if (type == 0 || type == 9) {
            uint32_t id = type == 0 ? r[3] : u32(r + 4);
            uint32_t flags = u32(r + (type == 0 ? 4 : 8));
            if (flags & 1) {
                for (unsigned i = 0; i < out->cpu_count; ++i)
                    if (out->cpu_ids[i] == id) return "duplicate enabled APIC ID";
                if (out->cpu_count == ACPI_MAX_CPUS) return "too many ACPI CPUs";
                out->cpu_ids[out->cpu_count++] = id;
            }
        } else if (type == 1) {
            uint32_t address = u32(r + 4), gsi = u32(r + 8);
            if (out->io_count == ACPI_MAX_IOAPICS || address < 0x100000 || address % 4096)
                return "invalid IOAPIC address/count";
            for (unsigned i = 0; i < out->io_count; ++i)
                if (out->io[i].address == address || out->io[i].gsi == gsi)
                    return "duplicate IOAPIC";
            unsigned i = out->io_count++;
            out->io[i].address = address; out->io[i].gsi = gsi; out->io[i].id = r[2];
        } else if (type == 2) {
            unsigned source = r[3], flags = u16(r + 8);
            if (r[2] || source >= 16 || out->isa[source].present ||
                (flags & ~15U) || (flags & 3) == 2 || (flags >> 2 & 3) == 2)
                return "invalid ISA override";
            out->isa[source].present = 1; out->isa[source].gsi = u32(r + 4);
            out->isa[source].flags = flags; ++out->override_count;
        } else if (type == 5) {
            if (lapic_override++) return "duplicate LAPIC override";
            out->lapic = u64(r + 4);
        }
        at += size; /* Unknown records are skipped only after bounds checks. */
    }
    if (!out->cpu_count || !out->io_count || out->lapic < 0x100000 || out->lapic % 4096)
        return "missing MADT topology";
    return 0;
}
const char *acpi_discover(uint64_t rsdp, AcpiMap map, void *ctx, AcpiInfo *out) {
    *out = (AcpiInfo){0};
    const uint8_t *p = map(ctx, rsdp, 20);
    if (!p || !signature(p, "RSD PTR ", 8) || !checksum(p, 20)) return "RSDP checksum/signature";
    uint64_t root = u32(p + 16);
    unsigned width = 4;
    if (p[15] >= 2) {
        p = map(ctx, rsdp, 36);
        if (!p) return "short extended RSDP";
        uint32_t length = u32(p + 20);
        if (length < 36 || length > 4096 || !(p = map(ctx, rsdp, length)) || !checksum(p, length))
            return "extended RSDP checksum/bounds";
        if (u64(p + 24)) { root = u64(p + 24); width = 8; }
    }
    uint32_t length;
    p = table(map, ctx, root, &length);
    if (!p || !signature(p, width == 8 ? "XSDT" : "RSDT", 4) ||
        (length - 36) % width || (length - 36) / width > 4096) return "ACPI root table";
    unsigned found = 0;
    for (uint32_t at = 36; at < length; at += width) {
        uint64_t address = width == 8 ? u64(p + at) : u32(p + at);
        const uint8_t *header = map(ctx, address, 36);
        if (!header) return "ACPI child header bounds";
        int apic = signature(header, "APIC", 4), fadt = signature(header, "FACP", 4);
        int dmar = signature(header, "DMAR", 4), mcfg = signature(header, "MCFG", 4);
        int ivrs = signature(header, "IVRS", 4);
        if (!apic && !fadt && !dmar && !mcfg && !ivrs) continue;
        uint32_t child_length;
        const uint8_t *child = table(map, ctx, address, &child_length);
        if (!child) return "ACPI child checksum/bounds";
        if (apic) {
            if (found++) return "duplicate MADT";
            const char *error = madt(child, child_length, out);
            if (error) return error;
        } else if (fadt) {
            if (out->fadt_present++ || child_length < 116) return "duplicate/short FADT";
            out->boot_arch = u16(child + 109);
            out->reset = power_reset_parse(child,child_length);
        } else if (dmar) {
            if (out->dmar) return "duplicate DMAR";
            out->dmar = child; out->dmar_length = child_length;
        } else if (mcfg) {
            if (out->mcfg) return "duplicate MCFG";
            out->mcfg = child; out->mcfg_length = child_length;
        } else if (ivrs) out->ivrs_present = 1;
    }
    return found ? 0 : "missing MADT";
}

#ifndef ACPI_HOST_TEST
#include "kernel.h"
AcpiInfo acpi;
static const uint8_t *firmware_map(void *ctx, uint64_t address, size_t length) {
    (void)ctx;
    if (address < 4096 || address >= PHYSICAL_LIMIT || !length || length > PHYSICAL_LIMIT - address)
        return 0;
    /* ACPI tables remain reserved, mapped WB, and outside our conventional-RAM
     * allocator. Refuse pointers into arbitrary MMIO or reclaimed RAM. */
    for (uint64_t at = 0; at < boot.memory_map_size; at += boot.descriptor_size) {
        const BootMemoryDescriptor *d = (const void *)(boot.memory_map + at);
        if ((d->type != 9 && d->type != 10) || d->pages > UINT64_MAX / 4096 ||
            address < d->physical_start) continue;
        uint64_t size = d->pages * 4096, offset = address - d->physical_start;
        if (offset < size && length <= size - offset) return (const void *)address;
    }
    return 0;
}
void acpi_init(void) {
    const char *error = acpi_discover(boot.rsdp, firmware_map, 0, &acpi);
    if (error) fail(error, boot.rsdp);
    serial("ACPI_OK cpus="); hex(acpi.cpu_count); serial(" ioapics="); hex(acpi.io_count);
    serial(" lapic="); hex(acpi.lapic); serial(" isa_overrides="); hex(acpi.override_count);
    serial(" boot_arch="); hex(acpi.boot_arch); serial("\n");
}
#endif
