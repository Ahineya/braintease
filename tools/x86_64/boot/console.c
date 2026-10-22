#include "kernel.h"
#ifdef BOOT_FONT_ATLAS
extern const uint8_t font_payload_start[], font_payload_end[];
static const uint8_t *font_alpha;
#else
#include "../../../ports/graphics/fonts/rom8x16.inc"
#endif
static unsigned font_width = 8, font_height = 16;
static unsigned console_ready;
static unsigned graphics_owns_display;
void console_graphics(unsigned active);
void boot_diagnostic(const char *message, uint64_t status) {
    if (!console_ready) return;
    console_graphics(0);
    screen_status("Kernel panic; machine stopped");
    const char *prefix = "\n[KERNEL PANIC] ";
    while (*prefix) console_write(*prefix++);
    while (*message) console_write(*message++);
    console_write(' '); console_write('0'); console_write('x');
    for (int shift = 60; shift >= 0; shift -= 4) console_write("0123456789abcdef"[status >> shift & 15]);
    console_write('\n');
    console_flush();
}

/* Rendering and scrolling only touch ordinary native RAM. The framebuffer is
 * a write-only presentation target (except the explicit boot readback test).
 * These buffers are platform allocations, outside all C16 process spaces. */
static uint32_t *shadow;
static size_t shadow_bytes;
static unsigned dirty, dirty_left, dirty_top, dirty_right, dirty_bottom;
static uint64_t last_present, present_count, presented_bytes, scroll_count;
static unsigned console_scale, console_margin, console_top, console_bottom;
static unsigned console_columns, console_rows, console_x, console_y, cell_width, cell_height;
static unsigned cursor_visible, cursor_presented, cursor_x, cursor_y;
static void pixel_copy(void *destination, const void *source, size_t pixels) {
    /* Forward overlap is intentional for upward scrolling. Avoid the runtime's
     * byte-at-a-time ABI memcpy; REP also works without enabling SIMD. */
#ifdef CONSOLE_HOST_TEST
    memmove(destination, source, pixels * 4);
#else
    __asm__ volatile("cld; rep movsl" : "+D"(destination), "+S"(source), "+c"(pixels) : : "memory");
#endif
}
static void damage(unsigned x, unsigned y, unsigned w, unsigned h) {
    if (x >= boot.width || y >= boot.height || !w || !h) return;
    unsigned right = w > boot.width - x ? boot.width : x + w;
    unsigned bottom = h > boot.height - y ? boot.height : y + h;
    if (!dirty) {
        dirty_left = x; dirty_top = y; dirty_right = right; dirty_bottom = bottom; dirty = 1;
    } else {
        if (x < dirty_left) dirty_left = x;
        if (y < dirty_top) dirty_top = y;
        if (right > dirty_right) dirty_right = right;
        if (bottom > dirty_bottom) dirty_bottom = bottom;
    }
}
static void present_rectangle(unsigned x, unsigned y, unsigned width, unsigned height) {
    for (unsigned row = y; row < y + height; ++row)
        pixel_copy((uint32_t *)boot.framebuffer + (size_t)row * boot.stride + x,
                   shadow + (size_t)row * boot.width + x, width);
    presented_bytes += (uint64_t)width * height * 4;
}
void console_flush(void) {
    if (!shadow || graphics_owns_display) return;
    unsigned visible = console_ready && cursor_visible;
    unsigned x = console_x, y = console_y;
    /* A pending autowrap position belongs to the last cell, not the margin. */
    if (visible && x >= console_margin + console_columns * cell_width)
        x = console_margin + (console_columns - 1) * cell_width;
    if (!dirty && visible == cursor_presented &&
        (!visible || (x == cursor_x && y == cursor_y))) return;
    /* Cursor pixels are a presentation overlay: shadow never contains them.
     * Restore and redraw cells separately from the ordinary damage rectangle,
     * so a cursor jump cannot turn a tiny update into a screen-sized copy.
     * Inversion reads RAM only and stays visible on every application colour.
     * The cursor is steady; idle flushes do no work and require no blink timer. */
    if (cursor_presented) present_rectangle(cursor_x, cursor_y, cell_width, cell_height);
    if (dirty)
        present_rectangle(dirty_left, dirty_top, dirty_right - dirty_left, dirty_bottom - dirty_top);
    if (visible) {
        volatile uint32_t *video = (void *)boot.framebuffer;
        for (unsigned row = y; row < y + cell_height; ++row)
            for (unsigned column = x; column < x + cell_width; ++column)
                video[(size_t)row * boot.stride + column] =
                    shadow[(size_t)row * boot.width + column] ^ 0x00ffffffU;
        presented_bytes += (uint64_t)cell_width * cell_height * 4;
    }
    cursor_presented = visible; cursor_x = x; cursor_y = y;
#ifndef CONSOLE_HOST_TEST
    __asm__ volatile("sfence" : : : "memory");
#endif
    ++present_count; dirty = 0; last_present = rdtsc();
}
void console_graphics(unsigned active) {
    if (active) {
        if (!graphics_owns_display) console_flush();
        graphics_owns_display = 1;
    } else if (graphics_owns_display) {
        graphics_owns_display = 0; cursor_presented = 0;
        damage(0,0,boot.width,boot.height);
        console_flush();
    }
}
void console_report(void) {
    serial("CONSOLE_BATCHED_OK presents="); hex(present_count);
    serial(" bytes="); hex(presented_bytes); serial(" scrolls="); hex(scroll_count); serial("\n");
}
static uint32_t color(unsigned r, unsigned g, unsigned b) {
    return boot.pixel_format ? b | g << 8 | r << 16 : r | g << 8 | b << 16;
}
static void rectangle(unsigned x, unsigned y, unsigned w, unsigned h, uint32_t value) {
    uint32_t *pixels = shadow;
    damage(x, y, w, h);
    for (unsigned row = y; row < y + h && row < boot.height; ++row)
        for (unsigned column = x; column < x + w && column < boot.width; ++column)
            pixels[(size_t)row * boot.width + column] = value;
}
static void text_on(unsigned x, unsigned y, const char *s, unsigned scale,
                    uint32_t value, uint32_t background) {
    for (; *s; ++s, x += font_width * scale)
        for (unsigned row = 0; row < font_height; ++row)
            for (unsigned col = 0; col < font_width; ++col) {
#ifdef BOOT_FONT_ATLAS
                unsigned alpha = font_alpha[((uint8_t)*s * font_height + row) * font_width + col];
#else
                unsigned alpha = gx_font[(uint8_t)*s * 16 + row] & (0x8000U >> col) ? 255 : 0;
#endif
                if (!alpha) continue;
                uint32_t blended = 0;
                for (unsigned shift = 0; shift < 24; shift += 8)
                    blended |= ((((value >> shift) & 255) * alpha +
                                 ((background >> shift) & 255) * (255 - alpha) + 127) / 255) << shift;
                rectangle(x + col * scale, y + row * scale, scale, scale, blended);
            }
}
static void text(unsigned x, unsigned y, const char *s, unsigned scale, uint32_t value) {
    text_on(x, y, s, scale, value, color(15, 26, 43));
}
#ifndef CONSOLE_SCALE
#define CONSOLE_SCALE 0
#endif
/* This is the usable text grid, excluding the persistent title/status area,
 * margins and partial cells. It is also the geometry advertised to userland. */
