# Graphite applications and desktop roadmap

Agreed direction, 21 October 2026. Build a coherent application framework on
LVGL and GX, using useful applications to exercise each shared component.
Fuse-X is the first target; Riplix x64 follows when its graphics and application
services are ready. RVM remains supported by its existing graphics stack, but
does not set the performance or visual limits for this work.

Shared actions, split panes, forms and the table-and-plot workbench are
implemented. The coordinated next tracks are the document/editor framework
and a bounded desktop-in-one-window prototype. Each has independent acceptance;
one owner coordinates changes at the shared GX/runtime boundary.
The larger destination is a graphical Syma worksheet and a small desktop built
from the same application components.
Graphite is also the successor to the earlier Nano-X/Slate FuseDE experiment.
That experiment remains available for existing RVM users and compatibility
checks; new desktop and application work goes into this shared framework.

## Existing foundation

The [LVGL port](../../ports/lvgl/README.md) supplies native GX windows, full-color
rendering with RGB565 fallback, live 1x/2x density, compact Noto Sans typography,
Graphite semantic styles and Lucide icons. Calculator, file selector, interface
gallery and Data Viewer are staged separately in `fuse-x-shared`.

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

Next establish application identities and a launcher/task list, then settings
and file associations. Keep window policy, rendering and shell widgets separate;
the first two can be reused by the x64 display adapter without importing macOS
window code. Retire old launch paths only when their replacement has equivalent
session and application lifecycle coverage.

Fuse-X retains native host windows and adds an optional nested desktop session.
The foundation's portable window policy and packed compositor remain separate
from its macOS presenter. The complete Graphite shell, launch services and a
public desktop-manager authority contract follow the prototype. Keep desktop
policy above the GX window/surface/event boundary so Riplix x64 can supply its own backend. X11
compatibility is a separate project for existing applications, not a prerequisite
for the worksheet or the LVGL application framework.

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
already have a bounded Fuse-X profile. It currently excludes audio, threads,
OpenGL and several other SDL services; this roadmap does not assume them.

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
* Milestone 5 foundation: optional single-window Fuse-X session implemented in
  parallel with the editor. This is the portable window/composition foundation,
  with simple prototype chrome; the full Graphite FuseDE shell follows.
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
  client pixels. A full graphical shell and application launcher remain next.
* Milestone 4 and the full Milestone 5 shell remain planned. Packaging can
  proceed independently; the complete desktop is not a prerequisite for the
  graphical Syma worksheet.

See also the [LVGL and X11 assessment](lvgl-and-x11.md) and
[graphics architecture](architecture.md).
