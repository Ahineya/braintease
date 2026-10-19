/* SPDX-License-Identifier: MIT */
#include "qr.h"
#include "vendor/qrcodegen.h"
#include <stdlib.h>
#include <limits.h>

/* Keep the portable engine embedded for RFL's C-source import mechanism. */
#include "vendor/qrcodegen.c"
#define QR_BUFFER qrcodegen_BUFFER_LEN_FOR_VERSION(5)
struct Qr { uint_least8_t modules[QR_BUFFER]; };

Qr *qr_new(const unsigned char *bytes, unsigned length) {
    Qr *qr;
    unsigned i;
    uint_least8_t temporary[QR_BUFFER];
    if (length > QR_MAX_BYTES || (length && !bytes)) return NULL;
    for (i = 0; i < length; ++i) {
#if CHAR_BIT > 8
        if (bytes[i] > 255) return NULL;
#endif
        temporary[i] = bytes[i];
    }
    qr = malloc(sizeof(*qr));
    if (!qr) return NULL;
    if (!qrcodegen_encodeBinary(temporary, length, qr->modules,
            qrcodegen_Ecc_MEDIUM, 1, 5, qrcodegen_Mask_AUTO, false)) {
        free(qr); return NULL;
    }
    return qr;
}
void qr_free(Qr *qr) { free(qr); }
unsigned qr_columns(const Qr *qr) { return qr ? qrcodegen_getSize(qr->modules) + 8 : 0; }
unsigned qr_rows(const Qr *qr) { return (qr_columns(qr) + 1) / 2; }
static int dark(const Qr *qr, int x, int y) {
    return qrcodegen_getModule(qr->modules, x - 4, y - 4);
}
int qr_row(const Qr *qr, unsigned row, unsigned long *cells, unsigned capacity, unsigned long style) {
    unsigned x, width = qr_columns(qr), top, bottom, ch;
    if (!qr || !cells || row >= qr_rows(qr) || capacity < width) return -1;
    style &= 0xfc000000UL;
    for (x = 0; x < width; ++x) {
        top = dark(qr, x, row * 2);
        bottom = dark(qr, x, row * 2 + 1);
        ch = top ? (bottom ? QR_FULL_BLOCK : QR_UPPER_BLOCK) : (bottom ? QR_LOWER_BLOCK : ' ');
        cells[x] = style | ch;
    }
    return width;
}
