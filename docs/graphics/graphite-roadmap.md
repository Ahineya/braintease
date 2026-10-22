# Graphite applications and desktop roadmap

Agreed direction, 21 October 2026. Build a coherent application framework on
LVGL and GX, using useful applications to exercise each shared component.
Fuse-X is the first target; Riplix x64 follows when its graphics and application
services are ready. RVM remains supported by its existing graphics stack, but
does not set the performance or visual limits for this work.

Shared actions, split panes, forms and the table-and-plot workbench are
implemented, alongside the first document/editor framework and guest Graphite
desktop shell. Application and desktop work have independent acceptance;
one owner coordinates changes at the shared GX/runtime boundary.
The larger destination is a graphical Syma worksheet and a small desktop built
from the same application components.
Graphite is also the successor to the earlier Nano-X/Slate FuseDE experiment.
That experiment remains available for existing RVM users and compatibility
checks; new desktop and application work goes into this shared framework.
The first [session registry and window-control profile](../fuse-x/desktop.md#session-registry-and-window-controls)
now implements running application identities, explicit helper association,
revisioned snapshots, coalesced watches and process-bound window authority.
The [Graphite shell and launch catalog](../fuse-x/desktop.md#graphite-shell-and-application-catalog)
now use this foundation for searchable launchers, pins and window menus.
The [FuseDE desktop contract](#fusede-desktop-contract-proposed) below defines
the proposed shell behavior beyond the implemented nested-desktop foundation.
The session-service foundation follows the completed separate-process SDL mixer
work; the proposed shell extensions retain their separate x64 integration gates.

The first [grouped application dock](../fuse-x/desktop.md#grouped-application-dock)
is also implemented in the nested desktop adapter: one running icon per explicit
application ID, live window menus, cooperative group actions and stable ordering.
Graphite apps and the terminal register identities. Windowless helpers never
create task entries. The guest shell now owns the dock, pinned launchers and
regional top panels; the compositor retains a fallback dock when it is absent.
Workspaces, tiling, fullscreen spaces and Activity Monitor remain planned.

## Existing foundation

The [LVGL port](../../ports/lvgl/README.md) supplies native GX windows, full-color
rendering with RGB565 fallback, live 1x/2x density, compact Noto Sans typography,
Graphite semantic styles and Lucide icons. Calculator, file selector, interface
gallery, Data Viewer and Text Editor run in both presentation modes. The normal
`--desktop` launcher builds/installs those five apps, the shell and its catalog;
standalone host-share staging remains available.

Shared components include text inputs, buttons, checkboxes, radio lists, lists,
popup menus, modal dialogs and a bounded virtual table. Keyboard focus, modal
input isolation, Unicode clipboard access, column resizing and two-axis scrolling
are part of the component contracts. CSV source files remain read-only. Loading,
parsing and sorting now use cancellable incremental jobs with transactional
commit; each returns to input/rendering between bounded batches.

## Milestone 1 Shared actions and split panes

* Give actions one identity, handler, enabled state and shortcut definition.
  Toolbar buttons and menus use that definition; disabled actions cannot execute
  through another input path. Modal dialogs retain shortcut isolation.
* Add a reusable two-pane layout with pointer dragging, keyboard adjustment,
  minimum pane sizes, collapse/restore and logical coordinates across density
  changes. Keep pane contents alive while resizing.
* Exercise both in a Workspace gallery page, including disabled states and
  focus behavior. Use the same actions in Data Viewer and add an optional
  resizable details pane, preserving its preference under HOME.

Acceptance: actual Fuse-X input tests, host/guest rendering comparison at 1x/2x,
resize and density transitions, modal isolation, deletion/recreation and idle
publication. Stage tested executables only into `fuse-x-shared`.

## Milestone 2 Data and plot workbench

Extend Data Viewer with a shared plot component and table/plot split. Begin with
line and scatter series, numeric column selection, axis labels, fit-to-data,
pan/zoom and SVG export. Keep the series model and rendering independent of
CSV and Syma. Bound rendering work by the viewport while preserving peaks,
missing-data gaps and source row identities.

Add dropdowns and reusable Open/Save dialogs when the workbench needs them.
Progress and cancellation require incremental jobs or a worker boundary;
displaying a progress indicator around a blocking operation is insufficient.
Preserve source files and report invalid numeric values explicitly. Numerical
derivatives of sampled data are separate from symbolic differentiation.

The implemented first plot profile uses a document-independent immutable model,
viewport-bounded line/scatter geometry, axis labels and source-row selection.
Conservative gap handling avoids drawing across missing values; original
extrema survive viewport sampling. Drag/arrow panning, +/- zoom and Home/fit
are supported; wheel zoom is not part of the current GX input route. SVG
export uses temporary-file replacement and is cancellable. Raster/PNG export,
multiple series and numerical transformations remain later additions.

Acceptance: a CSV can be opened, inspected, plotted and exported; large inputs
remain interactive; plot controls work with mouse and keyboard at both densities.
The gallery exercises the same plot component and its empty/error states.

## Renderer foundation shared with future targets

Keep rendering acceleration below LVGL and SDL, with neither toolkit depending
on the other. The first retained GX profile supplies renderer-owned colour/A8
targets, opaque window-owned handles and validated command batches. The CPU
pixel primitives use conventional byte coverage and colour words, independent
of C16 pointers or host windows. Fuse-X is the first adapter; Riplix x64 will
need its own ABI and presentation adapter when its kernel graphics services are
ready. Ongoing x64 kernel work is outside this change.

LVGL initially uses a bounded native colour strip and retained plot masks.
Unsupported draws restore the current strip for software completion, preserving
existing pixels and allowing the next strip to use the native path again.
Older kernels retain the portable adapter. Acceptance includes byte-exact
native/software scenes, mixed RGB-image fallback, pointer/span validation,
transactional batches, ownership, resize/teardown and measured continuous drags.
See the [retained rendering contract](retained-rendering.md).

## Milestone 3 Document framework and graphical editing

Add tabs, common toolbar/status layouts, document dirty state, Save/Save As,
discard confirmation, recent documents and safe replacement saves. Establish
context-menu, tooltip, tree-view and progress components through real uses.

Adapt Rift's text model, undo, grapheme movement, selection and highlighting to
a graphical viewport. Keep editing and syntax logic shared with the terminal
editor; use LVGL for surrounding controls. Test long lines, large paste, mixed
Unicode, selection and scrolling before embedding the editor into a worksheet.

The first profile supplies the toolkit-independent `ui-document` session,
shared `ui-tabs` and a `ui-editor` viewport over Rift's existing C TextBuffer.
`lvedit` exercises New/Open/Save/Save As, per-document selection and undo,
unsaved close/quit confirmation, external-file conflicts and HOME recents.
The shared document tabs use compact integrated surfaces, left-side close
controls revealed on hover or keyboard focus, stable title geometry and visible
dirty indicators. The editor groups the strip directly with its viewport;
menus provide actions without a duplicate quick-action row or a full-path row.
Each document can select a syntax language before it has a filename. Automatic
keeps extension detection; explicit choices remain with the open document
through saving and reload without changing its contents or undo history.
The gallery's Editor page exercises the same viewport and tabs. Graphical
wrapping measures glyph advances in pixels rather than using terminal columns.
The existing 512 KiB document and 32 KiB clipboard bounds remain explicit;
font coverage and shaping retain the current Graphite capabilities.

Desktop foundation work proceeds in parallel: retain independent app surfaces,
compose them into one Fuse-X host window, and exercise focus, capture,
move/resize, minimize/restore, close and process cleanup. A terminal and
calculator suffice to test the boundary while editor development continues.
Acceptance uses the same input policy and compositor as the visible presenter,
including 1x/2x density, source-pixel checks and idle/damage measurements.

## Milestone 4 Graphical Syma worksheet

Reuse the [worksheet/session model](../../ports/syma/WORKSHEET.md), editor,
table and plot components. Results become typed presentation objects: expression,
text, table, plot or error. Keep exact symbolic terms available behind readable
presentation. Preserve explicit cell execution and current session semantics.

Evaluation must leave the interface responsive and support cancellation with
defined session-state behavior. First acceptance document: the existing OOP and
small mathematics libraries side by side. Then add useful data libraries for
CSV loading, column operations and plotting without requiring users to manage
the low-level effect protocol.

## Milestone 5 Desktop services and shell

Build a launcher, settings, file associations and task/window navigation around
the applications. Define application identity, launch arguments, lifecycle,
single-instance/document activation policy and desktop-wide window authority.
LVGL widgets do not themselves provide a system window manager.

Build this incrementally on the single-window prototype, alongside applications.
The immediate shared services are cohesive guest-namespace Open/Save dialogs
and window controls on the left: close, minimize, maximize/restore. Dialogs own
browsing, keyboard navigation, hidden-file visibility, validation and replacement
intent; applications own document loading and safe writes. They must not launch
private file-picker processes or change the caller's working directory.
Directory scans yield between bounded batches and cancel on navigation/close.

Application identities, the guest Graphite shell, launcher/catalog, pinned dock
and regional panel profile are implemented. Settings, file associations and
the larger workspace/fullscreen design remain later work. Keep window policy,
rendering and shell widgets separate;
the first two can be reused by the x64 display adapter without importing macOS
window code. Retire old launch paths only when their replacement has equivalent
session and application lifecycle coverage.

Fuse-X retains native host windows and adds an optional nested desktop session.
The foundation's portable window policy and packed compositor remain separate
from its macOS presenter. A separate guest Graphite shell now supplies the first
launch and navigation profile. The public session/control API is implemented; its registry
and policy do not depend on Graphite widgets. Keep desktop
policy above the GX window/surface/event boundary so Riplix x64 can supply its own backend. X11
compatibility is a separate project for existing applications, not a prerequisite
for the worksheet or the LVGL application framework.

## FuseDE desktop contract (proposed)

FuseDE names the desktop session and shell. Graphite supplies its shared
application controls, typography and visual language; GX supplies windows,
surfaces and input. This section describes the full design target; the first
session/control, regional panels, catalog and pinned-shell profile are implemented,
while workspace, tiling, fullscreen and Activity Monitor extensions remain
planned. The older Nano-X workspace service remains a separate interface.
The accepted prototype is described in [Fuse-X desktop integration](../fuse-x/desktop.md)
and implemented by the [portable desktop policy](../../libraries/graphics/desktop.rs).

The first complete shell targets one display in Fuse-X's optional nested mode.
Native macOS windows remain available. Both modes must run the same application
binaries; applications do not require shell-specific drawing code. The nested
mode develops the desktop behavior that a future x64 backend can reuse.
Multiple physical monitors, transparency, animated workspace transitions and
GPU effects remain separate work after reliable opaque software composition.

### Ownership and identity

| Object | Identity and ownership |
| --- | --- |
| Installed application | Stable package/application ID, display name, icon and executable/argument metadata. Display names and executable basenames are labels, not keys. |
| Application instance | Session-local instance ID with registered process identities. One application may have several instances, windows and helper processes. A PID or process group alone does not define an application. |
| Process | Kernel process identity and lifetime. The launcher records the actual child; window-owning processes and helpers associate with an instance explicitly. Parentage alone cannot establish trusted membership. |
| Window | Existing generation-checked GX handle and actual owner, plus proposed role, instance and transient-parent metadata. A document tab is application state unless the app creates a separate top-level window. |
| Workspace | Stable session ID, name, order, layout state and last focused window. Workspace membership and stacking belong to desktop policy, not applications. |
| Output | Logical bounds, density and available work area supplied by the backend. Host-window points, physical pixels and guest memory cells remain distinct units. |

The launcher starts explicit executable/argument vectors in the guest namespace,
reports failures, and applies a declared new-instance or existing-instance
activation policy. Passing documents to an existing process needs an explicit
activation protocol; changing a title or guessing a PID is insufficient.
Unregistered shell-launched GX clients remain usable as ungrouped windows with
an honest fallback name. Missing metadata must not prevent close or recovery.

One desktop manager owns placement, stacking, focus, capture, layout, work areas
and workspace transitions. A versioned session service must expose snapshots,
coalesced change notifications and supported actions to the shell. The backend
checks the manager's authority and actual window ownership; ordinary clients
may not declare themselves privileged panels or manipulate another process's
surfaces. Window commands identify a handle and generation, never a list index.
Capabilities distinguish enumeration, activation, layout and close requests
from privileged manager operations. The first process-bound authority and
revisioned snapshot/watch contract is implemented in
[GX_SESSION](../fuse-x/desktop.md#session-registry-and-window-controls).
Installed launch metadata and the manager-owned regional `GX_PANEL` profile are
implemented. Workspace and transient-window actions remain later extensions;
GX rendering handles alone do not grant cross-owner control.

Keep policy, pixel composition, platform adapters and shell widgets separate.
Graphite renders the dock, launcher, menus and settings; it does not replace
the compositor with one LVGL tree containing every application's controls.
The shell observes independent clients and sends validated commands. Restarting
shell widgets should preserve client processes, surfaces and usable placement;
safe fallback controls must remain available if the shell fails. Session-wide
shutdown is an explicit action, separate from closing the last window.

### Windows, focus and work areas

Proposed roles are ordinary window, transient dialog, popup, panel and desktop
background. Borderless appearance is a decoration choice, not permission to
reserve screen space, bypass focus rules or stay above all applications. Panel
reservations require manager approval; dialogs name a live permitted parent.

The default is overlapping windows with click-to-focus and raise. Keyboard
window switching uses visible ordinary windows on the active workspace;
application switching may activate an instance on another workspace. Neither
pointer hover, background painting nor a new background window steals keyboard
focus. A background launch can request attention. An explicit user launch or
dock activation may switch workspace and restore the target window.

Only mapped, approved panels reserve work area. The first shell has separate
top-left Riplix and top-right clock pills, plus a bottom application dock. The
clock format is `Wed 21 Oct 22:43`; the middle stays free for client pixels and
input. These regional panels are excluded
from normal window switching and tiled layout. Reservations leave a validated
nonempty usable area, are recomputed on output/density changes, and disappear
when their owner disconnects. Normal and maximized windows retain a reachable
recovery action when the area becomes smaller. An optional vertical dock can
later use the same edge/reservation model; its widget orientation does not
require a different window-management API.

A panel's work-area reservation, painted surface and input region are separate.
A floating dock can reserve a bottom edge for maximized/tiled placement while
painting and accepting pointer events only inside its own bounds. Floating
clients may extend into the reserved band; their pixels and input remain
available beside the dock. Do not implement a reservation by painting an opaque
full-width strip over those clients. Menu modality is a separate, temporary
input policy. The same distinction applies to a later vertical dock.

Minimize hides a window and its transient group without stopping its process
or discarding its document state. It releases capture and transfers focus to
the most recent eligible window. Restore recovers the prior workspace and
placement mode, clamping only when current bounds or constraints require it.
Close remains a request: unsaved-work dialogs can cancel it. Force termination
belongs in an explicit recovery action, never the normal close button.
Workspace switching, destruction and modal transitions cancel stale drags and
clear held input through the existing focus/capture contract. An app-modal
dialog blocks its instance's affected windows, not the whole desktop.

### Global shortcuts and desktop actions (planned)

Direction to evaluate, 21 October 2026: provide a desktop shortcut mechanism
without choosing exact keybindings yet. The HP laptop's keyboard ergonomics
will inform those defaults. Run-dialog and screenshot shortcuts are desired
examples for the future shortcut service; the Applications Run dialog is
implemented, while screenshot tools and exact key chords remain undecided.

Keep logical **Ctrl**, **Shift**, **Alt** and **Super** distinct. Super names the
Windows/Meta modifier; mapping macOS Command/Option and left/right physical
keys belongs in an explicit backend/profile policy. Do not globally collapse
Command into Ctrl. Preserve the distinction between physical HID keys,
layout-dependent symbols and committed text/IME events, so a future binding can
state which interpretation it needs. Native macOS may retain host-reserved
combinations that the nested guest cannot intercept; expose those limits rather
than promising identical interception on hosted and bare-metal backends.

Bindings should resolve to stable named actions such as `desktop.run`,
`screenshot.region`, `screenshot.window` and `screenshot.screen`, using the same
action registry as future menus and settings. A desktop binding has an explicit
scope and enabled state. Define precedence among host shortcuts, DE actions,
modal UI and the focused application; ordinary terminal Ctrl-C and application
shortcuts remain available unless a user explicitly reserves a conflicting
chord. Capture must not strand a drag, held key or modifier. A consumed shortcut
owns its press, repeats, release and associated text, with focus loss and
cancellation clearing that state. Existing hosted menu shortcuts are not yet
this general service.

Evaluate INI-based system defaults with per-user overrides, following the
existing configuration libraries. Entries can reference action IDs and a chord,
with an explicit disable/override operation; arbitrary shell text should not be
required to invoke built-in actions. Validate unknown actions/modifiers,
unsupported keys, duplicate chords and scope conflicts before activating a
configuration. Reload atomically, retain the previous valid configuration on
failure, and provide a recovery path when a user changes navigation bindings.
A later DE settings GUI should edit that same schema rather than introduce a
second shortcut implementation. File locations and exact defaults are still
open decisions.

Screenshot support should separate permission-checked compositor capture from
the selection overlay, cancellation, preview and save/clipboard UI. A future
macOS-style interaction can then select a region, window or whole output without
making the rendering backend own the tool's workflow. Capability discovery must
handle an unavailable capture or clipboard backend. Screenshots and the Run
window remain future features; this milestone only records their action/input
requirements.

### Workspaces, tiling and fullscreen

Ordinary workspaces can be created, named, reordered and removed. Each holds
its own window membership, floating geometry, tiled tree and focus history.
Moving a window also moves its transient children. A command explicitly chooses
whether to follow it; removal moves surviving windows to a chosen ordinary
workspace without closing applications. Switching hides other workspaces from
composition and input, while their programs continue to execute. It does not
promise application suspension or zero background rendering.

Each ordinary workspace chooses floating or tiled layout. Start with an
adjustable binary split tree, horizontal/vertical splits, keyboard move/resize
and configurable modest gaps. Eligible resizable ordinary windows participate;
panels, backgrounds, popups, transient dialogs, fixed-size clients and explicitly
floating windows do not. Floating exceptions remain over their owning layout;
transients follow their parent. Preserve each window's floating bounds when
switching modes. Minimize temporarily removes a tile while retaining its
placement hint; restore reinserts it without leaving a hole. If minimum sizes
cannot fit, retain valid reachable windows and expose a floating fallback
rather than issuing impossible resizes. Dragging a tiled window offers an
explicit rearrange or detach operation, not an accidental conversion.

Maximize and fullscreen are separate commands and states:

| Command | Proposed result | Restore behavior |
| --- | --- | --- |
| Maximize | Fill the current work area with normal window controls and panels still available. Other windows remain on that workspace. | Restore floating bounds or the prior tiled placement. |
| App fullscreen | Place one selected top-level and its transients in a dedicated temporary space, hide normal decorations/panel reservations there, and use the full logical output. | Return to the prior ordinary workspace and placement mode. |

Maximizing a tiled client preserves its workspace's underlying split layout;
restoring reveals that arrangement rather than recomputing a new one.

Fullscreen is inspired by application spaces: the temporary space appears in
the workspace switcher, and moving between it and ordinary workspaces keeps the
app running. Other independent windows of that app retain their workspace;
fullscreen does not silently gather them all. The dock/status strip can be
revealed deliberately without shrinking fullscreen content. A desktop-owned
keyboard command and revealed menu always provide an exit path even if client
input is captured. Fullscreen does not imply changing the physical display
mode or giving an application exclusive hardware access.

Entering saves normal bounds, tile placement and prior maximize state. Leaving
restores that state; if its workspace or tile no longer exists, use an ordinary
workspace and valid floating bounds. Minimizing fullscreen returns the user to
an ordinary workspace and retains fullscreen intent for explicit dock restore.
Destroying the primary window removes the temporary space and chooses a live
focus target. Fixed-size applications require an advertised letterboxed/scaled
presentation capability or have fullscreen disabled; do not pretend they can
accept arbitrary content sizes. In hosted native-window mode, guest fullscreen
and macOS fullscreen remain distinct backend capabilities. This first design
targets nested mode and does not remap native macOS Spaces behind the user.

### Dock and task navigation

The first guest shell profile is implemented: one icon per app, pinned defaults
and user overrides, an Applications button at the far left of the dock, compact
category navigation and word search. Single-window clicks activate; multi-window
clicks open a chooser. Context menus list windows and offer Open new, pin/unpin,
show/minimize all and cooperative close. The Riplix pill also opens Applications,
About and Exit desktop shell. The Applications menu is horizontally centered
above the dock and includes a fixed Run action for names/paths and quoted
arguments, with optional terminal launch and visible exec errors. No screenshot
tool or exact global shortcut bindings are added by this profile.

The curated INI [application catalog](../../ports/fusede/README.md#graphite-application-catalog)
uses stable IDs, names, icons, categories, GUI/terminal kinds and exact absolute
executable/argument vectors. Availability is checked against the installed path
then one explicit host-share fallback. Missing apps are marked unavailable;
they do not resolve through shell-command guesses. TUI entries launch through
gterm with app-specific metadata, so Commander, Rift, Music, Telegram, Chat,
Multiplexer and JavaScript have their own grouped icon without shell/helper
entries. Legacy Nano-X apps remain earlier porting experiments; their catalog
integration is not a current goal. Reconsider the browser choice separately.

Catalogs install under `/usr/share/applications`; `FUSEDE_APPLICATIONS` selects
another directory, including staged `/mnt/host/fusede/applications`. Pins merge
catalog defaults with `$HOME/.config/fusede/pins.ini`, with atomic replacement
and no overwrite during system catalog installation. Normal
`scripts/run-fuse-x.sh --desktop` builds/installs the five Graphite apps plus
`fusedeshell` and starts it through `/init`. `--run-only` needs the installed
shell or staged `/mnt/host/fusedeshell.fxe`; native-window and rescue sessions
keep graphical terminals. Shell exit/revocation restores fallback dock controls
without closing client apps. The standalone LVGL builder's `--app all` remains
the five ordinary apps, with `--app fusedeshell` separate.

The remaining design target extends these controls across workspaces and an
explicit existing-instance/document activation protocol:

The dock combines pinned launchers and running application instances, grouped
by stable application ID. Running and active indicators have stable geometry;
minimized windows remain discoverable without changing label widths. Clicking
an idle launcher starts the app. Clicking a running entry activates its most
recent eligible window, restoring and switching workspace if necessary. A
window menu lists every instance/window, workspace and minimized state, with
New Window when supported. Repeated activation does not minimize all windows.
The dock's own transient menus are excluded from the task list and tiled tree.

The dock does not infer liveness from titles or kill an application when its
last window closes. Registered background instances can remain visible by
declared policy. Helpers such as the mixer belong to their instance but do not
become launcher buttons. A terminal is its own application; the foreground
shell job can inform its title without making every command a desktop app.
Native Fuse-X retains its single macOS Dock identity; this guest dock is shell
content inside the nested desktop, not a collection of new host applications.

### Application artwork and package ownership (planned)

The current Lucide symbols remain useful for actions and fallback icons. The
dock and catalog should later support distinct application artwork supplied by
[pkg](../../ports/pkg/README.md). Keep the stable application ID separate from
its artwork: updating an icon must not change window grouping or user pins.

Define package-owned icon assets and density variants, a bounded image-decoding
path and a generic missing-icon fallback before selecting the visual style.
Installation/removal should update catalog metadata and assets together, with
provenance/licenses preserved; arbitrary asset paths must not become executable
commands. Decide supported raster formats, asset locations and versioned
metadata with the package design. No new icon file format, asset loader or
package schema is implemented by the current shell changes.

### Activity Monitor and memory inspection

Build the first graphical monitor as a normal Graphite application using the
shared virtual table, plots, actions and incremental jobs. Begin read-only:
sortable process list, filter, CPU history and memory summary. Existing
[`/dev/proc` snapshots](../../ports/fuzix/ripple/resources.h) already supply
versioned address-free system and process records used by `ps`, `top` and
`free`. Consume those through a small data-provider boundary, not raw `p_tab`
layouts, command-output scraping or direct runtime internals.

CPU percentages come from successive user/system counter deltas divided by
their matching sample interval and clock rate. The first observation and
missing counters display unavailable, not a fabricated zero. PID/start-time
changes reset history. Snapshots are coherent per record, not an atomic freeze
of the whole process table; tolerate exit/reuse between samples. Join optional
application/window metadata separately, retaining process detail for helpers
and unregistered programs. Launch metadata supplies full app names because
current process names contain only eight characters.

Show physical KiB, mapped/private/shared process memory and total pool use with
their actual meanings. Shared mappings must not be summed as exclusive RAM;
system pool totals count shared pages once and may include pinned allocations.
Fuse-X's present process accounting is provisional and does not represent all
macOS resident memory, surface storage or renderer caches. Expose limitations;
do not label unsupported categories as exact. Surface/cache statistics, richer
native accounting, I/O rates and audio underruns need separate advertised
providers before the monitor can show them.

Sample on a bounded timer while visible, pause or reduce sampling when hidden,
and retain a bounded history. Sorting and plotting must yield to input. Add
terminate/force-terminate only with explicit user actions, permission checks
and process-identity validation at the control boundary; stale selection must
never signal a recycled PID. Unsupported safe control stays disabled. The
initial read-only monitor needs no new kernel interface or x64 change.

### Delivery and x64 boundary

The first opt-in native x64 implementation is now under
[`tools/x86_64/graphics`](../../tools/x86_64/graphics/README.md#opt-in-native-desktop-profile).
It links this shared window policy into the existing freestanding native
runtime and supplies the GX session/panel adapter over native retained frames
and GOP presentation. It leaves process mappings, scheduling, LP64 and ring 3
work independent. Native applications include the shell, calculator, file
selector, gallery, Data Viewer, editor and terminal. The test root is generated
from public source/apps, never from the owner's personal images. Physical
acceptance and the selected HP candidate belong in the hardware handoff;
compilation or a disposable VM alone does not certify them.

The [2026-10-22 HP trial](../../tools/x86_64/hardware/receipts/2026-10-22/graphite-native-trial.json)
boots the desktop at 1920×1200, but hover/click instability, roughly 15-second
Calculator startup and lag remain. A repeated panel-configuration revision
loop is fixed and host-tested in source, **not deployed** in the running trial.
First validate the new idle-watch check in a VM and a fresh hardware candidate,
then profile cold startup and full-resolution composition separately. The
retained renderer/A8 adapter and hardware scroll/gesture input follow measured
needs. The software fallback does not inherit Fuse-X's performance claims.

Keep this sequence independent of the worksheet and application milestones:

1. Define the versioned session registry, manager authority and launch metadata;
   exercise snapshots, stale handles and coalesced events with a small probe.
   Keep fallback desktop controls and the native-window mode working.
2. The first Graphite shell, catalog, pinned grouped dock and separate top pills
   are implemented through the manager-owned regional panel boundary. Continue
   with settings, richer window roles and read-only Activity Monitor, preserving
   independent client surfaces and fallback controls.
3. Add ordinary workspaces and per-workspace focus, then opt-in tiling with
   floating exceptions. Test lifecycle and minimum-size failure paths before
   elaborate arrangement UI.
4. Add dedicated fullscreen spaces and the distinct maximize/restore behavior,
   including dock activation, modal dialogs and return after app failure.
5. Add opt-in session persistence and recovery. Save launch descriptors,
   workspace/layout preferences and app-provided restore tokens atomically;
   never persist PIDs, live handles or an assumption that unsaved buffers were
   saved. Restore known applications once through the session service, with a
   safe startup mode and visible failures. Do not replay arbitrary foreground
   commands inferred from process titles.

Acceptance at each stage covers keyboard and pointer paths, focus/capture
recovery, minimized and crashed clients, density and output resize, exact
unmodified client pixels, bounded repaint damage and idle publication. Add
mixed LVGL, SDL and terminal workloads, measuring input-to-present latency and
audio continuity while moving/resizing windows. Window/process capacity is a
reported resource limit with graceful failure, not a desktop API constant;
the current prototype's sixteen-window storage is not a permanent promise or
an instruction to enlarge shared allocations without measurement.

Develop these features on disposable Fuse-X images. No automatic shell/app
startup is added to x64 test builds. The portable policy, shell components and
data-provider interfaces may be reused there, but hardware input, scanout,
native ABI buffers, scheduling, process control and backend authority require
their own reviewed adapters and actual x64 acceptance. Coordinate those seams
with the [x64 master plan](../x64-master-plan.md); do not implement a second
scheduler or change shared process/memory layouts as desktop work. A successful
Fuse-X test or bare-metal library compile is not certification of HP graphics.

## Packaging and ongoing quality

* Introduce selected applications through [pkg](../../ports/pkg/README.md), with
  application metadata, icons, assets and licensing. Consider a versioned shared
  LVGL library once the API and several applications justify it.
* Keep styles, spacing, typography and interaction rules in the shared layer.
  A gallery/theme inspector can later expose supported theme settings live.
* Add every new component to the gallery, covering focus, keyboard, disabled,
  validation, empty and error states. Test fullscreen, Retina and bounded work
  with large data; measure input-to-published-frame latency before optimizing.
* Track C16's static global capacity separately from heap usage. Keep document
  buffers, recent-file storage and large caches dynamically allocated, and
  account for font/icon assets when growing the application framework.
  The current executable layout reserves sixteen 64,000-cell banks for static
  data (about 2 MiB of physical storage). A follow-up should replace that fixed
  reservation with loader-allocated data regions and explicit size/overlap
  validation. This requires coordinated compiler, loader and runtime work:
  legacy heaps start immediately after the reservation, and the x64 adapter
  must agree on a compatible format. It is independent of this graphics work;
  do not enlarge a single constant or change another agent's kernel layout.
* Extend font coverage and fallback when real documents require it. Runtime font
  loading and complex shaping are later capabilities, with explicit memory and
  rendering budgets.
* Never build test images from personal disks. Use disposable offline fixtures;
  staging host-share executables must not modify running guest images.

## SDL coordination

The ongoing [SDL 1.2 port](../../ports/sdl/README.md) is a complementary
application compatibility layer. Its software surfaces, events and GX backend
already have a bounded Fuse-X profile. PCM audio and a WAV-effects mixer are
implemented; the optional independent mixer process keeps uploaded effects
playing while the application is busy without creating general SDL threads.
General SDL threads, OpenGL and several other SDL services remain outside the
current profile. Desktop responsiveness must not assume those services exist.

LVGL and SDL remain independent clients of GX. Share format negotiation,
window/input semantics, density policy where applicable, damaged-rectangle
publication, clipboard services and future x64 ABI decisions at that boundary.
Do not route LVGL through SDL or introduce a dependency in either direction.
SDL applications retain their own interfaces; Graphite governs our native
application framework. Coordinate changes to common GX/runtime files with the
ongoing SDL work rather than changing those interfaces as part of a widget task.

## Progress

* Milestone 1: implemented. Shared actions and horizontal/vertical split panes
  are exercised by the gallery's Workspace page and Data Viewer's optional
  details pane. Guest interaction tests cover enabled-state consistency,
  modal isolation, dragging, keyboard adjustment, density/resize, idle behavior
  and settings across restart; host component checks also run with UBSan.
* Milestone 2: implemented and accepted for the first single-series profile.
  Shared choices, validated Open/Save paths, progress/cancel, line/scatter plots
  and a resizable table/plot Data Viewer are present. The gallery adds Plots
  (F7) and Forms (F8), including empty/error and replacement-confirmation
  states. Host checks cover model/geometry bounds, source identities,
  transactional/cancellable SVG, widget ownership, restored focus and density.
  Guest checks cover source-linked selection after sorting, numeric gaps,
  progress/cancel, path validation/replacement intent, persistent settings,
  real pointer/keyboard input, large datasets and clean page/window teardown.
  Forms and plot controls/axes compare exactly at 1x/2x; the synthetic sine
  curve permits only a tightly bounded one-pixel antialias/libm difference.
  The shared plot now batches curve coverage, caches axis text and can use
  Fuse-X's optional native A8 raster operation. The first retained GX target
  profile is implemented, preserving exact software fallback and ownership.
  In paired 1470x900/2x drags, 40 samples improved from p95 33 to 22 ms;
  100,000 dense samples improved from 18.5 to 30.4 processing frames/s,
  p95 60 to 36 ms. The 1920x1080/2x maximum remains CPU limited (dense p95
  69 ms), so the 33 ms interactive budget remains a performance objective.
  Clipped endpoints, portable fallback, bank-crossing masks and cache lifetimes
  have dedicated regression checks. See the LVGL port's measured comparison.
  SVG is the export format in this profile. PNG/raster export, multiple series
  and numerical data transformations remain future work.
* Milestone 3: first editor/document profile implemented; guest integration and
  visual acceptance are recorded with the LVGL port. Tree views, tooltips and
  richer editor features remain later additions.
  The guest accepts a 14,602-octet Unicode paste in one undo group (0.205 s in
  the recorded run), round-trips the complete 32 KiB clipboard, and passes
  exact host/guest editor and gallery pixels at 1x/2x. Atomic saves, Unicode
  paths, independent tabs, HOME preferences, failed-open preservation and
  unsaved-close cancellation are covered. Focus loss during a selection drag
  restores the caret and scrolling. Lossless font packing makes room for the
  larger application without changing the executable data layout; replacing
  the fixed static reservation remains the separate compiler/loader follow-up.
* Milestone 5 foundation and first shell profile: optional single-window Fuse-X
  session, portable window/composition foundation, session registry and separate
  guest Graphite shell are implemented. The shell supplies the application
  catalog/search menu, pinned grouped dock and disconnected Riplix/clock panels.
  Manager authority and regional `GX_PANEL` reservations keep shell ownership,
  client surfaces, gaps and safe fallback controls separate.
  The current x64 kernel, hardware drivers and production build remain outside
  this change. A bare-metal library compile does not certify x64 graphics.
  Private guest tests exercise independent terminal/calculator processes,
  retained movement, capture, resize, density, pixel parity and teardown. A
  separate visible macOS smoke verifies one native outer window, clipboard,
  Window-menu restore and creating a terminal after closing all clients.
  The next shared slice adds in-process Open/Save browsers across Text Editor,
  Data Viewer and plot export, replacing application-specific picker processes.
  Scanning and sorting yield between bounded batches, preserve caller cwd and
  retain the committed listing on cancellation or failure. Host and C16 tests
  exercise the same scanner fixtures, including Unicode, physical symlinks,
  directory limits and budget-one progress (11,899 checks). The shared dialogs
  cover keyboard/mouse navigation, hidden files, Save-name retention,
  replacement intent, cancellation and compact 540x360 layouts at both densities.
  Window controls now sit on the left, followed by a left-aligned title using
  Graphite's pinned Noto masks. Maximize/restore retains normal bounds through
  workspace resize and minimize; fixed clients expose disabled maximize. The
  final desktop guest suite covers 26 behaviors and over two million exact
  client pixels. The shell/catalog slice adds GUI/TUI launch identities and
  persistent user pins without changing native mode or x64 startup.
* Milestone 4 and the remaining Milestone 5 workspace, tiling, fullscreen,
  settings/file-association and Activity Monitor work remain planned. Packaging can
  proceed independently; the complete desktop is not a prerequisite for the
  graphical Syma worksheet.

See also the [LVGL and X11 assessment](lvgl-and-x11.md) and
[graphics architecture](architecture.md).