int console_dimensions(unsigned *rows, unsigned *columns) {
    if (!console_ready || !rows || !columns) return 0;
    *rows = console_rows;
    *columns = console_columns;
    return 1;
}
static uint32_t utf8_code, console_foreground, console_background;
static unsigned utf8_remaining, console_bold, console_reverse, console_underline;
static unsigned console_fg_index, console_g0, console_g1, console_shift, console_wrap;
enum { ESC_START=1, ESC_CSI, ESC_CHARSET, ESC_OSC, ESC_OSC_END,
       ESC_STRING, ESC_STRING_END, ESC_INTERMEDIATE };
static unsigned escape_state, escape_count, escape_values[16];
static unsigned escape_private, escape_intermediate, escape_bad, escape_parameters;
static unsigned escape_charset;
static uint32_t ansi_color(unsigned index) {
    /* Same base palette as the shared terminal model. Native pixels can also
     * retain the exact 256-colour cube/grays instead of approximating them. */
    static const uint8_t rgb[16][3] = {
        {0,0,0},{170,0,0},{0,170,0},{170,85,0},
        {0,0,170},{170,0,170},{0,170,170},{170,170,170},
        {85,85,85},{255,85,85},{85,255,85},{255,255,85},
        {85,85,255},{255,85,255},{85,255,255},{255,255,255}
    };
    if (index < 16) return color(rgb[index][0], rgb[index][1], rgb[index][2]);
    if (index >= 232) { unsigned gray = 8 + (index - 232) * 10; return color(gray,gray,gray); }
    unsigned n = index - 16, r = n / 36, g = n / 6 % 6, b = n % 6;
    return color(r ? 55 + 40*r : 0, g ? 55 + 40*g : 0, b ? 55 + 40*b : 0);
}
static void console_attributes_reset(void) {
    console_foreground = color(200, 220, 230);
    console_background = color(15, 26, 43);
    console_fg_index = 16;
    console_bold = console_reverse = console_underline = 0;
}
static uint32_t console_ink(void) {
    return console_bold && console_fg_index < 8 ? ansi_color(console_fg_index + 8) : console_foreground;
}
static uint32_t console_paper(void) {
    return console_reverse ? console_ink() : console_background;
}
static void console_sgr(void) {
    for (unsigned i = 0; i <= escape_count; ++i) {
        unsigned value = escape_values[i];
        if (!value) console_attributes_reset();
        else if (value == 1) console_bold = 1;
        else if (value == 22) console_bold = 0;
        else if (value == 4) console_underline = 1;
        else if (value == 24) console_underline = 0;
        else if (value == 7) console_reverse = 1;
        else if (value == 27) console_reverse = 0;
        else if ((value >= 30 && value <= 37) || (value >= 90 && value <= 97)) {
            console_fg_index = value < 90 ? value-30 : value-90+8;
            console_foreground = ansi_color(console_fg_index);
        } else if (value == 39) { console_foreground = color(200,220,230); console_fg_index = 16; }
        else if ((value >= 40 && value <= 47) || (value >= 100 && value <= 107))
            console_background = ansi_color(value < 100 ? value-40 : value-100+8);
        else if (value == 49) console_background = color(15,26,43);
        else if (value == 38 || value == 48) {
            uint32_t selected = 0; unsigned valid = 0;
            if (i+2 <= escape_count && escape_values[i+1] == 5) {
                unsigned index = escape_values[i+2]; i += 2;
                if (index < 256) { selected = ansi_color(index); valid = 1; }
            } else if (i+4 <= escape_count && escape_values[i+1] == 2) {
                unsigned r=escape_values[i+2], g=escape_values[i+3], b=escape_values[i+4]; i += 4;
                if (r<256 && g<256 && b<256) { selected = color(r,g,b); valid = 1; }
            } else break; /* Do not interpret incomplete colour data as SGR. */
            if (valid && value == 38) { console_foreground = selected; console_fg_index = 16; }
            if (valid && value == 48) console_background = selected;
        }
    }
}
static void console_clear(unsigned x, unsigned y, unsigned mode) {
    uint32_t background = console_paper();
    unsigned right = console_margin + console_columns * cell_width;
    if (mode == 2 || mode == 3)
        rectangle(console_margin, console_top, right-console_margin, console_bottom-console_top, background);
    else if (mode == 1) {
        rectangle(console_margin, console_top, right-console_margin, y-console_top, background);
        rectangle(console_margin, y, x-console_margin+cell_width, cell_height, background);
    } else if (!mode) {
        rectangle(x, y, right-x, cell_height, background);
        if (y + cell_height < console_bottom)
            rectangle(console_margin, y+cell_height, right-console_margin,
                      console_bottom-y-cell_height, background);
    }
}
static void console_csi(unsigned c) {
    if (escape_bad || escape_intermediate) return;
    if (escape_private) {
        if (escape_private == '?' && (c == 'h' || c == 'l')) {
            for (unsigned i = 0; i <= escape_count; ++i)
                if (escape_values[i] == 7) {
                    console_wrap = c == 'h';
                    unsigned right = console_margin + console_columns*cell_width;
                    if (!console_wrap && console_x >= right) console_x = right-cell_width;
                } else if (escape_values[i] == 25) cursor_visible = c == 'h';
        }
        return; /* Other modes are consumed without pretending to implement them. */
    }
    if (c == 'm') { console_sgr(); return; }
    unsigned x = (console_x - console_margin) / cell_width;
    unsigned y = (console_y - console_top) / cell_height;
    if (x >= console_columns) x = console_columns-1;
    unsigned count = escape_values[0] ? escape_values[0] : 1;
    unsigned moved = 1;
    switch (c) {
    case 'A': y = count > y ? 0 : y - count; break;
    case 'B': y += count; break;
    case 'C': x += count; break;
    case 'D': x = count > x ? 0 : x - count; break;
    case 'H': case 'f': y = count - 1; x = escape_values[1] ? escape_values[1] - 1 : 0; break;
    case 'J': console_clear(console_margin+x*cell_width, console_y, escape_values[0]); moved = 0; break;
    case 'K': {
        unsigned mode = escape_values[0], right = console_margin + console_columns*cell_width;
        unsigned start = mode ? console_margin : console_margin+x*cell_width;
        unsigned end = mode == 1 ? console_margin+(x+1)*cell_width : right;
        if (mode <= 2) rectangle(start, console_y, end-start, cell_height, console_paper());
        moved = 0; break;
    }
    case 'X':
        if (count > console_columns-x) count = console_columns-x;
        rectangle(console_margin+x*cell_width, console_y, count*cell_width, cell_height, console_paper());
        moved = 0; break;
    default: return;
    }
    if (moved) {
        if (x >= console_columns) x = console_columns - 1;
        if (y >= console_rows) y = console_rows - 1;
        console_x = console_margin + x * cell_width;
        console_y = console_top + y * cell_height;
    }
}
static int console_escape(unsigned c) {
    if (c == 24 || c == 26) { escape_state = 0; return 1; } /* CAN / SUB */
    if (escape_state == ESC_OSC || escape_state == ESC_STRING) {
        if (c == 27) escape_state = escape_state == ESC_OSC ? ESC_OSC_END : ESC_STRING_END;
        else if (c == 7 && escape_state == ESC_OSC) escape_state = 0;
        return 1;
    }
    if (escape_state == ESC_OSC_END || escape_state == ESC_STRING_END) {
        if (c == '\\' || (c == 7 && escape_state == ESC_OSC_END)) escape_state = 0;
        else if (c != 27) escape_state = escape_state == ESC_OSC_END ? ESC_OSC : ESC_STRING;
        return 1;
    }
    if (c == 27) { escape_state = ESC_START; utf8_remaining = 0; return 1; }
    if (!escape_state) return 0;
    if (c < 32) return 0; /* C0 controls still work inside a split sequence. */
    if (c == 127) return 1;
    if (escape_state == ESC_START) {
        if (c == '[') {
            escape_state = ESC_CSI;
            escape_count = escape_private = escape_intermediate = escape_bad = escape_parameters = 0;
            memset(escape_values, 0, sizeof(escape_values));
        } else if (c == '(' || c == ')') { escape_charset = c == ')'; escape_state = ESC_CHARSET; }
        else if (c == ']') escape_state = ESC_OSC;
        else if (c == 'P' || c == '^' || c == '_') escape_state = ESC_STRING;
        else escape_state = c >= 0x20 && c <= 0x2f ? ESC_INTERMEDIATE : 0;
        return 1;
    }
    if (escape_state == ESC_CHARSET) {
        if (c == '0' || c == 'B') {
            if (escape_charset) console_g1 = c == '0'; else console_g0 = c == '0';
        }
        escape_state = c >= 0x20 && c <= 0x2f ? ESC_INTERMEDIATE : 0;
        return 1;
    }
    if (escape_state == ESC_INTERMEDIATE) {
        if (c < 0x20 || c > 0x2f) escape_state = 0;
        return 1;
    }
    if (c >= 0x40 && c <= 0x7e) { console_csi(c); escape_state = 0; return 1; }
    if (c >= '0' && c <= '9') {
        unsigned *value = &escape_values[escape_count];
        escape_parameters = 1;
        if (escape_intermediate || *value > (65535U-(c-'0'))/10) escape_bad = 1;
        else *value = *value*10+c-'0';
    } else if (c == ';') {
        escape_parameters = 1;
        if (escape_intermediate || escape_count+1 >= sizeof(escape_values)/sizeof(*escape_values)) escape_bad = 1;
        else ++escape_count;
    } else if (c >= 0x3c && c <= 0x3f) {
        if (escape_private || escape_parameters || escape_intermediate) escape_bad = 1;
        else escape_private = c;
    } else if (c >= 0x20 && c <= 0x2f) escape_intermediate = 1;
    else escape_bad = 1;
    return 1;
}
static void console_newline(void) {
    console_x = console_margin;
    console_y += cell_height;
    if (console_y < console_bottom) return;
    unsigned line_height = cell_height;
    uint32_t *pixels = shadow;
    unsigned width = boot.width - 2 * console_margin;
    for (unsigned y = console_top; y < console_bottom - line_height; ++y)
        pixel_copy(pixels + (size_t)y * boot.width + console_margin,
                   pixels + (size_t)(y + line_height) * boot.width + console_margin, width);
    ++scroll_count;
    damage(console_margin, console_top, width, console_bottom - console_top);
    console_y = console_bottom - line_height;
    rectangle(console_margin, console_y, boot.width - 2 * console_margin,
              line_height, console_paper());
}
static uint32_t console_dec_graphics(uint32_t code) {
    switch (code) {
    case 'q': return 0x2500; case 'x': return 0x2502;
    case 'l': return 0x250c; case 'k': return 0x2510;
    case 'm': return 0x2514; case 'j': return 0x2518;
    case 't': return 0x251c; case 'u': return 0x2524;
    case 'w': return 0x252c; case 'v': return 0x2534;
    case 'n': return 0x253c; default: return code;
    }
}
static int console_box(uint32_t code, uint32_t foreground) {
    unsigned sides;
    switch (code) {
    case 0x2500: sides=3; break; case 0x2502: sides=12; break;
    case 0x250c: sides=10; break; case 0x2510: sides=9; break;
    case 0x2514: sides=6; break; case 0x2518: sides=5; break;
    case 0x251c: sides=14; break; case 0x2524: sides=13; break;
    case 0x252c: sides=11; break; case 0x2534: sides=7; break;
    case 0x253c: sides=15; break; default: return 0;
    }
    /* The existing smooth atlas has only the banner's double-line slots.
     * Cell-aligned strokes give both atlases gapless single-line TUI borders. */
    unsigned stroke=console_scale, x=console_x+(cell_width-stroke)/2, y=console_y+(cell_height-stroke)/2;
    if (sides&1) rectangle(console_x,y,x-console_x+stroke,stroke,foreground);
    if (sides&2) rectangle(x,y,console_x+cell_width-x,stroke,foreground);
    if (sides&4) rectangle(x,console_y,stroke,y-console_y+stroke,foreground);
    if (sides&8) rectangle(x,y,stroke,console_y+cell_height-y,foreground);
    return 1;
}
static void console_printable(uint32_t code) {
    if (utf8_remaining) {
        if ((code & 0xc0) != 0x80) { utf8_remaining = 0; code = '?'; }
        else {
            utf8_code = (utf8_code << 6) | (code & 63);
            if (--utf8_remaining) return;
            code = utf8_code;
        }
    } else if (code >= 0xc2 && code < 0xe0) {
        utf8_code = code & 31; utf8_remaining = 1; return;
    } else if (code >= 0xe0 && code < 0xf0) {
        utf8_code = code & 15; utf8_remaining = 2; return;
    } else if (code >= 0xf0 && code <= 0xf4) {
        utf8_code = code & 7; utf8_remaining = 3; return;
    }
    if (code < 128 && (console_shift ? console_g1 : console_g0)) code = console_dec_graphics(code);
    unsigned right = console_margin+console_columns*cell_width;
    if (console_x >= right) {
        if (console_wrap) console_newline(); else console_x = right-cell_width;
    }
    uint32_t foreground = console_reverse ? console_background : console_ink();
    uint32_t background = console_paper();
    rectangle(console_x, console_y, cell_width, cell_height, background);
    if (!console_box(code, foreground)) {
        /* Both atlases retain CP437 slots for the banner's box characters. */
        switch (code) {
        case 0x2588: code = 0xdb; break;
        case 0x2557: code = 0xbb; break;
        case 0x2554: code = 0xc9; break;
        case 0x255a: code = 0xc8; break;
        case 0x255d: code = 0xbc; break;
        case 0x2550: code = 0xcd; break;
        case 0x2551: code = 0xba; break;
        default: if (code >= 128) code = '?'; break;
        }
        char value[2] = {(char)code, 0};
        text_on(console_x, console_y, value, console_scale, foreground, background);
    }
    if (console_underline)
        rectangle(console_x, console_y+cell_height-console_scale, cell_width, console_scale, foreground);
    console_x += cell_width;
    if (!console_wrap && console_x >= right) console_x = right-cell_width;
}
static void console_put(char c) {
    if (console_escape((uint8_t)c)) return;
    if (c == '\r') { console_x = console_margin; return; }
    if (c == '\n') { console_newline(); return; }
    if (c == '\b') { if (console_x >= console_margin + cell_width) console_x -= cell_width; return; }
    if (c == '\t') {
        unsigned count = 8 - ((console_x - console_margin) / cell_width) % 8;
        /* Generated padding is output, not additional input to a split CSI. */
        while (count--) console_printable(' ');
        return;
    }
    if (c == 14 || c == 15) { console_shift = c == 14; return; }
    if ((uint8_t)c < 32 || c == 127) return;
    console_printable((uint8_t)c);
}
void console_write(char c) {
    serial_char(c);
    if (!console_ready) return;
    console_put(c);
    if (rdtsc() - last_present >= boot.tsc_hz / 60) console_flush();
}
/* Fixed six-column marker plus one separator. Serial stays plain text;
 * only the framebuffer marker receives a color, independent of app ANSI. */
