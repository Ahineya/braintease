/* Production framebuffer parser/rendering fixtures using the exact terminal
 * sequences emitted by ripple/curses and mux. Run with fatal UBSan. */
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
static uint64_t pages_alloc(unsigned n) { void *p=calloc(n,4096); assert(p); return (uintptr_t)p; }
static void page_free(uint64_t p) { (void)p; abort(); }
static void *framebuffer_map(uint64_t p, size_t n) { (void)n; return (void *)(uintptr_t)p; }
void console_write(char c);
void console_flush(void);
void screen_status(const char *s);
#include "../boot/console.c"
static void put(const char *s) { while (*s) console_write(*s++); }
static void fresh(void) {
    put("\030\033[0m\033(B\033)B\017\033[?7;25h\033[2J\033[H");
    assert(!escape_state && !console_g0 && !console_g1 && !console_shift);
}
static uint32_t pixel(unsigned column,unsigned row,unsigned x,unsigned y) {
    return shadow[(size_t)(console_top+row*cell_height+y)*boot.width +
                  console_margin+column*cell_width+x];
}
static void glyph(unsigned column,unsigned row,unsigned code,uint32_t fg,uint32_t bg) {
    for(unsigned y=0;y<cell_height;y++) for(unsigned x=0;x<cell_width;x++) {
        unsigned on=gx_font[code*16+y/console_scale]&(0x8000U>>(x/console_scale));
        assert(pixel(column,row,x,y)==(on?fg:bg));
    }
}
static void blank(unsigned column,unsigned row,uint32_t bg) {
    for(unsigned y=0;y<cell_height;y++) for(unsigned x=0;x<cell_width;x++)
        assert(pixel(column,row,x,y)==bg);
}
static void box(unsigned column,unsigned sides,uint32_t fg,uint32_t bg) {
    unsigned stroke=console_scale,cx=(cell_width-stroke)/2,cy=(cell_height-stroke)/2;
    for(unsigned y=0;y<cell_height;y++) for(unsigned x=0;x<cell_width;x++) {
        unsigned horizontal=y>=cy&&y<cy+stroke;
        unsigned vertical=x>=cx&&x<cx+stroke;
        unsigned on=(horizontal&&(((sides&1)&&x<cx+stroke)||((sides&2)&&x>=cx)))||
                    (vertical&&(((sides&4)&&y<cy+stroke)||((sides&8)&&y>=cy)));
        assert(pixel(column,0,x,y)==(on?fg:bg));
    }
}
static uint64_t header_hash(void) {
    uint64_t sum=0;
    for(unsigned y=0;y<console_top;y++)for(unsigned x=0;x<boot.width;x++)
        sum=sum*33+shadow[(size_t)y*boot.width+x];
    return sum;
}
static void fixtures(void) {
    uint32_t fg=color(200,220,230),bg=color(15,26,43);
    uint64_t header=header_hash();
    fresh(); put("\033(BX\033[0 q\033[?1049h\033[?25l\033[?2026h\033[?1006h\033[?1003h\033[?2004hY");
    assert(console_x==console_margin+2*cell_width && console_y==console_top && !escape_state);
    glyph(0,0,'X',fg,bg); glyph(1,0,'Y',fg,bg); blank(2,0,bg);
    fresh();
    put("\033[?1049h\033[0m\033(B\033[r\033[?7l\033[?1l\033[?25l\033[H\033[2J\033[?1006h\033[?1003hX");
    assert(!escape_state && !console_wrap && console_x==console_margin+cell_width);
    glyph(0,0,'X',fg,bg);
    put("\033[?1003l\033[?1002l\033[?1000l\033[?1006l\033[?2026l\033[0m\033(B\033[r\033[?7h\033[?25h\033[0 q\033[?1049l");
    assert(!escape_state && console_wrap && console_x==console_margin+cell_width);
    /* Cursor-style intermediates and designation suffixes never become text. */
    fresh(); put("\033[2 q\033$B\033#8\033[?7;25hZ");
    assert(console_x==console_margin+cell_width && !escape_state);
    glyph(0,0,'Z',fg,bg);
    fresh(); put("\033]0;private title\007X\033]2;another title\033\\Y\033Pdiscard DCS\033\\Z");
    assert(console_x==console_margin+3*cell_width && !escape_state);
    glyph(0,0,'X',fg,bg); glyph(1,0,'Y',fg,bg); glyph(2,0,'Z',fg,bg);
    /* U+041C (Cyrillic capital Em) contains continuation byte 0x9c;
     * UTF-8 string payloads must not mistake it for an 8-bit ST control. */
    fresh(); put("\033]0;\xd0\x9c title\007X\033P\xd0\x9c payload\033\\Y");
    assert(!escape_state && console_x==console_margin+2*cell_width);
    glyph(0,0,'X',fg,bg); glyph(1,0,'Y',fg,bg); blank(2,0,bg);
    fresh(); put("\033(0lqkmjxntuvw\033(BZ");
    const unsigned sides[]={10,3,9,6,5,12,15,14,13,7,11};
    for(unsigned i=0;i<sizeof(sides)/sizeof(*sides);i++)box(i,sides[i],fg,bg);
    glyph(11,0,'Z',fg,bg);
    /* G1/SO/SI and UTF-8 use the same cell-aligned line shapes. */
    fresh(); put("\033)0\016x\017q\xe2\x94\x80\xe2\x94\xbc");
    box(0,12,fg,bg); glyph(1,0,'q',fg,bg); box(2,3,fg,bg); box(3,15,fg,bg);
    fresh(); put("\033[31;44mX\033[1mY\033[22mZ\033[7mR\033[27mS");
    glyph(0,0,'X',ansi_color(1),ansi_color(4)); glyph(1,0,'Y',ansi_color(9),ansi_color(4));
    glyph(2,0,'Z',ansi_color(1),ansi_color(4)); glyph(3,0,'R',ansi_color(4),ansi_color(1));
    glyph(4,0,'S',ansi_color(1),ansi_color(4));
    put("\033[4m ");
    for(unsigned y=0;y<cell_height;y++)for(unsigned x=0;x<cell_width;x++)
        assert(pixel(5,0,x,y)==(y>=cell_height-console_scale?ansi_color(1):ansi_color(4)));
    put("\033[24;39;49mD\033[97;104mW\033[mE");
    glyph(6,0,'D',fg,bg); glyph(7,0,'W',ansi_color(15),ansi_color(12)); glyph(8,0,'E',fg,bg);
    fresh(); put("\033[38;5;196;48;5;232mX\033[38;2;1;2;3;48;2;4;5;6mY");
    glyph(0,0,'X',color(255,0,0),color(8,8,8)); glyph(1,0,'Y',color(1,2,3),color(4,5,6));
    put("\033[38;5;999mZ\033[38;2;300;1;7mW");
    glyph(2,0,'Z',color(1,2,3),color(4,5,6)); glyph(3,0,'W',color(1,2,3),color(4,5,6));
    assert(!console_reverse && !console_bold);
    fresh(); put("\033[31"); assert(escape_state==ESC_CSI && console_x==console_margin);
    put("mX"); glyph(0,0,'X',ansi_color(1),bg);
    fresh(); put("\033[31\t");
    assert(escape_state==ESC_CSI && !escape_intermediate && escape_values[0]==31);
    assert(console_x==console_margin+8*cell_width);
    put("mX"); assert(!escape_state && console_x==console_margin+9*cell_width);
    for(unsigned x=0;x<8;x++)blank(x,0,bg);
    glyph(8,0,'X',ansi_color(1),bg);
    fresh(); put("\033("); assert(escape_state==ESC_CHARSET);
    put("0q"); box(0,3,fg,bg);
    fresh(); put("\033[31\030X\033[31\033[32mY");
    glyph(0,0,'X',fg,bg); glyph(1,0,'Y',ansi_color(2),bg);
    fresh(); put("\033]0;unfinished title"); assert(escape_state==ESC_OSC);
    put("\030X"); assert(!escape_state); glyph(0,0,'X',fg,bg);
    fresh(); put("\033[1 2m\033[1:2m\033[999999999999999999999H");
    put("\033[0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0mZ");
    assert(console_x==console_margin+cell_width && !escape_state && console_foreground==fg);
    glyph(0,0,'Z',fg,bg);
    fresh(); put("ABCDEFGH\033[1;3H\033[3X");
    assert(console_x==console_margin+2*cell_width);
    glyph(0,0,'A',fg,bg); glyph(1,0,'B',fg,bg);
    for(unsigned x=2;x<5;x++)blank(x,0,bg);
    glyph(5,0,'F',fg,bg);
    /* Erase uses the selected background, stays in-grid and preserves cursor. */
    put("\033[42m\033[1K");
    for(unsigned x=0;x<3;x++)blank(x,0,ansi_color(2));
    assert(console_x==console_margin+2*cell_width);
    put("\033[2;4H\033[1J");
    for(unsigned x=0;x<console_columns;x++)blank(x,0,ansi_color(2));
    for(unsigned x=0;x<4;x++)blank(x,1,ansi_color(2));
    blank(4,1,bg); assert(console_y==console_top+cell_height);
    fresh(); put("\033[?7l\033[999;999H");
    uint64_t scrolls=scroll_count; put("XY");
    assert(scroll_count==scrolls && console_x==console_margin+(console_columns-1)*cell_width);
    glyph(console_columns-1,console_rows-1,'Y',fg,bg);
    put("\033[?7hZ\033[31m");
    assert(console_x==console_margin+console_columns*cell_width); /* SGR preserves pending wrap. */
    put("W"); assert(scroll_count==scrolls+1);
    /* Bootstrap telemetry colors stay legible and restore application style. */
    fresh(); put("\033[31;44;1;4;7m\033(0");
    console_boot_status(1,"WiFi initialized",0);
    assert(console_foreground==ansi_color(1) && console_background==ansi_color(4));
    assert(console_fg_index==1 && console_bold && console_underline && console_reverse && console_g0);
    unsigned green=0;
    for(unsigned y=0;y<cell_height;y++)for(unsigned x=0;x<6*cell_width;x++)
        green+=shadow[(size_t)(console_top+y)*boot.width+console_margin+x]==color(60,205,110);
    assert(green);
    assert(header_hash()==header);
    fresh();
    uint32_t state=0x12345678;
    for(unsigned i=0;i<20000;i++) {
        state=state*1664525U+1013904223U;
        console_write((char)(state>>24));
        assert(console_x>=console_margin && console_x<=console_margin+console_columns*cell_width);
        assert(console_y>=console_top && console_y<console_bottom);
        assert(escape_count<sizeof(escape_values)/sizeof(*escape_values));
    }
    assert(header_hash()==header);
}
int main(void) {
    boot.width=641;boot.height=481;boot.stride=650;boot.tsc_hz=60000;
    size_t count=(size_t)boot.stride*boot.height;
    uint32_t *storage=malloc((count+2)*4);assert(storage);
    for(unsigned format=0;format<2;format++) {
        for(size_t i=0;i<count+2;i++)storage[i]=0xdeadbeef;
        boot.framebuffer=(uintptr_t)(storage+1);boot.framebuffer_size=count*4;boot.pixel_format=format;
        console_init();fixtures();console_flush();
        for(unsigned y=0;y<boot.height;y++) {
            for(unsigned x=0;x<boot.width;x++)
                assert(storage[1+(size_t)y*boot.stride+x]==
                       (shadow[(size_t)y*boot.width+x] ^
                        (cursor_presented && x>=cursor_x && x<cursor_x+cell_width &&
                         y>=cursor_y && y<cursor_y+cell_height ? 0x00ffffffU : 0)));
            for(unsigned x=boot.width;x<boot.stride;x++)assert(storage[1+(size_t)y*boot.stride+x]==0xdeadbeef);
        }
        assert(storage[0]==0xdeadbeef && storage[count+1]==0xdeadbeef);
    }
    free(shadow);free(storage);
    puts("console ANSI checks passed: curses/mux sequences, SGR/RGB/backgrounds, DEC/UTF8 borders, ECH/wrap, split/malformed controls, status restoration, fuzz/guards");
}
