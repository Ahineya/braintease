/* Native host harness for the production loader's bounds and ownership paths.
 * It never executes guest x86 code. QEMU covers execution separately. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#undef memset
#undef memcpy
#undef memmove
/* Keep the test's byte loops distinct from sanitizer/libc interceptors. */
#define memset test_memset
#define memcpy test_memcpy
#define memmove test_memmove
#define RIPLIX_X86_KERNEL_H
#include "../boot/bootinfo.h"
static BootInfo boot;
static uint64_t page_alloc(void), pages_alloc(unsigned), page_available(void);
static uint64_t page_mapping_owned(void) { return 0; } /* No MMIO in this fixture. */
static void page_free(uint64_t);
static void console_write(char c) { (void)c; }
static void console_boot_status(unsigned s, const char *m, const char *d) { (void)s; (void)m; (void)d; }
static void out8(uint16_t p, uint8_t v) { (void)p; (void)v; abort(); } /* No hardware I/O. */
static int console_input(void) { return -1; }
static void screen_status(const char *s) { (void)s; }
static void serial(const char *s) { (void)s; }
static void serial_char(char c) { (void)c; }
static void hex(uint64_t n) { (void)n; }
static uint64_t rdtsc(void) { return 0; }
static uint64_t timer_ticks(void) { return 0; }
static void timer_idle(uint64_t ms) { (void)ms; }
static _Noreturn void fail(const char *s, uint64_t value) {
    fprintf(stderr, "%s: %llu\n", s, (unsigned long long)value); exit(2);
}
#include "../boot/runtime.c"
AcpiInfo acpi;
int entropy_available(void) { return 0; }
int entropy_fill(uint8_t *p, unsigned n) { (void)p; (void)n; abort(); }
U rcc_native_entry(Context *c) { (void)c; return 0; }

