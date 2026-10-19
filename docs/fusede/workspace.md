# FuseDE workspace services

FuseDE applications can enumerate windows, observe workspace changes and request
activation, minimization, restoration or close through
[`fde_workspace.h`](../../ports/fusede/include/fde_workspace.h). NanoWM adds Slate
decorations, visible resize grips, minimize/maximize buttons and reserved panel
space on RVM. Fuse-X keeps macOS window management and exposes the shared window
list and supported actions through its native adapter.

## Using the workspace

In an RVM graphical session, drag a titlebar to move a window. Drag a border or
the bottom-right grip to resize it; resizing takes effect on release. The titlebar
buttons minimize, maximize/restore and request close. Alt+Tab cycles through
application windows, including minimized ones; Alt+Shift+Tab cycles backwards.
Closing an editor sends its normal close request, allowing it to save or cancel.

`fdectl` provides a command-line view of the same service on both platforms:

```sh
fdectl list
fdectl info 42
fdectl minimize 42
fdectl activate 42
fdectl maximize 42
fdectl restore 42
fdectl close 42
```

Use the ID shown by `list`. An asterisk marks the active window; `m` means
minimized, `M` maximized and `p` panel. Activating a minimized window brings it
back. Restore also reverses maximization. Unknown or destroyed IDs fail without
affecting another window. Fuse-X minimizes through the guest hide/show mechanism;
maximize remains a native macOS operation and returns unsupported through this
API. Native chrome, desktop placement and screen geometry remain host policy.

## Window identity and notifications

Window IDs identify the application's Nano-X window throughout reparenting and
decoration changes. NanoWM registers the relationship between each client and its
frame; frame IDs are absent from the public list. The server owns the registry
and implements changes to those registered windows. NanoWM owns frame drawing,
pointer interaction and the decision to decorate a client. Frame registration
requires the single manager connection and its own frame containing that client.

Call `fde_workspace_get` for capabilities and a revision, then enumerate with
`fde_window_next(0, &window)` and subsequent calls using the preceding ID. End of
list returns a zero ID. Recheck the revision after enumeration when a consistent
snapshot matters. Each record includes title, process ID, role, state and client
size. RVM positions refer to the frame origin. Native positions are local to
their host window and do not describe macOS desktop placement.

`fde_workspace_watch(1)` subscribes to `FDE_EVENT_WORKSPACE` on the existing
Nano-X event stream. The event contains no pointers or window snapshot. Changes
coalesce while a notification is queued; query the latest state after receiving
it. Changes include creation/destruction, title, focus, geometry, stacking,
minimize/maximize state and panel reservations. Idle clients block on the normal
event mechanism; they need no polling timer.

The registry is bounded by the server's existing 256-window limit. The private
C16 transport uses request 252, workspace version 1, fixed-size scalar requests
and typed replies. Wrong versions, operation codes or frame lengths disconnect
the offending connection. Semantic failures return status values. This is a
local display service for the session's clients, rather than an authorization
boundary between different users: clients may activate or request close of
another listed application. Roles and frame ownership are checked separately.

## Roles and panels

Set a role before mapping a top-level. Normal and modeless dialog roles support
an optional dialog transient parent on the same connection. Parent cycles,
foreign parents and changes to a mapped role are rejected. Modal dialogs and
popup capture/dismissal remain subsequent contracts.

On RVM, panel registration supplies an edge and thickness. Panels receive zero
server border and no NanoWM frame. One panel may occupy each edge; combined
reservations must leave at least 64×64 pixels of workspace. A mapped panel
reserves its edge; unmapping, destroying or disconnecting it releases the space.
Top/bottom panels span the display width and left/right panels span its height;
corners can overlap. Edge stacking/layout coordination belongs to a future shell.

Maximized windows follow reservation changes. Ordinary frames retain a reachable
caption when the work area changes. Panels stay above ordinary windows without
repeatedly raising or repainting at idle. Pointer focus and ordinary `GrSetFocus`
do not steal keyboard focus for an RVM panel; explicit `FDE_ACTIVATE` supports
deliberate keyboard access. Panels are excluded from Alt+Tab.

Fuse-X reports `FDE_CAP_HOSTED`, preserves ordinary native chrome/focus for
panels, reserves no guest work area and reports zero work-area dimensions.
Borderless native windows, transparency and input pass-through remain separate
capabilities. An application must inspect the capabilities before promising a
borderless dock or programmatic maximize.

## Manager recovery and acceptance

The server detaches application windows before destroying a disconnected
manager's frames. A replacement NanoWM adopts surviving mapped top-levels using
their original client IDs. Minimized windows remain discoverable and can be
activated. The normal `startx` supervisor still ends its session if NanoWM exits;
automatic manager restart is a separate session-policy change.

The integration suite runs `fdectl --check` on disposable RVM and Fuse-X disks.
It checks identity across reparenting, supported actions, panel geometry/focus,
work-area recovery, transient cycles, foreign role/frame requests, coalesced
notifications, close delivery and stale IDs. The RVM harness also kills and
restarts NanoWM while an application survives. Existing colour-resource checks
continue to compare guest pixels with an independent oracle and check idle
painting, client cleanup and filesystem consistency.

```sh
python3 ports/fusede/tools/test_showcase.py
```

RVM uses JIT. Native checks run without opening host windows. General widgets,
the Nuklear file-list evaluation, proportional fonts, the dock, top bar and
file manager remain the next milestones. RVM guest resolution remains 640×480;
larger scanout and backing-surface budgets require their own measured change.
