/* Production root-stream serializer with an injected block device. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define PERSISTENT_ROOT 1
#define BANK 64000U
typedef uint64_t U;
typedef struct {
    uint16_t cells[BANK * 3];
    size_t file_offset;
    unsigned file_open, file_writable;
} Context;
static Context context;
static size_t ram_root_size;
static uint8_t disk[8192];
static unsigned transfers, fail_at, fail_flush, dirty_pages[188];
static size_t span(U pointer, U n) {
    U at = (pointer >> 16) * BANK + (pointer & 65535);
    assert((pointer & 65535) < BANK && at <= BANK * 3 && n <= BANK * 3 - at);
    return at;
}
static void dirty(Context *c, size_t at, size_t n) {
    (void)c;
    if (n) for (size_t p = at / 1024; p <= (at + n - 1) / 1024; ++p) dirty_pages[p] = 1;
}
static _Noreturn void fail(const char *message, U status) { (void)message; (void)status; abort(); }
static void serial(const char *message) { (void)message; }
static U block_init(void) { return sizeof(disk); }
static int block_transfer(U offset, void *data, size_t n, unsigned write) {
    assert(offset <= sizeof(disk) && n <= sizeof(disk) - offset);
    if (++transfers == fail_at) return -1;
    if (write) memcpy(disk + offset, data, n); else memcpy(data, disk + offset, n);
    return 0;
}
static int block_flush(void) { return fail_flush ? -1 : 0; }
#include "../boot/root.inc"

int main(void) {
    Context *c = &context;
    root_init();
    c->cells[100] = 'r'; c->cells[101] = 'o'; c->cells[102] = 'o'; c->cells[103] = 't';
    c->cells[110] = 'r'; c->cells[111] = '+'; c->cells[112] = 'b';
    assert(rcc_native_ext_fopen(c, 100, 110) == 1);
    U p = (1ULL << 16) | 63900;
    size_t at = span(p, 3000);
    for (unsigned i = 0; i < 3000; ++i) c->cells[at + i] = (uint16_t)(0x9213U + i * 171U);
    assert(rcc_native_ext_rcc_hosted_write_cells(c, 1, p, 3000) == 3000);
    for (unsigned i = 0; i < 3000; ++i) {
        uint16_t v = c->cells[at + i];
        assert(disk[i * 2] == (uint8_t)v && disk[i * 2 + 1] == v >> 8);
    }
    memset(c->cells + at, 0, 6000);
    assert(rcc_native_ext_fseek(c, 1, 0, 0) == 0);
    assert(rcc_native_ext_rcc_hosted_read_cells(c, 1, p, 3000) == 3000);
    for (unsigned i = 0; i < 3000; ++i) assert(c->cells[at + i] == (uint16_t)(0x9213U + i * 171U));
    for (unsigned i = at / 1024; i <= (at + 2999) / 1024; ++i) assert(dirty_pages[i]);
    assert(rcc_native_ext_ftell(c, 1) == 6000);
    assert(rcc_native_ext_fseek(c, 1, (uint32_t)-1, 2) == 0);
    assert(rcc_native_ext_rcc_hosted_read_cells(c, 1, p, 2) == 0); /* Odd trailing octet. */
    assert(rcc_native_ext_fread(c, p, 1, 2, 1) == 1);
    assert(rcc_native_ext_fread(c, p, 1, 2, 1) == 0);
    assert(rcc_native_ext_fseek(c, 1, (uint32_t)-1, 0) == 65535);
    assert(rcc_native_ext_fseek(c, 1, sizeof(disk) + 1, 0) == 65535);
    assert(rcc_native_ext_fseek(c, 1, 0, 3) == 65535);
    assert(rcc_native_ext_fseek(c, 1, 3, 0) == 0);
    c->cells[at] = 0xabcd; c->cells[at + 1] = 0xeffe;
    assert(rcc_native_ext_fwrite(c, p, 1, 2, 1) == 2);
    assert(disk[3] == 0xcd && disk[4] == 0xfe); /* Character I/O emits octets. */
    assert(rcc_native_ext_fseek(c, 1, 3, 0) == 0);
    assert(rcc_native_ext_fread(c, p, 1, 2, 1) == 2);
    assert(c->cells[at] == 0xcd && c->cells[at + 1] == 0xfe);
    assert(rcc_native_ext_fread(c, p, UINT64_MAX, 2, 1) == 0);
    assert(rcc_native_ext_fseek(c, 1, 0, 0) == 0);
    fail_at = transfers + 2;
    assert(rcc_native_ext_rcc_hosted_read_cells(c, 1, p, 3000) == 2048);
    assert(c->file_offset == 4096); /* Error after first completed native chunk. */
    fail_at = 0;
    c->file_writable = 0;
    assert(rcc_native_ext_fwrite(c, p, 1, 1, 1) == 0);
    assert(rcc_native_ext_rcc_hosted_write_cells(c, 1, p, 1) == 0);
    fail_flush = 1;
    assert(rcc_native_ext_fflush(c, 1) == 65535);
    assert(rcc_native_ext_rcc_hosted_sync_image(c) == 65535);
    assert(rcc_native_ext_fclose(c, 1) == 65535 && !c->file_open);
    puts("PASS: root stream cells/octets, bank/page edges, EOF, dirty tracking and I/O/flush errors");
}
