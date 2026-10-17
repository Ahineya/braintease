/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NXR_SERVER_H
#define NXR_SERVER_H
#include <sys/time.h>
#include "nano-X.h"
void nxr_select(GR_TIMEOUT);
int nxr_accept(int);
int nxr_open_socket(void);
void nxr_forget(int);
void nxr_receive(int);
int nxr_write(int, const void *, int);
void nxr_bad(int);
void nxr_time(struct timeval *);
int nxr_dispatch(int, void *);
#endif
