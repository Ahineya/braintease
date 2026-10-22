/* Production console metrics plus the shared C16 terminal-size descriptor.
 * cc -O1 -Wall -Wextra -Werror -fsanitize=undefined \
 *   -fno-sanitize-recover=all tools/x86_64/tests/terminal-geometry.c \
 *   -o /tmp/riplix-terminal-geometry && /tmp/riplix-terminal-geometry
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../boot/bootinfo.h"
#include "../boot/terminal-size.h"
#define RIPLIX_X86_KERNEL_H
#define CONSOLE_HOST_TEST 1
BootInfo boot;
static uint64_t rdtsc(void) { return 0; }
static void serial_char(char c) { (void)c; }
static void serial(const char *s) { (void)s; }
static void hex(uint64_t n) { (void)n; }
static _Noreturn void fail(const char *s, uint64_t n) {
    fprintf(stderr, "%s: %llu\n", s, (unsigned long long)n); abort();
}
static uint64_t pages_alloc(unsigned n) {
    void *p = calloc(n, 4096); assert(p); return (uintptr_t)p;
}
static void page_free(uint64_t p) { (void)p; abort(); }
static void *framebuffer_map(uint64_t p, size_t n) { (void)n; return (void *)(uintptr_t)p; }
void console_write(char c);
void console_flush(void);
void screen_status(const char *s);
#include "../boot/console.c"
static void put(const char *s) { while (*s) console_write(*s++); }

static void geometry(unsigned width, unsigned height, unsigned rows, unsigned columns) {
    size_t count = (size_t)(width + 3) * height;
    uint32_t *allocation = malloc((count + 2) * sizeof(*allocation)); assert(allocation);
    for (size_t i = 0; i < count + 2; ++i) allocation[i] = 0xdeadbeef;
    boot = (BootInfo){.width=width, .height=height, .stride=width+3,
        .framebuffer=(uintptr_t)(allocation+1), .framebuffer_size=count*4, .tsc_hz=60000};
    console_init();
    unsigned actual_rows = 0, actual_columns = 0;
    assert(console_dimensions(&actual_rows, &actual_columns));
    assert(actual_rows == rows && actual_columns == columns);
    assert(console_bottom == console_top + rows * cell_height);
    assert(console_margin + columns * cell_width <= width - console_margin);
    assert(console_margin + (columns + 1) * cell_width > width - console_margin);
    assert(console_bottom <= height - console_margin);
    assert(console_bottom + cell_height > height - console_margin);
    uint16_t reply[] = {1, 4, 0, 0};
    assert(!x64_terminal_size_reply(reply, 4, actual_rows, actual_columns));
    assert(reply[2] == rows && reply[3] == columns);
    /* Advertised last cell is the same last cell the renderer accepts. */
    put("\033[999;999H");
    assert(console_x == console_margin + (columns-1) * cell_width);
    assert(console_y == console_top + (rows-1) * cell_height);
    uint64_t previous_scrolls = scroll_count;
    put("XY");
    assert(scroll_count == previous_scrolls + 1);
    assert(console_y == console_bottom - cell_height);
    console_flush();
    uint32_t *video = allocation + 1;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = width; x < boot.stride; ++x)
            assert(video[(size_t)y * boot.stride + x] == 0xdeadbeef);
    assert(allocation[0] == 0xdeadbeef && allocation[count+1] == 0xdeadbeef);
    free(shadow); shadow = 0; shadow_bytes = 0; console_ready = 0;
    free(allocation);
}

static void rejected(unsigned version, unsigned length, unsigned row_request,
                     unsigned column_request, size_t cells, unsigned rows,
                     unsigned columns, unsigned status) {
    uint16_t descriptor[] = {0xcafe, (uint16_t)version, (uint16_t)length,
        (uint16_t)row_request, (uint16_t)column_request, 0xbeef};
    uint16_t original[6]; memcpy(original, descriptor, sizeof(original));
    assert(x64_terminal_size_reply(descriptor+1, cells, rows, columns) == status);
    assert(!memcmp(original, descriptor, sizeof(original)));
}

int main(void) {
    unsigned rows = 123, columns = 456;
    assert(!console_dimensions(&rows, &columns) && rows == 123 && columns == 456);
    assert(!console_dimensions(0, &columns));
    geometry(128, 128, 3, 12);
    geometry(641, 481, 25, 76);
    geometry(800, 600, 33, 96);
    geometry(1920, 1080, 29, 116);
    geometry(1920, 1200, 33, 116);
    geometry(3840, 2160, 29, 116);
    assert(x64_terminal_size_reply(0, 4, 24, 80) == 4);
    for (size_t cells = 0; cells < 4; ++cells) rejected(1, 4, 0, 0, cells, 24, 80, 4);
    rejected(0, 4, 0, 0, 4, 24, 80, 2);
    rejected(2, 4, 0, 0, 4, 24, 80, 2);
    rejected(1, 3, 0, 0, 4, 24, 80, 3);
    rejected(1, 5, 0, 0, 4, 24, 80, 3);
    rejected(1, 4, 1, 0, 4, 24, 80, 3);
    rejected(1, 4, 0, 1, 4, 24, 80, 3);
    rejected(1, 4, 0, 0, 4, 0, 80, 7);
    rejected(1, 4, 0, 0, 4, 24, 0, 7);
    rejected(1, 4, 0, 0, 4, 65536, 80, 7);
    rejected(1, 4, 0, 0, 4, 24, 65536, 7);
    uint16_t descriptor[] = {1, 4, 0, 0, 0xbeef};
    assert(!x64_terminal_size_reply(descriptor, 4, 65535, 65535));
    assert(descriptor[0] == 1 && descriptor[1] == 4 && descriptor[2] == 65535 &&
           descriptor[3] == 65535 && descriptor[4] == 0xbeef);
    puts("terminal geometry checks passed: real console grid, scale/header/margins, last-cell wrap, stride guards, C16 ABI validation");
}
