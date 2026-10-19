/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NXR_WORKSPACE_WIRE_H
#define NXR_WORKSPACE_WIRE_H
#include "nxproto.h"
#include "fde_workspace.h"
#define NXR_WORKSPACE 252
#define NXR_WORKSPACE_VERSION 1
#define NXR_WS_INFO 1
#define NXR_WS_NEXT 2
#define NXR_WS_GET 3
#define NXR_WS_ROLE 4
#define NXR_WS_ACTION 5
#define NXR_WS_WATCH 6
#define NXR_WS_MANAGE 7
#define NXR_WS_FRAME 8
struct nxr_workspace_req {
    nxReq header;
    unsigned version, op;
    GR_WINDOW_ID id, other;
    unsigned value, edge, extent;
};
struct nxr_workspace_reply {
    unsigned status;
    struct fde_workspace_info workspace;
    struct fde_window_info window;
};
#endif