#define TEST_PAGES 8192
static uint8_t arena[TEST_PAGES * 4096] __attribute__((aligned(4096)));
static uint8_t busy[TEST_PAGES];
static unsigned fail_allocation;
static uint64_t page_available(void) {
    unsigned count = 0;
    for (unsigned i = 0; i < TEST_PAGES; ++i) count += !busy[i];
    return count;
}
static uint64_t pages_alloc(unsigned count) {
    if (fail_allocation) return 0;
    unsigned run = 0;
    for (unsigned i = 0; i < TEST_PAGES; ++i) {
        if (busy[i]) { run = 0; continue; }
        if (++run != count) continue;
        unsigned first = i + 1 - count;
        for (unsigned j = first; j <= i; ++j) busy[j] = 1;
        memset(arena + first * 4096, 0, count * 4096);
        return (uintptr_t)(arena + first * 4096);
    }
    return 0;
}
static uint64_t page_alloc(void) { return pages_alloc(1); }
static void page_free(uint64_t address) {
    if (address < (uintptr_t)arena || address >= (uintptr_t)(arena + sizeof(arena)) || address % 4096)
        fail("test invalid free", address);
    unsigned index = (address - (uintptr_t)arena) / 4096;
    if (!busy[index]) fail("test double free", address);
    busy[index] = 0;
}
static void check(int condition) { if (!condition) fail("loader test assertion", __LINE__); }
static void stream(Context *c, const uint8_t *bytes, size_t count) {
    check(rcc_native_ext_rcc_hosted_image_begin(c, count));
    for (size_t i = 0; i < count;) {
        size_t n = count - i < 4096 ? count - i : 4096;
        for (size_t j = 0; j < n; ++j) c->cells[KERNEL_START + 1024 + j] = bytes[i + j];
        check(rcc_native_ext_rcc_hosted_image_append(c, pointer(KERNEL_START + 1024), n));
        i += n;
    }
}
int main(int argc, char **argv) {
    check(argc == 2);
    FILE *file = fopen(argv[1], "rb"); check(file != NULL);
    check(!fseek(file, 0, SEEK_END)); size_t size = ftell(file); rewind(file);
    uint8_t *original = malloc(size), *bad = malloc(size);
    check(original && bad && fread(original, 1, size, file) == size); fclose(file);
    Context *c = &context;
    c->stack = 2 * BANK + 1;
    c->processes[0] = process_allocate(); check(c->processes[0] != NULL);
    U baseline = page_available();
    for (unsigned test = 0; test < 19; ++test) {
        memcpy(bad, original, size);
        ImageHeader *h = (void *)bad;
        ImageFunction *f = (void *)(h + 1);
        ImageHelper *helper = (void *)(f + h->functions);
        ImageRelocation *relocation = (void *)(helper + h->helpers);
        ImageInitializer *initializer = (void *)(relocation + h->relocations);
        switch (test) {
        case 0: h->tag ^= 1; break;
        case 1: h->total--; break;
        case 2: h->code_size = UINT32_MAX; break;
        case 3: h->functions = UINT32_MAX; break;
        case 4: h->global_bank = 240; break;
        case 5: h->global_cells = UINT32_MAX; break;
        case 6: h->entry = 8192; break;
        case 7: f[1].local = f[0].local; break;
        case 8: f[0].offset = UINT32_MAX; break;
        case 9: helper[0].arity = 99; break;
        case 10: h->magic[0] = 'X'; break;
        case 11: h->helpers = UINT32_MAX; break;
        case 12: h->bindings = h->code_size - 4; break;
        case 13: h->error = 0xf00001; break;
        case 14: f[0].reserved = 1; break;
        case 15: memset(helper[0].name, 'x', sizeof(helper[0].name)); break;
        case 16: check(h->initializers > 0); initializer[0].width = 0; break;
        case 17: initializer[0].pointer = 0xf00001; break;
        case 18: initializer[0].function = 2; break;
        }
        stream(c, bad, size);
        check(rcc_native_ext_rcc_hosted_image_prepare(c));
        check(rcc_native_ext_rcc_hosted_image_poll(c) == UINT32_MAX);
        rcc_native_ext_rcc_hosted_image_cancel(c);
        check(page_available() == baseline && c->images_loaded == 0);
    }
    check(!rcc_native_ext_rcc_hosted_image_begin(c, 1));
    check(!rcc_native_ext_rcc_hosted_image_begin(c, IMAGE_MAX + 1));
    stream(c, original, size - 1);
    check(rcc_native_ext_rcc_hosted_image_prepare(c));
    check(rcc_native_ext_rcc_hosted_image_poll(c) == UINT32_MAX);
    rcc_native_ext_rcc_hosted_image_cancel(c);
    check(rcc_native_ext_rcc_hosted_image_begin(c, size));
    c->cells[KERNEL_START + 1024] = 256;
    check(!rcc_native_ext_rcc_hosted_image_append(c, pointer(KERNEL_START + 1024), 1));
    check(c->processes[0]->loading_done == 0);
    rcc_native_ext_rcc_hosted_image_cancel(c);
    stream(c, original, size);
    fail_allocation = 1;
    check(!rcc_native_ext_rcc_hosted_image_prepare(c));
    fail_allocation = 0;
    rcc_native_ext_rcc_hosted_image_cancel(c);
    check(page_available() == baseline && c->images_loaded == 0);
    stream(c, original, size);
    Process *p = c->processes[0];
    check(!rcc_native_ext_rcc_hosted_image_append(c, pointer(KERNEL_START + 1024), 1));
    check(rcc_native_ext_rcc_hosted_image_prepare(c));
    Module *m = p->staged; check(m != NULL);
    U token = rcc_native_ext_rcc_hosted_image_poll(c);
    check(c->functions[token_index(token)].owner == m);
    p->module = m; p->staged = NULL;
    rcc_native_process_argument(c, 0, 0); rcc_native_process_call(c, token, 1, 0);
    image_apply(c, m);
    process_clone(c, 63, 0); check(c->processes[63] != NULL);
    p->module = NULL; images_collect(c);
    check(m->package != NULL); /* Child still owns the same executable code. */
    process_release(c->processes[63]); c->processes[63] = NULL;
    images_collect(c); check(m->package == NULL);
    check(c->images_loaded == 1 && c->images_released == 1);
    process_release(p); c->processes[0] = NULL;
    check(page_available() == TEST_PAGES);
    free(original); free(bad);
    puts("PASS: RXE1 malformed packages, transactional failure, fork ownership and page reclamation");
    return 0;
}
