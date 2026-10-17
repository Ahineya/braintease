/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_TERMINAL_H
#define RIPPLE_TERMINAL_H
#include <stdint.h>
#define VT_MAX_COLS 80
#define VT_MAX_ROWS 30
#define VT_DEFAULT_COLS 80
#define VT_DEFAULT_ROWS 30
struct vt_cell { uint16_t ch, colour; };
struct vt {
    /* Active rows are packed with a stride of cols, within bounded storage. */
    struct vt_cell cells[2][VT_MAX_ROWS * VT_MAX_COLS];
    unsigned cols,rows;
    unsigned x,y,sx,sy,primary_x,primary_y,top,bottom;
    unsigned fg,bg,bold,reverse,visible,wrap,pending,alternate,appcursor;
    unsigned parser,private_mode,count,params[16];
    unsigned g0,g1,shift,charset_target;
    unsigned char dirty[VT_MAX_ROWS];
    void (*scroll)(struct vt *,int);
    void (*reply)(struct vt *,const char *);
    void *user;
};
void vt_init(struct vt *);
/* Invalid dimensions return -1 without modifying the model. */
int vt_init_size(struct vt *,unsigned cols,unsigned rows);
/* Preserve the top-left overlap on both screens; discard cropped cells,
 * clamp saved cursors, reset margins and damage all active rows. No reflow. */
int vt_resize(struct vt *,unsigned cols,unsigned rows);
void vt_feed(struct vt *,const char *,unsigned);
void vt_dirty(struct vt *);
struct vt_cell *vt_cells(struct vt *);
#endif
