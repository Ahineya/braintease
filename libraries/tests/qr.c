/* SPDX-License-Identifier: MIT */
#include "../qr/qr.h"
#include "../../ports/graphics/terminal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

static void dump(Qr *qr) {
    unsigned y, x;
    unsigned long cells[QR_MAX_COLUMNS];
    puts("QR_BEGIN");
    for (y = 0; y < qr_rows(qr); ++y) {
        assert(qr_row(qr, y, cells, QR_MAX_COLUMNS, 0) == (int)qr_columns(qr));
        for (x = 0; x < qr_columns(qr); ++x)
            putchar(cells[x] == QR_UPPER_BLOCK ? 'U' : cells[x] == QR_LOWER_BLOCK ? 'L' : cells[x] == QR_FULL_BLOCK ? '#' : '.');
        putchar('\n');
    }
    puts("QR_END");
}
int main(int argc, char **argv) {
    Qr *qr;
    unsigned n, y, x;
    unsigned char bytes[85];
    unsigned long cells[QR_MAX_COLUMNS + 2];
    if (argc == 2 && !strcmp(argv[1], "--screen")) {
        static struct vt terminal;
        int c;
        vt_init(&terminal);
        /* Deliberately feed one byte at a time, including fragmented UTF-8. */
        while ((c = getchar()) != EOF) { char b = c; vt_feed(&terminal, &b, 1); }
        for (n = 0; n < 80 * 30; ++n)
            printf("%u:%u\n", vt_cells(&terminal)[n].ch, vt_cells(&terminal)[n].colour);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--dump")) {
        qr = qr_new((const unsigned char *)argv[2], strlen(argv[2]));
        if (!qr) return 1;
        dump(qr); qr_free(qr); return 0;
    }
    assert(!qr_new(NULL, 1) && !qr_new(bytes, 85));
    assert(!qr_columns(NULL) && !qr_rows(NULL));
    assert(qr_row(NULL, 0, cells, QR_MAX_COLUMNS, 0) == -1);
    for (n = 0; n <= QR_MAX_BYTES; ++n) {
        for (x = 0; x < n; ++x) bytes[x] = (x * 73 + n * 17) & 255;
        qr = qr_new(bytes, n); assert(qr);
        assert(qr_columns(qr) <= 45 && qr_rows(qr) <= 23);
        assert(qr_columns(qr) % 4 == 1);
        for (y = 0; y < qr_rows(qr); ++y) {
            for (x = 0; x < QR_MAX_COLUMNS + 2; ++x) cells[x] = 0x12345678UL;
            assert(qr_row(qr, y, cells + 1, qr_columns(qr) - 1, 0) == -1);
            for (x = 0; x < QR_MAX_COLUMNS + 2; ++x) assert(cells[x] == 0x12345678UL);
            assert(qr_row(qr, y, cells + 1, QR_MAX_COLUMNS, 0x1c200000UL) == (int)qr_columns(qr));
            assert(cells[0] == 0x12345678UL && cells[qr_columns(qr) + 1] == 0x12345678UL);
            for (x = 0; x < qr_columns(qr); ++x) {
                unsigned ch = cells[x + 1] & 0x001fffffUL;
                assert((cells[x + 1] & ~0x001fffffUL) == 0x1c000000UL);
                assert(ch == ' ' || ch == QR_UPPER_BLOCK || ch == QR_LOWER_BLOCK || ch == QR_FULL_BLOCK);
                if (x < 4 || x >= qr_columns(qr) - 4 || y < 2 || y >= qr_rows(qr) - 2) assert(ch == ' ');
            }
        }
        assert(qr_row(qr, qr_rows(qr), cells, QR_MAX_COLUMNS, 0) == -1);
        assert(qr_row(qr, 0, NULL, QR_MAX_COLUMNS, 0) == -1);
        qr_free(qr);
    }
#if CHAR_BIT > 8
    bytes[0] = 256; assert(!qr_new(bytes, 1));
#endif
    qr_free(NULL);
    puts("QR_OK: binary payloads 0..84, limits, atomic rows, guards, quiet zone and color isolation");
    return 0;
}