void console_boot_status(unsigned status, const char *message, const char *detail) {
    const char *marker = status == 0 ? "..... " : status == 1 ? "[ OK ]" : "[ERR ]";
    uint32_t saved = console_foreground, background = console_background;
    unsigned bold=console_bold, reverse=console_reverse, underline=console_underline, index=console_fg_index;
    unsigned g0=console_g0, g1=console_g1, shift=console_shift;
    console_bold=console_reverse=console_underline=0; console_fg_index=16;
    console_g0=console_g1=console_shift=0; console_background=color(15,26,43);
    console_foreground = status == 1 ? color(60, 205, 110) :
                         status == 0 ? saved : color(245, 95, 95);
    while (*marker) console_write(*marker++);
    console_foreground = saved;
    console_write(' ');
    while (*message) console_write(*message++);
    if (detail && *detail) {
        console_write(':'); console_write(' ');
        while (*detail) console_write(*detail++);
    }
    console_write('\n'); console_flush();
    console_foreground=saved; console_background=background; console_fg_index=index;
    console_bold=bold; console_reverse=reverse; console_underline=underline;
    console_g0=g0; console_g1=g1; console_shift=shift;
}
void screen_status(const char *message) {
    unsigned top = (font_height + 24) * console_scale;
    rectangle(console_margin, top, boot.width - 2 * console_margin,
              2 * cell_height, color(15, 26, 43));
    for (unsigned row = 0; *message && row < 2; ++row) {
        unsigned count = 0, last_space = 0;
        while (count < console_columns && message[count]) {
            if (message[count] == ' ') last_space = count;
            ++count;
        }
        if (message[count] && last_space) count = last_space;
        for (unsigned column = 0; column < count; ++column) {
            char value[2] = {message[column], 0};
            text(console_margin + column * cell_width, top + row * cell_height,
                 value, console_scale, color(160, 225, 195));
        }
        message += count;
        while (*message == ' ') ++message;
    }
    console_flush();
}
void console_init(void) {
    if (!boot.framebuffer || boot.pixel_format > 1 || boot.framebuffer % 4 ||
        boot.width < 128 || boot.width > 16384 || boot.height < 128 || boot.height > 16384 ||
        boot.stride < boot.width || boot.stride > 65536 ||
        (uint64_t)boot.stride * boot.height * 4 > boot.framebuffer_size)
        fail("framebuffer mapping", boot.framebuffer);
    console_ready = cursor_presented = 0; cursor_visible = 1;
    /* MMIO addresses are native physical addresses, including GOP apertures
     * above 64 GiB. Map just the visible span; never enlarge the RAM allocator
     * or reinterpret these addresses as C16 pointers. Registers remain UC. */
    framebuffer_map(boot.framebuffer, (size_t)boot.stride * boot.height * 4);
    size_t required = (size_t)boot.width * boot.height * 4;
    if (required > shadow_bytes) {
        unsigned pages = (required + 4095) / 4096;
        uint64_t allocation = pages_alloc(pages);
        if (!allocation) fail("console RAM allocation", required);
        if (shadow) for (size_t i = 0; i < shadow_bytes; i += 4096) page_free((uint64_t)shadow + i);
        shadow = (void *)allocation; shadow_bytes = (size_t)pages * 4096;
    }
    dirty = 0; escape_state = utf8_remaining = 0;
    console_g0 = console_g1 = console_shift = 0; console_wrap = 1;
    console_attributes_reset();
    serial("FRAMEBUFFER_MAP base="); hex(boot.framebuffer); serial("\n");
#ifdef BOOT_FONT_ATLAS
    const uint32_t *header = (const void *)font_payload_start;
    if (font_payload_end - font_payload_start < 16 || header[0] != 0x31465447 ||
        !header[1] || header[1] > 64 || header[2] < 12 || header[2] > 48 || header[3] != 256 ||
        (size_t)(font_payload_end - font_payload_start) != 16 + (size_t)header[1] * header[2] * 256)
        fail("gterm font atlas", 0);
    font_width = header[1]; font_height = header[2]; font_alpha = font_payload_start + 16;
#endif
    /* The smooth gterm atlas is already rendered at its intended size.
     * Increase only when a larger display leaves room for a full console. */
    console_scale = CONSOLE_SCALE;
    if (!console_scale) {
        console_scale = boot.width / (100 * font_width);
        if (console_scale > boot.height / (32 * font_height)) console_scale = boot.height / (32 * font_height);
        if (console_scale > 8) console_scale = 8;
    }
    /* Even a manual override must leave room for the header and console. */
    unsigned limit = boot.width / (20 * font_width + 32);
    if (limit > boot.height / (7 * font_height + 52)) limit = boot.height / (7 * font_height + 52);
    if (console_scale > limit) console_scale = limit;
    if (!console_scale) console_scale = 1;
    console_margin = 16 * console_scale;
    cell_width = font_width * console_scale; cell_height = font_height * console_scale;
    console_top = (3 * font_height + 36) * console_scale;
    if (boot.height <= console_top + console_margin) fail("framebuffer too small for font", font_height);
    console_columns = (boot.width - 2 * console_margin) / cell_width;
    console_rows = (boot.height - console_margin - console_top) / cell_height;
    if (!console_columns || !console_rows) fail("framebuffer console dimensions", console_scale);
    console_bottom = console_top + console_rows * cell_height;
    console_x = console_margin; console_y = console_top;
    rectangle(0, 0, boot.width, boot.height, color(15, 26, 43));
    rectangle(console_margin, 8 * console_scale, boot.width - 2 * console_margin,
              2 * console_scale, color(45, 210, 190));
    rectangle(console_margin, 16 * console_scale, 4 * console_scale,
              cell_height, color(60, 205, 110));
    console_flush();
    if (((volatile uint32_t *)boot.framebuffer)[(size_t)(16 * console_scale) * boot.stride + console_margin] != color(60, 205, 110))
        fail("framebuffer readback", 0);
    text(24 * console_scale, 16 * console_scale, "Riplix x86_64", console_scale, color(230, 240, 250));
    screen_status("Own stack, exception tables, page tables and physical allocator ready");
    serial("FRAMEBUFFER_CONSOLE scale="); hex(console_scale);
    serial(" cell_width="); hex(cell_width); serial(" cell_height="); hex(cell_height);
    serial(" columns="); hex(console_columns); serial(" rows="); hex(console_rows); serial("\n");
    serial("CONSOLE_GEOMETRY rows="); hex(console_rows);
    serial(" columns="); hex(console_columns); serial("\n");
    serial("FRAMEBUFFER_OK\n");
    console_ready = 1;
    console_flush();
}

