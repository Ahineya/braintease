/* Pixel-backed cursor acceptance fixtures against the production console.
 * cc -O1 -Wall -Wextra -Werror -fsanitize=undefined \
 *   -fno-sanitize-recover=all tools/x86_64/tests/console-cursor.c \
 *   -o /tmp/riplix-console-cursor && /tmp/riplix-console-cursor
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../boot/bootinfo.h"
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
    void *p=calloc(n,4096); assert(p); return (uintptr_t)p;
}
static void page_free(uint64_t p) { (void)p; abort(); }
static void *framebuffer_map(uint64_t p, size_t n) { (void)n; return (void *)(uintptr_t)p; }
void console_write(char c);
void console_flush(void);
void screen_status(const char *s);
#include "../boot/console.c"
static uint32_t *storage, *video, *saved;
static size_t count, ram_bytes;
static void put(const char *s) { while (*s) console_write(*s++); }
static void check_frame(void) {
    for(unsigned y=0;y<boot.height;y++) {
        for(unsigned x=0;x<boot.width;x++) {
            unsigned cursor=cursor_presented && x>=cursor_x && x<cursor_x+cell_width &&
                            y>=cursor_y && y<cursor_y+cell_height;
            assert(video[(size_t)y*boot.stride+x]==
                   (shadow[(size_t)y*boot.width+x] ^ (cursor?0x00ffffffU:0)));
        }
        for(unsigned x=boot.width;x<boot.stride;x++)
            assert(video[(size_t)y*boot.stride+x]==0xdeadbeef);
    }
    assert(storage[0]==0xdeadbeef && storage[count+1]==0xdeadbeef);
}
static void save_shadow(void) { memcpy(saved,shadow,ram_bytes); }
static void unchanged_shadow(void) { assert(!memcmp(saved,shadow,ram_bytes)); }
static void fixtures(void) {
    uint64_t cell_bytes=(uint64_t)cell_width*cell_height*4;
    assert(cursor_visible && cursor_presented && cursor_x==console_margin && cursor_y==console_top);
    check_frame(); save_shadow();
    uint64_t presents=present_count,bytes=presented_bytes;
    for(unsigned i=0;i<10000;i++)console_flush();
    assert(present_count==presents && presented_bytes==bytes);
    /* Cursor-only transitions restore source RAM exactly, with bounded writes. */
    put("\033[?25l"); assert(!dirty); console_flush(); unchanged_shadow(); check_frame();
    assert(!cursor_presented && presented_bytes==bytes+cell_bytes);
    bytes=presented_bytes; presents=present_count;
    put("\033[?25l"); console_flush(); assert(present_count==presents);
    put("\033[?25h"); console_flush(); unchanged_shadow(); check_frame();
    assert(cursor_presented && presented_bytes==bytes+cell_bytes);
    put("Hello\b"); console_flush(); check_frame(); save_shadow();
    assert(console_x==console_margin+4*cell_width);
    bytes=presented_bytes;
    unsigned old_x=cursor_x,old_y=cursor_y;
    put("\033[999;999H"); assert(!dirty);
    /* Neither restoration nor overlay may depend on the device's old pixels.
     * Poison outside both cells too: a cursor-only jump must leave it untouched,
     * proving actual write bounds independently of the byte counter. */
    for(unsigned row=0;row<boot.height;row++)
        for(unsigned col=0;col<boot.width;col++)
            video[(size_t)row*boot.stride+col]=0xabcdef12;
    console_flush(); unchanged_shadow();
    assert(presented_bytes==bytes+2*cell_bytes);
    assert(cursor_x==console_margin+(console_columns-1)*cell_width &&
           cursor_y==console_bottom-cell_height);
    for(unsigned row=0;row<boot.height;row++)
        for(unsigned col=0;col<boot.width;col++) {
            unsigned old=col>=old_x && col<old_x+cell_width && row>=old_y && row<old_y+cell_height;
            unsigned current=col>=cursor_x && col<cursor_x+cell_width &&
                             row>=cursor_y && row<cursor_y+cell_height;
            uint32_t expected=old || current ? shadow[(size_t)row*boot.width+col] : 0xabcdef12;
            assert(video[(size_t)row*boot.stride+col]==(expected^(current?0x00ffffffU:0)));
        }
    damage(0,0,boot.width,boot.height); console_flush(); check_frame();
    bytes=presented_bytes;
    put("\033[H"); console_flush(); unchanged_shadow(); check_frame();
    assert(presented_bytes==bytes+2*cell_bytes);
    /* Mode changes can arrive in fragments and in a private-mode list. */
    presents=present_count;
    put("\033[?25"); console_flush(); assert(present_count==presents && cursor_visible);
    put("l"); console_flush(); unchanged_shadow(); check_frame(); assert(!cursor_visible);
    put("\033[?7;25h"); console_flush(); check_frame(); assert(console_wrap && cursor_visible);
    put("\033[?7;25l"); console_flush(); check_frame(); assert(!console_wrap && !cursor_visible);
    put("\033[?7;25h\033[?1003l\033[?25:1l"); console_flush(); check_frame();
    assert(console_wrap && cursor_visible);
    /* Text and erasure beneath the cursor never bake the overlay into RAM. */
    put("\033[H\033[31;44mA\b"); console_flush(); check_frame();
    assert(cursor_x==console_margin && cursor_y==console_top);
    put("\033[2K"); console_flush(); check_frame();
    for(unsigned y=console_top;y<console_top+cell_height;y++)
        for(unsigned x=console_margin;x<console_margin+console_columns*cell_width;x++)
            assert(shadow[(size_t)y*boot.width+x]==ansi_color(4));
    /* Pending autowrap stays at the last usable cell, including a one-row grid. */
    put("\033[999;999HX"); console_flush(); check_frame();
    assert(console_x==console_margin+console_columns*cell_width);
    assert(cursor_x==console_margin+(console_columns-1)*cell_width);
    uint64_t scrolls=scroll_count;
    put("Y"); console_flush(); check_frame(); assert(scroll_count==scrolls+1);
    /* A copied scroll row contains text pixels only, with no cursor trail. */
    put("\033[0m\033[H\033[2J");
    for(unsigned row=0;row<console_rows;row++)
        rectangle(console_margin,console_top+row*cell_height,console_columns*cell_width,
                  cell_height,0x12000000U+row);
    put("\033[999;1H"); console_flush(); check_frame();
    put("\n"); console_flush(); check_frame();
    for(unsigned row=0;row+1<console_rows;row++)
        for(unsigned y=0;y<cell_height;y++)
            for(unsigned x=0;x<console_columns*cell_width;x++)
                assert(shadow[(size_t)(console_top+row*cell_height+y)*boot.width+console_margin+x]
                       ==0x12000000U+row+1);
    /* Cursor updates leave margins, status area, partial cells and stride intact. */
    uint32_t random=0x12345678;
    for(unsigned i=0;i<300;i++) {
        random=random*1664525U+1013904223U;
        char sequence[48];
        switch(random%6) {
        case 0: snprintf(sequence,sizeof(sequence),"\033[%u;%uH",random%1000,random/1000%1000); break;
        case 1: strcpy(sequence,"\033[?25l"); break;
        case 2: strcpy(sequence,"\033[?25h"); break;
        case 3: strcpy(sequence,"\033[2K"); break;
        case 4: strcpy(sequence,"\n"); break;
        default: sequence[0]=(char)('A'+random%26); sequence[1]=0; break;
        }
        put(sequence); console_flush(); check_frame();
    }
    put("\033[?25l"); console_flush(); check_frame();
    /* A GUI presentation is protected from background console output. */
    console_graphics(1);
    for(unsigned y=0;y<boot.height;y++)for(unsigned x=0;x<boot.width;x++)
        video[(size_t)y*boot.stride+x]=0x12345678;
    put("background console text\n");console_flush();
    for(unsigned y=0;y<boot.height;y++)for(unsigned x=0;x<boot.width;x++)
        assert(video[(size_t)y*boot.stride+x]==0x12345678);
    console_graphics(0);check_frame();
}
static void geometry(unsigned width,unsigned height,unsigned format) {
    boot=(BootInfo){.width=width,.height=height,.stride=width+7,.pixel_format=format,.tsc_hz=60000};
    count=(size_t)boot.stride*boot.height;
    ram_bytes=(size_t)boot.width*boot.height*4;
    storage=malloc((count+2)*4); saved=malloc(ram_bytes); assert(storage && saved);
    for(size_t i=0;i<count+2;i++)storage[i]=0xdeadbeef;
    video=storage+1; boot.framebuffer=(uintptr_t)video; boot.framebuffer_size=count*4;
    console_init();fixtures();
    /* Reinitialization must discard an old presentation position cleanly. */
    console_init();check_frame();
    free(shadow); shadow=0; shadow_bytes=0; console_ready=0;
    free(storage);free(saved);
}
int main(void) {
    for(unsigned format=0;format<2;format++) {
        geometry(128,128,format); /* One usable row, including last-cell wrap. */
        geometry(641,481,format); /* Padded stride and partial right/bottom cells. */
        geometry(1920,1080,format); /* Automatic scale 2, same cursor cell geometry. */
    }
    puts("console cursor checks passed: RGB/BGR, scales, RAM-only overlay, hide/show, split/malformed modes, bounded jumps, pending wrap, erase/scroll trails, idle batching, reinit/guards");
}
