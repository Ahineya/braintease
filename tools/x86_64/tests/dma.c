/* Exercise production parsing AND MMIO decision logic with read-only mocks. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../boot/bootinfo.h"
#include "../boot/acpi.h"
#define RIPLIX_PCI_H
static BootInfo boot;
AcpiInfo acpi;
static uint32_t vtd[2][1024], config[1024];
static unsigned config_maps;
static void serial(const char *s) { (void)s; }
static void hex(uint64_t n) { (void)n; }
static uint32_t pci_read(unsigned bdf, unsigned offset) { assert(bdf == 0x100 && offset == 0); return 0x501715b7; }
static void *device_map(uint64_t address, size_t bytes) {
    assert(bytes == 4096);
    if (address == 0xfc800000) return vtd[0];
    if (address == 0xfc801000) return vtd[1];
    assert(address == 0xc0100000); ++config_maps; return config;
}
#include "../boot/dma.c"
static uint8_t dmar[4096], mcfg[4096];
static unsigned read_file(const char *path, uint8_t *bytes) {
    FILE *f = fopen(path, "rb"); assert(f);
    unsigned n = fread(bytes, 1, 4096, f); assert(!ferror(f) && feof(f)); fclose(f); return n;
}
static void w32(uint8_t *p, uint32_t n) { for (unsigned i = 0; i < 4; ++i) p[i] = n >> (8 * i); }
static void w64(uint8_t *p, uint64_t n) { for (unsigned i = 0; i < 8; ++i) p[i] = n >> (8 * i); }
int main(int argc, char **argv) {
    assert(argc == 3);
    acpi.dmar = dmar; acpi.mcfg = mcfg;
    acpi.dmar_length = read_file(argv[1], dmar); acpi.mcfg_length = read_file(argv[2], mcfg);
    boot.kernel_start = 0x1000000; boot.kernel_end = 0x2000000;
    config[0] = 0x501715b7; vtd[0][0] = vtd[1][0] = 0x20;
    DmaInfo out;
    assert(!dma_parse(&acpi, 0x100, &out));
    assert(out.unit_count == 2 && out.ecam == 0xc0100000 && !out.rmrr_count);
    assert(!dma_identity_check(0x100));
    config_maps = 0; vtd[1][7] = 1U << 31;
    assert(dma_identity_check(0x100)); assert(!config_maps); vtd[1][7] = 0;
    vtd[0][2] = 0x20; vtd[0][0x64/4] = 1;
    assert(dma_identity_check(0x100)); vtd[0][0x64/4] = 0; vtd[0][2] = 0;
    vtd[0][0] = UINT32_MAX; assert(dma_identity_check(0x100)); vtd[0][0] = 0x20;
    config[0]++; assert(dma_identity_check(0x100)); config[0]--;
    for (unsigned i = 0; i < 3; ++i) {
        config[64] = 0x10000 | (unsigned[]){0xf,0x13,0x1b}[i]; assert(dma_identity_check(0x100));
    }
    config[64] = 0x10010001; assert(dma_identity_check(0x100)); /* Cycle. */
    config[64] = 0x10110001; assert(dma_identity_check(0x100)); /* Unaligned next. */
    config[64] = 0; acpi.ivrs_present = 1; assert(dma_identity_check(0x100)); acpi.ivrs_present = 0;
    for (unsigned n = 0; n < acpi.dmar_length; ++n) {
        AcpiInfo a = acpi; a.dmar_length = n; assert(dma_parse(&a, 0x100, &out));
    }
    uint8_t saved = dmar[50]; dmar[50] = 0; assert(dma_parse(&acpi, 0x100, &out)); dmar[50] = saved;
    saved = dmar[65]; dmar[65] = 0; assert(dma_parse(&acpi, 0x100, &out)); dmar[65] = saved;
    saved = mcfg[55]; mcfg[55] = 0; assert(dma_parse(&acpi, 0x100, &out)); mcfg[55] = saved;
    assert(!dma_identity_check(0x100));
    /* A firmware-reserved DMA region must never contain our kernel buffers. */
    uint8_t synthetic[96] = {0};
    memcpy(synthetic, dmar, 48); w32(synthetic + 4, sizeof(synthetic));
    synthetic[50] = 16; synthetic[52] = 1; w64(synthetic + 56, 0xfc800000);
    synthetic[64] = 1; synthetic[66] = 32;
    w64(synthetic + 72, boot.kernel_start); w64(synthetic + 80, boot.kernel_start + 4095);
    synthetic[88] = 1; synthetic[89] = 8; synthetic[93] = 1;
    AcpiInfo saved_acpi = acpi; acpi.dmar = synthetic; acpi.dmar_length = sizeof(synthetic);
    const char *reserved = dma_identity_check(0x100);
    assert(reserved && strstr(reserved, "overlap reserved RMRR")); acpi = saved_acpi;
    memcpy(mcfg + 60, mcfg + 44, 16); w32(mcfg + 4, 76); acpi.mcfg_length = 76;
    assert(dma_identity_check(0x100)); acpi.mcfg_length = 60; w32(mcfg + 4, 60);
    assert(!dma_identity_check(0x100));
    puts("DMA handoff checks passed: actual HP DRHD/SATC/SIDP/MCFG, translation-off gate, ATS/PRI/PASID, malformed records/ECAM");
}