#ifdef BOOT_TEST
/* Compare the former read/modify-video scroll with eight RAM scrolls and one
 * presentation. Same pixels, no serial output inside the timed intervals. */
void console_check(void) {
    unsigned saved_cursor_visible = cursor_visible;
    cursor_visible = 0; console_flush();
    unsigned width = boot.width - 2 * console_margin;
    unsigned rounds = console_rows > 8 ? 8 : 1;
    volatile uint32_t *video = (void *)boot.framebuffer;
    for (unsigned y = console_top; y < console_bottom; ++y)
        rectangle(console_margin, y, width, 1, 0x100000 + y);
    console_flush();
    uint64_t begin = rdtsc();
    for (unsigned n = 0; n < rounds; ++n) {
        for (unsigned y = console_top; y < console_bottom - cell_height; ++y)
            for (unsigned x = console_margin; x < boot.width - console_margin; ++x)
                video[(size_t)y * boot.stride + x] = video[(size_t)(y + cell_height) * boot.stride + x];
        for (unsigned y = console_bottom - cell_height; y < console_bottom; ++y)
            for (unsigned x = console_margin; x < boot.width - console_margin; ++x)
                video[(size_t)y * boot.stride + x] = color(15, 26, 43);
    }
    __asm__ volatile("mfence" : : : "memory");
    uint64_t legacy = rdtsc() - begin;
    /* Shadow still holds the original pattern. */
    begin = rdtsc();
    for (unsigned n = 0; n < rounds; ++n) {
        console_y = console_bottom - cell_height;
        console_newline();
    }
    console_flush();
    uint64_t batched = rdtsc() - begin;
    for (unsigned y = console_top; y < console_bottom; ++y)
        for (unsigned x = console_margin; x < boot.width - console_margin; ++x) {
            uint32_t expected = y < console_bottom - rounds * cell_height ? 0x100000 + y + rounds * cell_height : color(15, 26, 43);
            if (shadow[(size_t)y * boot.width + x] != expected || video[(size_t)y * boot.stride + x] != expected)
                fail("console scroll pixels", (uint64_t)y << 32 | x);
        }
    rectangle(console_margin, console_top, width, console_bottom - console_top, color(15, 26, 43));
    console_x = console_margin; console_y = console_top; console_flush();
    serial("CONSOLE_SCROLL_CHECK_OK rounds="); hex(rounds);
    serial(" legacy_tsc="); hex(legacy); serial(" batched_tsc="); hex(batched); serial("\n");
    cursor_visible = saved_cursor_visible; console_flush();
}
#endif
