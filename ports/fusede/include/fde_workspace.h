/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FDE_WORKSPACE_H
#define FDE_WORKSPACE_H
#include "nano-X.h"
/* Private C16 workspace ABI v1. IDs always name application windows, never
 * decoration frames. Enumerate with next(0), next(previous.id), ... . */
#define FDE_EVENT_WORKSPACE 23
#define FDE_CAP_WINDOWS 1
#define FDE_CAP_ACTIVATE 2
#define FDE_CAP_MINIMIZE 4
#define FDE_CAP_MAXIMIZE 8
#define FDE_CAP_PANELS 16
#define FDE_CAP_HOSTED 32
#define FDE_ROLE_NORMAL 0
#define FDE_ROLE_DIALOG 1
#define FDE_ROLE_PANEL 2
#define FDE_EDGE_NONE 0
#define FDE_EDGE_TOP 1
#define FDE_EDGE_BOTTOM 2
#define FDE_EDGE_LEFT 3
#define FDE_EDGE_RIGHT 4
#define FDE_WINDOW_MAPPED 1
#define FDE_WINDOW_ACTIVE 2
#define FDE_WINDOW_MINIMIZED 4
#define FDE_WINDOW_MAXIMIZED 8
#define FDE_WINDOW_DECORATED 16
#define FDE_ACTIVATE 1
#define FDE_MINIMIZE 2
#define FDE_MAXIMIZE 3
#define FDE_RESTORE 4
#define FDE_CLOSE 5
#define FDE_OK 0
#define FDE_INVALID 1
#define FDE_DENIED 2
#define FDE_UNSUPPORTED 3
#define FDE_BUSY 4
#define FDE_FAILED 5
struct fde_workspace_info {
    unsigned long revision;
    unsigned capabilities, count;
    int x, y;
    unsigned width, height;
};
struct fde_window_info {
    GR_WINDOW_ID id, transient;
    unsigned long pid;
    unsigned role, state;
    int x, y;
    unsigned width, height;
    char title[64];
};
/* Return FDE_* status; output is unchanged on failure. next returns id=0 at
 * end. Recheck revision after enumeration when a consistent snapshot matters.
 * watch coalesces changes into one pointer-free event; re-query on receipt. */
int fde_workspace_get(struct fde_workspace_info *);
int fde_workspace_watch(unsigned enabled);
int fde_window_next(GR_WINDOW_ID after, struct fde_window_info *);
int fde_window_get(GR_WINDOW_ID, struct fde_window_info *);
/* Set roles before mapping. Transients must belong to the same connection.
 * RVM panels reserve an edge while mapped and preserve keyboard focus.
 * Hosted panels use ordinary native chrome and reserve no desktop space. */
int fde_window_role(GR_WINDOW_ID, unsigned role, GR_WINDOW_ID transient,
                    unsigned edge, unsigned extent);
int fde_window_action(GR_WINDOW_ID, unsigned action);
/* NanoWM only: one manager connection; bind its frame after reparenting. */
int fde_workspace_manage(void);
int fde_workspace_frame(GR_WINDOW_ID client, GR_WINDOW_ID frame);
#endif
