/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "workspace-wire.h"
static int workspace_request(unsigned op, GR_WINDOW_ID id, GR_WINDOW_ID other,
        unsigned value, unsigned edge, unsigned extent, struct nxr_workspace_reply *r) {
    struct nxr_workspace_req *q;
    LOCK(&nxGlobalLock);
    memset(r, 0, sizeof(*r));
    q = nxAllocReq(NXR_WORKSPACE, sizeof(*q), 0);
    q->version = NXR_WORKSPACE_VERSION; q->op = op; q->id = id; q->other = other;
    q->value = value; q->edge = edge; q->extent = extent;
    if (TypedReadBlock(r, sizeof(*r), NXR_WORKSPACE) < 0) r->status = FDE_FAILED;
    UNLOCK(&nxGlobalLock);
    return r->status;
}
int fde_workspace_get(struct fde_workspace_info *out) {
    struct nxr_workspace_reply r; int status;
    if (!out) return FDE_INVALID;
    status = workspace_request(NXR_WS_INFO,0,0,0,0,0,&r);
    if (!status) *out = r.workspace; return status;
}
int fde_window_get(GR_WINDOW_ID id, struct fde_window_info *out) {
    struct nxr_workspace_reply r; int status;
    if (!out) return FDE_INVALID;
    status = workspace_request(NXR_WS_GET,id,0,0,0,0,&r);
    if (!status) *out = r.window; return status;
}
int fde_window_next(GR_WINDOW_ID id, struct fde_window_info *out) {
    struct nxr_workspace_reply r; int status;
    if (!out) return FDE_INVALID;
    status = workspace_request(NXR_WS_NEXT,id,0,0,0,0,&r);
    if (!status) *out = r.window; return status;
}
int fde_workspace_watch(unsigned enabled) {
    struct nxr_workspace_reply r;return workspace_request(NXR_WS_WATCH,0,0,enabled,0,0,&r);
}
int fde_window_role(GR_WINDOW_ID id,unsigned role,GR_WINDOW_ID parent,unsigned edge,unsigned extent) {
    struct nxr_workspace_reply r;return workspace_request(NXR_WS_ROLE,id,parent,role,edge,extent,&r);
}
int fde_window_action(GR_WINDOW_ID id,unsigned action) {
    struct nxr_workspace_reply r;return workspace_request(NXR_WS_ACTION,id,0,action,0,0,&r);
}
int fde_workspace_manage(void) {
    struct nxr_workspace_reply r;return workspace_request(NXR_WS_MANAGE,0,0,0,0,0,&r);
}
int fde_workspace_frame(GR_WINDOW_ID id,GR_WINDOW_ID frame) {
    struct nxr_workspace_reply r;return workspace_request(NXR_WS_FRAME,id,frame,0,0,0,&r);
}
