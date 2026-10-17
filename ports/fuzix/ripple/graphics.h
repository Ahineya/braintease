/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_FUZIX_GRAPHICS_H
#define RIPPLE_FUZIX_GRAPHICS_H
int ripple_gfx_open(uint_fast8_t, uint16_t);
int ripple_gfx_close(uint_fast8_t);
int ripple_gfx_ioctl(uint_fast8_t, uarg_t, char *);
void ripple_gfx_poll(void);
void ripple_gfx_exit(ptptr);
void ripple_gfx_wakeup(void);
#endif
