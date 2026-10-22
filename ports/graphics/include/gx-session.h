/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPLIX_GX_SESSION_H
#define RIPLIX_GX_SESSION_H
#include <stdint.h>
/* Optional C16 adapter for the desktop session API. Probe GXS_INFO before use;
 * unsupported targets reject it. GX_VERSION and GX_INFO flags are unchanged.
 * IDs are session-scoped, nonzero, never reused. No address is an identity. */
#define GX_SESSION 0x471f
#define GXS_VERSION 1
#define GXS_INFO 0
#define GXS_REGISTER 1
#define GXS_ATTACH_CHILD 2
#define GXS_CLAIM_MANAGER 3
#define GXS_RELEASE_MANAGER 4
#define GXS_NEXT_WINDOW 5
#define GXS_GET_WINDOW 6
#define GXS_NEXT_INSTANCE 7
#define GXS_NEXT_PROCESS 8
#define GXS_CONTROL 9
#define GXS_WATCH 10
#define GXS_CAP_REGISTRY 1
#define GXS_CAP_CONTROL 2
#define GXS_CAP_WATCH 4
#define GXS_CAP_ATTACH_CHILD 8
#define GXS_CHANGED 1
#define GXS_VISIBLE 1
#define GXS_MINIMIZED 2
#define GXS_MAXIMIZED 4
#define GXS_FOCUSED 8
#define GXS_RESIZABLE 16
#define GXS_HIDDEN 32
#define GXS_CLOSE_PENDING 64
#define GXS_ROLE_NORMAL 0
#define GXS_ROLE_PANEL 1
#define GXS_ACTIVATE 1
#define GXS_MINIMIZE 2
#define GXS_MAXIMIZE 3
#define GXS_RESTORE 4
#define GXS_MOVE 5
#define GXS_RESIZE 6
#define GXS_CLOSE 7
/* One record is 338 C16 cells; native ABI adapters must marshal explicitly.
 * REGISTER reads only app_id/name/icon. App ID is ASCII [A-Za-z0-9._-],
 * 1..63 octets. Names/icons/titles are bounded NUL-terminated UTF-8.
 * Role reports normal or the manager-owned GX_PANEL overlay. Workspace
 * currently reports the single-workspace profile; panels are never dock apps.
 * Window bounds are logical client coordinates, independent of pixel density. */
struct gx_session_record {
    uint32_t window,process,instance,workspace;
    uint16_t pid,flags,role,reserved;
    int32_t x,y;
    uint16_t width,height;
    char app_id[64],name[64],icon[128],title[64];
};
struct gx_session {
    uint16_t version,operation;
    uint32_t authority,object,revision,value;
    uint16_t flags,reserved;
    struct gx_session_record *record;
};
/* INFO: zero inputs; returns caps in flags, window count in object, instance
 * count in value and current revision. REGISTER returns caller process and
 * instance IDs in record. ATTACH_CHILD: object=actual direct child PID; only a
 * registered parent may explicitly associate its child with its own instance.
 * Returns child process identity in object and instance identity in value.
 *
 * NEXT_*: object=last ID (0 starts), revision=0 starts a snapshot. Each result
 * returns current revision and next ID in object (0 ends). Retain that revision
 * for subsequent reads; EAGAIN means restart. GET_WINDOW: object=window ID.
 * record is required on registry reads and registration, not INFO/claim/watch.
 *
 * CLAIM_MANAGER: root-only exclusive process-owned authority, returns token.
 * RELEASE_MANAGER requires it. CONTROL: object=live window ID, value=command;
 * owner may control its own window with authority=0, manager token permits
 * cross-owner commands. MOVE/RESIZE read record geometry, other controls do
 * not use record. Close is a request to the application, never a process kill.
 * Controls initially require nested desktop mode (GXS_CAP_CONTROL).
 *
 * WATCH acknowledges the revision the caller processed and subscribes this
 * process's /dev/gfx readiness to coalesced session changes. It returns current
 * revision and flags=GXS_CHANGED when newer. Watching does not grant control.
 * Drain/restart the snapshot then WATCH its accepted revision before select().
 * Authority, membership and watch subscriptions end at exec/exit, not descriptor
 * close. A newly executed image obtains a fresh registry process identity.
 */
#endif
