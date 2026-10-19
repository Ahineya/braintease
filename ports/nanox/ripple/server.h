/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NXR_SERVER_H
#define NXR_SERVER_H
#include <sys/time.h>
#include "nano-X.h"
#include "device.h"
struct gr_window;struct gr_pixmap;
void nxr_draw_target(struct gr_window *);
int nxr_buffer_window(struct gr_window *);
void nxr_buffer_damage(PSD,int,int,int,int);
int nxr_internal_pixmap(struct gr_pixmap *);
int nxr_clear_buffer(struct gr_window *,int,int,int,int,int);
void nxr_commit(int);
void nxr_select(GR_TIMEOUT);
int nxr_accept(int);
int nxr_open_socket(void);
void nxr_forget(int);
void nxr_receive(int);
int nxr_write(int, const void *, int);
void nxr_bad(int);
void nxr_time(struct timeval *);
int nxr_dispatch(int, void *);
unsigned nxr_image_count(void);
void nxr_image_forget(int);
void nxr_clipboard_forget(int);
int nxr_clipboard_valid(void *,unsigned);
void nxr_clipboard_dispatch(int,void *);
int nxr_image_valid(void *, unsigned);
void nxr_image_dispatch(int, void *);
void nxr_workspace_sync(void);
void nxr_workspace_forget(int);
int nxr_workspace_valid(void *, unsigned);
void nxr_workspace_dispatch(int, void *);
int nxr_workspace_focusable(struct gr_window *);
int nxr_workspace_key(unsigned, unsigned);
#endif
