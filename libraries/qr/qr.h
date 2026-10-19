/* SPDX-License-Identifier: MIT */
#ifndef RIPPLE_QR_H
#define RIPPLE_QR_H
/* Bounded byte-mode QR, medium ECC, versions 1..5. No I/O or host services.
 * One module per column, two module rows per terminal row. Includes a four
 * module quiet zone on every side. At most 45 columns by 23 terminal rows. */
#define QR_MAX_BYTES 84
#define QR_MAX_COLUMNS 45
#define QR_UPPER_BLOCK 0x2580
#define QR_LOWER_BLOCK 0x2584
#define QR_FULL_BLOCK 0x2588
typedef struct Qr Qr;
/* Copies octets; NULL on invalid input/limit or allocation failure.
 * C16 callers must supply cells in 0..255. Empty payloads are permitted. */
Qr *qr_new(const unsigned char *bytes, unsigned length);
void qr_free(Qr *qr);
unsigned qr_columns(const Qr *qr);
unsigned qr_rows(const Qr *qr);
/* All-or-nothing row write. Returns columns, or -1 for invalid arguments.
 * Style retains only the curses color pair. Use black foreground on white.
 * Cell low 21 bits contain space or one of QR_*_BLOCK; high bits hold the color pair. */
int qr_row(const Qr *qr, unsigned row, unsigned long *cells, unsigned capacity, unsigned long style);
#endif
