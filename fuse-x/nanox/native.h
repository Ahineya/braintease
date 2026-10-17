/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FUSE_X_NANOX_NATIVE_H
#define FUSE_X_NANOX_NATIVE_H
#include <gx.h>
#include "serv.h"
PSD GdCreatePixmap(PSD,int,int,MWIMGDATFMT,void *,int);
int nxh_new(GR_WINDOW *);
void nxh_destroy(GR_WINDOW *);
int nxh_resize(GR_WINDOW *,int,int);
int nxh_reparent(GR_WINDOW *,GR_WINDOW *);
void nxh_reparented(GR_WINDOW *);
void nxh_assign(GR_WINDOW *,PSD);
void nxh_damage(PSD,int,int,int,int);
int nxh_present(void);
int nxh_event(struct gx_event *);
int nxh_input(struct gx_event *);
GR_WINDOW *nxh_mouse_window(int,int);
void nxh_finish(void);
void nxh_release_keys(void);
void nxh_activate(GR_WINDOW *);
#endif
