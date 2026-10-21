/* Conservative firmware DMA handoff for the initial NVMe driver. Intel VT-d
 * GSTS.TES must be clear on every DRHD. IVRS and ATS/PRI/PASID endpoints are not yet
 * supported. No IOMMU register or PCI extended capability is written here.
 * Intel VT-d specification: DMAR structures, GSTS offset 1Ch, TES bit31. */
#include "dma.h"
static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static int sig(const uint8_t *p, const char *s) {
    for (unsigned i = 0; i < 4; ++i) if (p[i] != (uint8_t)s[i]) return 0;
    return 1;
}
static const char *scopes(const uint8_t *p, unsigned start, unsigned length) {
    for (unsigned at = start; at < length;) {
        if (length - at < 8 || p[at + 1] < 8 || p[at + 1] > length - at || (p[at + 1] & 1))
            return "DMAR device scope bounds";
        if (p[at] < 1 || p[at] > 5) return "DMAR unsupported device scope";
        for (unsigned n = at + 6; n < at + p[at + 1]; n += 2)
            if (p[n] > 31 || p[n + 1] > 7) return "DMAR device scope path";
        at += p[at + 1];
    }
    return 0;
}
const char *dma_parse(const AcpiInfo *a, unsigned bdf, DmaInfo *out) {
    *out = (DmaInfo){0};
    if (bdf > 65535) return "PCI BDF bounds";
    if (a->ivrs_present) return "AMD IOMMU handoff unsupported";
    if (!a->mcfg || a->mcfg_length < 60 || !sig(a->mcfg, "MCFG") ||
        u32(a->mcfg + 4) != a->mcfg_length || (a->mcfg_length - 44) % 16)
        return "missing/invalid PCI ECAM table";
    for (unsigned at = 44; at < a->mcfg_length; at += 16) {
        const uint8_t *p = a->mcfg + at;
        uint64_t base = u64(p);
        unsigned segment = u16(p + 8), first = p[10], last = p[11];
        if (!base || (base & 0xfffff) || first > last || u32(p + 12) ||
            base > UINT64_MAX - ((uint64_t)(last + 1) << 20)) return "PCI ECAM window bounds";
        /* MCFG base corresponds to bus ZERO even when StartBusNumber > 0. */
        if (!segment && (bdf >> 8) >= first && (bdf >> 8) <= last) {
            if (out->ecam) return "overlapping PCI ECAM windows";
            out->ecam = base + ((uint64_t)bdf << 12);
        }
    }
    if (!out->ecam) return "PCI ECAM window missing for NVMe";
    if (!a->dmar) return 0; /* No advertised Intel DMA-remapping hardware. */
    const uint8_t *p = a->dmar;
    if (a->dmar_length < 48 || !sig(p, "DMAR") || u32(p + 4) != a->dmar_length ||
        p[36] < 31 || p[36] > 51) return "invalid DMAR header";
    for (unsigned at = 48; at < a->dmar_length;) {
        if (a->dmar_length - at < 4) return "DMAR record header bounds";
        const uint8_t *r = p + at;
        unsigned type = u16(r), length = u16(r + 2);
        if (length < 4 || length > a->dmar_length - at) return "DMAR record bounds";
        const char *error = 0;
        if (type == 0) {
            if (length < 16 || (r[4] & ~1U) || u16(r + 6)) return "unsupported DRHD header/segment";
            uint64_t address = u64(r + 8);
            if (address < 0x100000 || address % 4096 || out->unit_count == DMA_MAX_UNITS)
                return "DRHD register bounds/count";
            for (unsigned i = 0; i < out->unit_count; ++i)
                if (out->units[i] == address) return "duplicate DRHD";
            out->units[out->unit_count++] = address;
            error = scopes(r, 16, length);
        } else if (type == 1) {
            if (length < 24 || u16(r + 6) || out->rmrr_count == DMA_MAX_RMRRS) return "unsupported RMRR header/count";
            uint64_t first = u64(r + 8), last = u64(r + 16);
            if (first > last || first % 4096 || (last & 4095) != 4095) return "RMRR address bounds";
            out->rmrr[out->rmrr_count].first = first; out->rmrr[out->rmrr_count++].last = last;
            error = scopes(r, 24, length);
        } else if (type == 2) {
            if (length < 8 || u16(r + 6)) return "unsupported ATSR header/segment";
            error = scopes(r, 8, length);
        } else if (type == 3) {
            if (length != 20) return "short RHSA";
        } else if (type == 4) {
            if (length < 9) return "short ANDD";
            if (r[length - 1]) return "unterminated ANDD";
        } else if (type == 5 || type == 6) {
            /* SATC/SIDP describe on-SoC devices, not additional remapping
             * engines. Their scopes are still checked; endpoint ATS/PRI/PASID
             * is separately rejected before enabling this NVMe controller. */
            if (length < 8 || u16(r + 6)) return "unsupported SATC/SIDP header/segment";
            error = scopes(r, 8, length);
        } else return "unsupported DMAR structure";
        if (error) return error;
        at += length;
    }
    return out->unit_count ? 0 : "DMAR has no DRHD units";
}
#ifndef DMA_HOST_TEST
#include "pci.h"
const char *dma_identity_check(unsigned bdf) {
    DmaInfo info;
    const char *error = dma_parse(&acpi, bdf, &info);
    if (error) return error;
    for (unsigned i = 0; i < info.rmrr_count; ++i)
        if (boot.kernel_start <= info.rmrr[i].last && info.rmrr[i].first < boot.kernel_end)
            return "kernel DMA buffers overlap reserved RMRR";
    for (unsigned i = 0; i < info.unit_count; ++i) {
        volatile uint32_t *vtd = device_map(info.units[i], 4096);
        uint32_t version = vtd[0], status = vtd[0x1c / 4];
        serial("DMAR_HANDOFF base="); hex(info.units[i]); serial(" version="); hex(version);
        serial(" gsts="); hex(status); serial("\n");
        if (!(version & 0xf0) || version == UINT32_MAX || status == UINT32_MAX) return "invalid VT-d registers";
        if (status & (1U << 31)) return "VT-d translation still enabled; probe skipped";
        uint32_t cap = vtd[8 / 4];
        if (cap == UINT32_MAX) return "invalid VT-d capability register";
        if (cap & 0x60) { /* CAP.PLMR/PHMR: only read PMEN when implemented. */
            uint32_t protected = vtd[0x64 / 4];
            serial("DMAR_PMEN value="); hex(protected); serial("\n");
            if (protected & 0x80000001U) return "VT-d protected memory still enabled; probe skipped";
        }
    }
    volatile uint32_t *config = device_map(info.ecam, 4096);
    if (config[0] != pci_read(bdf, 0)) return "PCI ECAM identity mismatch";
    uint8_t seen[1024] = {0};
    for (unsigned at = 256; at;) {
        if (at < 256 || at > 4092 || at % 4 || seen[at / 4]) return "PCI extended capability chain";
        seen[at / 4] = 1;
        uint32_t header = config[at / 4];
        if (!header || header == UINT32_MAX) break;
        unsigned id = header & 65535;
        if (id == 0xf || id == 0x13 || id == 0x1b) return "NVMe ATS/PRI/PASID handoff unsupported";
        at = header >> 20;
    }
    serial("DMA_IDENTITY_OK iommu_translation_off no_ats_pri_pasid\n");
    return 0;
}
#endif
