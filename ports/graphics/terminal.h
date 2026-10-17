/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_TERMINAL_H
#define RIPPLE_TERMINAL_H
#include <stdint.h>
#define VT_COLS 80
#define VT_ROWS 30
struct vt_cell { uint16_t ch, colour; };
struct vt {
    struct vt_cell cells[2][VT_ROWS * VT_COLS];
    unsigned x,y,sx,sy,primary_x,primary_y,top,bottom;
    unsigned fg,bg,bold,reverse,visible,wrap,pending,alternate,appcursor;
    unsigned parser,private_mode,count,params[16];
    unsigned char dirty[VT_ROWS];
    void (*scroll)(struct vt *,int);
    void (*reply)(struct vt *,const char *);
    void *user;
};
void vt_init(struct vt *);
void vt_feed(struct vt *,const char *,unsigned);
void vt_dirty(struct vt *);
struct vt_cell *vt_cells(struct vt *);
#endif
