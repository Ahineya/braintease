#include "../text/text.h"
#include "../../runtime/include/ripple_unicode.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void drawing(void) {
    const unsigned char *s = (const unsigned char *)"AЖé中👩‍💻Z\r\n";
    unsigned long styles[7] = {0, 1UL<<22, 1UL<<23, 1UL<<24, 1UL<<26, 2UL<<26, 3UL<<26};
    unsigned long cells[12], split, n = strlen((const char *)s), selected = 1UL<<25;
    TextBuffer *b = text_new();
    assert(b && !text_append(b, s, n));
    assert(text_visual(b, text_endline(b, 0), 4) == 8);
    assert(text_column(b, 0, 4, 4) == 6);
    for (split = 0; split <= n; ++split) {
        /* Move the physical gap through every UTF-8 octet and CRLF boundary. */
        assert(!text_replace(b, split, 0, (const unsigned char *)"!", 1));
        assert(!text_replace(b, split, 1, NULL, 0));
        assert(!text_render(b, 0, 0, 8, 4, styles, selected, 6, 9, cells));
        assert(cells[0] == 'A' && cells[1] == 0x416);
        assert(ru_scalar_count(cells[2]) == 2 && ru_scalar(cells[2], 0) == 'e' && ru_scalar(cells[2], 1) == 0x301);
        assert(cells[3] == (selected | 0x4e2d) && cells[4] == (selected | RU_CONTINUATION));
        assert(ru_scalar_count(cells[5]) == 3 && ru_scalar(cells[5], 0) == 0x1f469 && ru_scalar(cells[5], 1) == 0x200d && ru_scalar(cells[5], 2) == 0x1f4bb);
        assert(cells[6] == RU_CONTINUATION && cells[7] == 'Z' && !text_overflow(b));
        assert(!text_render(b, 0, 4, 3, 4, styles, selected, 0, 0, cells));
        assert(cells[0] == ' ' && ru_scalar_count(cells[1]) == 3 && cells[2] == RU_CONTINUATION && text_overflow(b));
        assert(!text_render(b, 0, 0, 4, 4, styles, selected, 0, 0, cells));
        assert(cells[3] == ' ' && text_overflow(b));
    }
    text_free(b);
    b = text_new();
    assert(b && !text_append(b, (const unsigned char *)"中\tX", 5));
    assert(text_visual(b, 5, 4) == 5 && text_column(b, 0, 3, 4) == 3);
    text_free(b);
}
static void navigation(void) {
    const char *s = "AЖé中👩‍💻Z\r\n";
    const unsigned long boundaries[] = {0, 1, 3, 6, 9, 20, 21, 23};
    TextBuffer *b = text_new(); unsigned i; unsigned long p, bounds[2];
    assert(b && !text_append(b, (const unsigned char *)s, strlen(s)));
    for (i = 0; i + 1 < sizeof(boundaries)/sizeof(boundaries[0]); ++i) {
        assert(text_next(b, boundaries[i]) == boundaries[i+1]);
        assert(text_previous(b, boundaries[i+1]) == boundaries[i]);
        for (p = boundaries[i]; p < boundaries[i+1]; ++p) assert(text_boundary(b, p) == boundaries[i]);
    }
    assert(text_next(b, 23) == 23 && text_previous(b, 0) == 0);
    assert(text_step(b, 1, 4) == 20 && text_step(b, 20, -4) == 1);
    assert(text_find_char(b, 0, 0x4e2d, 0, 0, 1) == 6);
    assert(text_find_char(b, 0, 0x4e2d, 0, 1, 1) == 3);
    assert(text_find_char(b, 20, 0x1f4bb, 1, 0, 1) == 9);
    assert(text_find_char(b, 20, 0x1f4bb, 1, 1, 1) == 20);
    assert(text_find_char(b, 0, 'A', 1, 0, 1) < 0);
    text_free(b);
    s = "Жé слово\xc2\xa0中🙂!";
    b = text_new(); assert(b && !text_append(b, (const unsigned char *)s, strlen(s)));
    assert(text_word(b, 0, 'e', 1) == 2);
    assert(text_word(b, 0, 'w', 1) == 6);
    assert(text_word(b, 6, 'b', 1) == 0);
    assert(text_word(b, 6, 'w', 1) == 18);
    assert(!text_object(b, 2, 'w', 0, bounds) && bounds[0] == 0 && bounds[1] == 5);
    text_free(b);
}
static void malformed(void) {
    const unsigned char s[] = {0xf0, '(', 0x8c, '(', 27, '\t'};
    unsigned char copy[sizeof(s)]; unsigned long cells[8], styles[7] = {0,0,0,0,0,0,0};
    TextBuffer *b = text_new();
    assert(b && !text_append(b, s, sizeof(s)));
    assert(text_visual(b, sizeof(s), 4) == 8);
    assert(!text_render(b, 0, 0, 8, 4, styles, 0, 0, 0, cells));
    assert(cells[0] == RU_REPLACEMENT && cells[1] == '(' && cells[2] == RU_REPLACEMENT && cells[3] == '(' && cells[4] == '?');
    assert(!text_copy(b, 0, sizeof(s), copy) && !memcmp(copy, s, sizeof(s)));
    text_free(b);
}
static void long_word(void) {
    unsigned char data[2049]; unsigned i; unsigned long bounds[2];
    TextBuffer *b = text_new();
    for (i = 0; i < 2048; i += 2) { data[i] = 0xd0; data[i+1] = 0x96; }
    data[2048] = '!';
    assert(b && !text_append(b, data, sizeof(data)));
    assert(text_word(b, 0, 'e', 1) == 2046 && text_word(b, 2048, 'b', 1) == 0);
    assert(!text_object(b, 1000, 'w', 0, bounds) && bounds[0] == 0 && bounds[1] == 2048);
    assert(text_find_char(b, 2048, 0x416, 1, 0, 1) == 2046);
    assert(text_find_char(b, 2048, 0x416, 1, 0, 1024) == 0);
    assert(text_find_char(b, 0, 0x416, 0, 0, 1023) == 2046);
    text_free(b);
}
int main(void) {
    drawing(); navigation(); malformed(); long_word();
    puts("Text Unicode: grapheme rendering, wide clipping, selection, tabs and split gap passed");
    return 0;
}
