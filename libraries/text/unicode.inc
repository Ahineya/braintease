/* Gap-aware adapter to the shared Unicode engine. No whole-line copy or
 * grapheme allocation is needed for motion/width; only drawing interns glyphs. */
static uint32_t scalar_at(TextBuffer *b, unsigned long at, unsigned long end, unsigned long *next) {
    unsigned char bytes[4]; size_t n = 0, used = 0;
    while (n < 4 && at + n < end) { bytes[n] = text_byte(b, at + n); ++n; }
    if (!n) { *next = at; return RU_REPLACEMENT; }
    { uint32_t cp = ru_next(bytes, n, &used); *next = at + used; return cp; }
}
static unsigned long cluster(TextBuffer *b, unsigned long at, unsigned long end, unsigned *width, uint32_t *glyph) {
    struct ru_break state = {0,0,0,0,0,0};
    unsigned long p = at, next; unsigned total = 0, scalars = 0;
    uint32_t cp, value = 0; int w, failed = 0, ch = text_byte(b, at);
    if (at >= end) { if (width) *width = 0; if (glyph) *glyph = ' '; return end; }
    if (ch >= 32 && ch < 127 && (at + 1 == end || text_byte(b, at + 1) < 128)) {
        if (width) *width = 1; if (glyph) *glyph = ch; return at + 1;
    }
    while (p < end) {
        cp = scalar_at(b, p, end, &next);
        if (!ru_join(&state, cp) && p != at) break;
        if (width || glyph) {
            w = ru_width(cp);
            if (w > 0 && (unsigned)w > total) total = w;
            if (cp == 0xfe0f || cp == 0x20e3 || state.regional >= 2) total = 2;
            if (glyph && !failed) {
                if (!scalars) value = w < 0 ? '?' : !w ? ru_append(0x25cc, cp) : cp;
                else { value = ru_append(value, cp); if (value == RU_REPLACEMENT) failed = 1; }
            }
        }
        ++scalars; p = next;
    }
    if (width) *width = total ? total : 1;
    if (glyph) *glyph = value;
    return p;
}
unsigned long text_boundary(TextBuffer *b, unsigned long at) {
    unsigned long p, next, len = text_length(b); int previous, current;
    if (at > len) at = len;
    if (!at) return 0;
    previous = text_byte(b, at - 1); current = text_byte(b, at);
    if (previous < 128 && current < 128 && !(previous == '\r' && current == '\n')) return at;
    p = text_start(b, text_row(b, at));
    while (p < at) { next = cluster(b, p, len, NULL, NULL); if (next > at) break; p = next; }
    return p;
}
unsigned long text_next(TextBuffer *b, unsigned long at) {
    return cluster(b, text_boundary(b, at), text_length(b), NULL, NULL);
}
unsigned long text_previous(TextBuffer *b, unsigned long at) {
    unsigned long p, next, len = text_length(b);
    if (at > len) at = len;
    if (!at) return 0;
    if (text_byte(b, at - 1) >= 32 && text_byte(b, at - 1) < 127 &&
        (at == 1 || text_byte(b, at - 2) < 128)) return at - 1;
    p = text_start(b, text_row(b, at));
    if (p == at) return at > 1 && text_byte(b, at - 2) == '\r' ? at - 2 : at - 1;
    while (p < at) { next = cluster(b, p, len, NULL, NULL); if (next >= at) return p; p = next; }
    return p;
}
unsigned long text_step(TextBuffer *b, unsigned long at, int count) {
    at = text_boundary(b, at);
    while (count > 0 && at < text_length(b)) { at = cluster(b, at, text_length(b), NULL, NULL); --count; }
    while (count < 0 && at) { at = text_previous(b, at); ++count; }
    return at;
}
static int unicode_space(uint32_t cp) {
    return cp == ' ' || (cp >= 9 && cp <= 13) || cp == 0x85 || cp == 0xa0 ||
        cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 ||
        cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000;
}
