/* Production console with a mock video aperture. Run on the host with UBSan.
 * Deliberately poison video RAM after rendering: scroll must use RAM shadow. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../boot/bootinfo.h"
#define RIPLIX_X86_KERNEL_H
#define CONSOLE_HOST_TEST 1
BootInfo boot;
static uint64_t clock_now;
static uint64_t rdtsc(void) { return clock_now; }
static char serial_output[4096];
static unsigned serial_used;
static void serial_char(char c) { if(serial_used+1<sizeof(serial_output)){serial_output[serial_used++]=c;serial_output[serial_used]=0;} }
static void serial(const char *s) { (void)s; }
static void hex(uint64_t n) { (void)n; }
static _Noreturn void fail(const char *s, uint64_t n) { fprintf(stderr, "%s: %llu\n", s, (unsigned long long)n); abort(); }
static uint64_t pages_alloc(unsigned n) { void *p = calloc(n, 4096); assert(p); return (uintptr_t)p; }
static void page_free(uint64_t p) { (void)p; abort(); }
static void *framebuffer_map(uint64_t p, size_t n) { (void)n; return (void *)(uintptr_t)p; }
void console_write(char c);
void console_flush(void);
void screen_status(const char *s);
#include "../boot/console.c"
static void put(const char *s) { while (*s) console_write(*s++); }
static void check_pixels(uint32_t *hardware) {
    for (unsigned y = 0; y < boot.height; ++y) {
        for (unsigned x = 0; x < boot.width; ++x)
            assert(hardware[(size_t)y * boot.stride + x] ==
                   (shadow[(size_t)y * boot.width + x] ^
                    (cursor_presented && x >= cursor_x && x < cursor_x + cell_width &&
                     y >= cursor_y && y < cursor_y + cell_height ? 0x00ffffffU : 0)));
        for (unsigned x = boot.width; x < boot.stride; ++x)
            assert(hardware[(size_t)y * boot.stride + x] == 0xdeadbeef);
    }
}
int main(void) {
    boot.width = 641; boot.height = 481; boot.stride = 650; boot.tsc_hz = 60000;
    size_t count = (size_t)boot.stride * boot.height;
    uint32_t *storage = malloc((count + 2) * 4), *hardware = storage + 1;
    for (size_t i = 0; i < count + 2; ++i) storage[i] = 0xdeadbeef;
    boot.framebuffer = (uintptr_t)hardware; boot.framebuffer_size = count * 4;
    for (unsigned format = 0; format < 2; ++format) {
        boot.pixel_format = format; console_init(); check_pixels(hardware);
        uint64_t before = present_count;
        put("prompt> "); assert(dirty && present_count == before);
        console_flush(); assert(present_count == before + 1); check_pixels(hardware);
        put("\033[2J\033[H\tX\bY\rZ\n\xe2\x95\x94\xe2\x95\x90\xe2\x95\x97\n");
        assert(!escape_state && !utf8_remaining); console_flush(); check_pixels(hardware);
        /* Real rendered markers: exact alignment, color and default restoration. */
        put("\033[2J\033[H"); serial_used=0;
        uint32_t normal=console_foreground;
        console_boot_status(0,"Initializing WiFi",0);
        console_boot_status(1,"WiFi device initialized",0);
        console_boot_status(2,"WiFi initialization failed","firmware ALIVE timeout");
        assert(!strcmp(serial_output,".....  Initializing WiFi\n[ OK ] WiFi device initialized\n[ERR ] WiFi initialization failed: firmware ALIVE timeout\n"));
        assert(console_foreground==normal);
        unsigned green=0,red=0,plain=0;
        for(unsigned y=console_top;y<console_top+3*cell_height;y++)
            for(unsigned x=console_margin;x<console_margin+6*cell_width;x++) {
                uint32_t pixel=shadow[(size_t)y*boot.width+x];
                green+=pixel==color(60,205,110);red+=pixel==color(245,95,95);
            }
        for(unsigned y=console_top;y<console_top+3*cell_height;y++)
            for(unsigned x=console_margin+7*cell_width;x<boot.width-console_margin;x++)
                plain+=shadow[(size_t)y*boot.width+x]==normal;
        assert(green && red && plain); check_pixels(hardware);
        /* Give every source row a unique value, then poison the device. */
        for (unsigned y = console_top; y < console_bottom; ++y)
            for (unsigned x = console_margin; x < boot.width - console_margin; ++x)
                shadow[(size_t)y * boot.width + x] = 0x100000 + y;
        for (size_t i = 0; i < count; ++i) if (i % boot.stride < boot.width) hardware[i] = 0xbad;
        console_y = console_bottom - cell_height; console_newline();
        assert(shadow[(size_t)console_top * boot.width + console_margin] == 0x100000 + console_top + cell_height);
        damage(0, 0, boot.width, boot.height); console_flush(); check_pixels(hardware);
        before = present_count;
        for (unsigned i = 0; i < 250; ++i) put("line\n");
        assert(present_count == before); /* Clock frozen: all output batched. */
        clock_now += boot.tsc_hz / 60; console_write('!');
        assert(present_count == before + 1); check_pixels(hardware);
        assert(storage[0] == 0xdeadbeef && storage[count + 1] == 0xdeadbeef);
    }
    printf("console checks passed: RGB/BGR, padded stride, guards, poisoned video, ANSI/UTF8, colored aligned status markers, batching (%llu scrolls)\n", (unsigned long long)scroll_count);
    free(shadow); free(storage);
}
