/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_TERMINAL_H
#define RIPPLE_TERMINAL_H
#include <stdint.h>
#include "../../runtime/include/ripple_unicode.h"
#define VT_MAX_COLS 240
#define VT_MAX_ROWS 100
#define VT_DEFAULT_COLS 80
#define VT_DEFAULT_ROWS 30
/* Internal cell, not a wire ABI: 21 glyph bits + 11 style bits fit 32 bits.
 * Keep scrolling/copy storage the same size as the original 8-bit terminal. */
struct vt_cell { uint32_t ch:21, colour:11; };
typedef char vt_cell_is_32_bits[sizeof(struct vt_cell)==sizeof(uint32_t)?1:-1];
struct vt {
    /* Two owned, dynamically sized screens, each with a stride of cols. */
    struct vt_cell *cells[2];
    unsigned cols,rows;
    unsigned x,y,sx,sy,primary_x,primary_y,top,bottom;
    unsigned fg,bg,bold,reverse,visible,wrap,pending,alternate,appcursor;
    unsigned parser,private_mode,count,params[16],intermediate;
    unsigned underline,blink,cursor_shape,bracketed_paste;
    unsigned synchronized;
    unsigned g0,g1,shift,charset_target;
    struct ru_decoder utf8;
    struct ru_break grapheme;
    unsigned last_at; /* Last grapheme head plus one; zero after cursor controls. */
    unsigned char dirty[VT_MAX_ROWS];
    void (*scroll)(struct vt *,int);
    void (*reply)(struct vt *,const char *);
    void *user;
};
void vt_init(struct vt *);
/* Release owned screen buffers. Destroy before reinitializing a model. */
void vt_destroy(struct vt *);
/* Invalid dimensions return -1 without modifying the model. */
int vt_init_size(struct vt *,unsigned cols,unsigned rows);
/* Preserve the top-left overlap on both screens; discard cropped cells,
 * clamp saved cursors, reset margins and damage all active rows. No reflow. */
int vt_resize(struct vt *,unsigned cols,unsigned rows);
void vt_feed(struct vt *,const char *,unsigned);
void vt_dirty(struct vt *);
struct vt_cell *vt_cells(struct vt *);
#endif
