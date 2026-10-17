/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NANOX_RIPPLE_SCREEN_H
#define NANOX_RIPPLE_SCREEN_H
#include <gx.h>
#include "device.h"
int nxr_present(void);
int nxr_descriptor(void);
int nxr_event(struct gx_event *event);
#endif
