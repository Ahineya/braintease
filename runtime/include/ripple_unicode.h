/* SPDX-License-Identifier: MIT */
#ifndef RIPPLE_UNICODE_H
#define RIPPLE_UNICODE_H
#include <stdint.h>
#include <stddef.h>
#define RU_REPLACEMENT 0xfffdUL
#define RU_CONTINUATION 0x1fffffUL
#define RU_CLUSTER_BASE 0x110000UL
#define RU_CLUSTER_LIMIT 512
#define RU_CLUSTER_SCALARS 16
#define RU_OTHER 0
#define RU_CR 1
#define RU_LF 2
#define RU_CONTROL 3
#define RU_EXTEND 4
#define RU_ZWJ 5
#define RU_RI 6
#define RU_PREPEND 7
#define RU_SPACING 8
#define RU_L 9
#define RU_V 10
#define RU_T 11
#define RU_LV 12
#define RU_LVT 13
struct ru_decoder { uint32_t value, minimum; unsigned need; };
struct ru_break { unsigned started, previous, regional, pictographic, zwj, conjunct; };
/* 0: incomplete; 1: scalar/replacement consumed; -1: replacement, retry octet.
 * No allocation; callers retain state across reads and reset on stream reset. */
int ru_decode(struct ru_decoder *, unsigned, uint32_t *);
unsigned ru_encode(uint32_t, unsigned char *);
uint32_t ru_next(const unsigned char *, size_t, size_t *);
unsigned ru_gcb(uint32_t);
unsigned ru_pictographic(uint32_t);
unsigned ru_incb(uint32_t);
int ru_width(uint32_t);
/* Returns true when this scalar continues the previous extended grapheme. */
int ru_join(struct ru_break *, uint32_t);
size_t ru_next_grapheme(const unsigned char *, size_t, size_t, unsigned *);
size_t ru_previous_grapheme(const unsigned char *, size_t, size_t);
size_t ru_prefix(const unsigned char *, size_t, size_t);
size_t ru_columns(const unsigned char *, size_t);
/* Stable, bounded display-only interning. Original application UTF-8 is retained.
 * Exhaustion/overlong clusters render replacement; no live ID is ever evicted. */
uint32_t ru_append(uint32_t, uint32_t);
unsigned ru_scalar_count(uint32_t);
uint32_t ru_scalar(uint32_t, unsigned);
int ru_glyph_width(uint32_t);
/* Shared Unicode bitmap font: ROM ASCII is retained by the graphical clients. */
unsigned ru_font_row(uint32_t, unsigned);
#endif
