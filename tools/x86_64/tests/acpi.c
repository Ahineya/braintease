/* Host regression for the production parser, including actual HP MADT bytes.
 * cc -DACPI_HOST_TEST tests/acpi.c boot/acpi.c boot/power.c -o /tmp/acpi-test */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../boot/acpi.h"
static uint8_t memory[8192];
enum { BASE = 0x100000, RSDP = 0, ROOT = 64, MADT = 256, FADT = 4096 };
static unsigned madt_size;
static void w16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void w32(uint8_t *p, uint32_t v) { w16(p, v); w16(p + 2, v >> 16); }
static void w64(uint8_t *p, uint64_t v) { w32(p, v); w32(p + 4, v >> 32); }
static void sum(uint8_t *p, unsigned n, unsigned at) {
    p[at] = 0;
    unsigned s = 0;
    for (unsigned i = 0; i < n; ++i) s += p[i];
    p[at] = (uint8_t)-s;
}
static void fix(void) {
    sum(memory, 20, 8); sum(memory, 36, 32);
    sum(memory + ROOT, 52, 9); sum(memory + MADT, madt_size, 9); sum(memory + FADT, 116, 9);
}
static const uint8_t *map(void *ctx, uint64_t address, size_t n) {
    (void)ctx;
    if (address < BASE || address - BASE >= sizeof(memory) || n > sizeof(memory) - (address - BASE)) return 0;
    return memory + (address - BASE);
}
static void setup(void) {
    memset(memory, 0, sizeof(memory));
    memcpy(memory, "RSD PTR ", 8); memory[15] = 2; w32(memory + 20, 36); w64(memory + 24, BASE + ROOT);
    memcpy(memory + ROOT, "XSDT", 4); w32(memory + ROOT + 4, 52);
    w64(memory + ROOT + 36, BASE + MADT); w64(memory + ROOT + 44, BASE + FADT);
    uint8_t *p = memory + MADT;
    memcpy(p, "APIC", 4); w32(p + 36, 0xfee00000); w32(p + 40, 1);
    const uint8_t records[] = {
        0,8,0,0,1,0,0,0, /* Enabled xAPIC 0. */
        9,16,0,0,0,1,0,0,1,0,0,0,1,0,0,0, /* Enabled x2APIC 256. */
        9,16,0,0,0,2,0,0,2,0,0,0,2,0,0,0, /* Online-capable, disabled. */
        1,12,2,0,0,0,0xc0,0xfe,0,0,0,0,
        2,10,0,0,2,0,0,0,0,0,
        10,12,0,0,0xff,0xff,0xff,0xff,1,0,0,0,
        127,3,0 /* Unknown but bounded. */
    };
    memcpy(p + 44, records, sizeof(records)); madt_size = 44 + sizeof(records); w32(p + 4, madt_size);
    p = memory + FADT; memcpy(p, "FACP", 4); w32(p + 4, 116); w16(p + 109, 3);
    fix();
}
static AcpiInfo out;
static void good(void) { const char *e = acpi_discover(BASE, map, 0, &out); if (e) puts(e); assert(!e); }
static void bad(void) { assert(acpi_discover(BASE, map, 0, &out)); }
int main(int argc, char **argv) {
    assert(argc == 2);
    setup(); good(); assert(out.cpu_count == 2 && out.cpu_ids[1] == 256 && out.io_count == 1);
    assert(out.isa[0].present && out.isa[0].gsi == 2 && out.boot_arch == 3);
    setup(); memory[8]++; bad();
    setup(); memory[32]++; bad();
    setup(); w32(memory + 20, UINT32_MAX); sum(memory, 20, 8); bad();
    setup(); w64(memory + 24, UINT64_MAX - 10); fix(); bad();
    setup(); memory[ROOT + 9]++; bad();
    setup(); w32(memory + ROOT + 4, 35); fix(); bad();
    setup(); w32(memory + ROOT + 4, 1024 * 1024 + 1); fix(); bad();
    setup(); w64(memory + ROOT + 36, BASE + sizeof(memory) - 16); fix(); bad();
    setup(); memory[MADT + 9]++; bad();
    setup(); w32(memory + MADT + 4, 43); fix(); bad();
    setup(); memory[MADT + 45] = 0; fix(); bad();
    setup(); memory[MADT + 45] = 255; fix(); bad();
    setup(); memory[MADT + 45] = 7; fix(); bad();
    setup(); w32(memory + MADT + 56, 0); fix(); bad(); /* Duplicate APIC 0. */
    setup(); w32(memory + MADT + 36, 0xfee00001); fix(); bad();
    setup(); memory[MADT + 84 + 4] = 1; fix(); bad(); /* IOAPIC unaligned. */
    setup(); memory[MADT + 96 + 8] = 2; fix(); bad(); /* Reserved polarity. */
    setup(); memory[MADT + 96 + 3] = 16; fix(); bad();
    setup(); w64(memory + ROOT + 44, BASE + MADT); fix(); bad();
    setup(); memcpy(memory + MADT, "NONE", 4); fix(); bad();
    /* ACPI 1 RSDT still parses through the same bounded path. */
    setup(); memory[15] = 0; w32(memory + 16, BASE + ROOT);
    memcpy(memory + ROOT, "RSDT", 4); w32(memory + ROOT + 4, 44);
    w32(memory + ROOT + 36, BASE + MADT); w32(memory + ROOT + 40, BASE + FADT);
    sum(memory, 20, 8); sum(memory + ROOT, 44, 9); good();
    /* Firmware fixture was read-only captured over SSH, not reconstructed. */
    setup(); FILE *f = fopen(argv[1], "rb"); assert(f);
    madt_size = (unsigned)fread(memory + MADT, 1, 2048, f); assert(!ferror(f) && feof(f)); fclose(f);
    assert(madt_size == 856); good(); assert(out.cpu_count == 14 && out.io_count == 1);
    assert(out.lapic == 0xfee00000 && out.io[0].address == 0xfec00000 && out.pcat);
    assert(out.override_count == 2 && out.isa[0].gsi == 2 && out.isa[9].flags == 13);
    puts("ACPI parser: valid/invalid topology, RSDT/XSDT and actual HP x2APIC MADT passed");
}
