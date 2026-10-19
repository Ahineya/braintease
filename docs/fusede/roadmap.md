# FuseDE roadmap

The initial NXDSKTOP environment, message box, file selector, calculator and Tetris
are implemented. The skin authoring/geometry foundation and first guest colour
showcase are delivered on both platforms; see [colour resources](colour-resources.md).
Follow the [implementation plan](implementation-plan.md)
for colour assets, window roles, shared controls and the custom shell. Remaining
companion applications can proceed independently. Every new shared application,
including the horizontal/vertical applications bar, must build for RVM and
Fuse-X. The custom desktop shell targets RVM. Later stages remain planned.

## 1 Port NXDSKTOP and companion apps

The first batch uses the pinned source and existing adaptation/build layers. RVM already has
Nano-X, NanoWM and the first applications; Fuse-X already has native Nano-X
windows. Preserve those working paths while adding a selectable NXDSKTOP
session. `startx` now selects NXDSKTOP, while `startx --minimal` keeps
`nxlaunch` available as a fallback.

| Program | Port scope |
| --- | --- |
| `nxdsktop` | Start menu, clock, resource display, application launching and platform session actions |
| `nxmsg` | Message and confirmation dialogs |
| `nxselect` | File selection and editor/viewer launch workflow |
| `nxcalc` | Standalone calculator |
| `nxtetris` | Delivered on RVM and Fuse-X: timed gameplay, controls and persistent high score |
| `nxmine` | Planned small interactive game |
| `nxview` | Delivered shared-library PNG/JPEG/GIF/BMP viewer, file selection, fit, zoom and pan on RVM and Fuse-X |
| `nxworld` | World-map demo, including required data and drawing operations |
| `nxterm`, `nxclock`, `nxeyes` | Reuse existing ports and integrate launcher entries |
| `nxroach` | Retain the existing RVM root-desktop effect; native desktop overlays are separate work |

Validated initial batch: RVM JIT exercises the real Start menu, About/Help
acceptance and cancellation, calculator integer/decimal arithmetic and error
recovery, file selection/cancellation, literal filename arguments to Levee,
saving, status refresh, sync, desktop exit/reopen and host close. The disposable
disk passes filesystem checking afterward. Fuse-X's native integration checks
dialog cancellation/result files, calculator rendering and independent window
close alongside its existing graphics/session checks.

The shell, dialogs, selector, calculator, Tetris and image viewer are delivered;
next add Mines and map. Image decoding and transport are shared with NetSurf;
the viewer remains a separate application. Track each app
as built, installed and validated separately on RVM and Fuse-X.

Replace ELKS memory reporting with our resource interface. Use our editor,
filesystem paths and shutdown services. Avoid shell-string interpolation of
selected filenames; launch with explicit arguments. Make dialog pipes
nonblocking and handle cancellation, partial results, child exit and reaping
without freezing the UI. Use event/deadline-driven clock updates and retain
existing user configuration during disk upgrades.

Acceptance:

- RVM starts the basic desktop, launches multiple apps and returns cleanly to
  the console when the graphics session ends; the session can be reopened.
- File selection opens the chosen file in Levee through `nxterm`; cancellation,
  spaces in paths and ordinary failures have correct behavior.
- Independent companion apps work as Fuse-X native windows without a guest
  desktop, preserve other clients on close, and respect native session lifetime.
- Games respond to input, timers advance, dialogs recover from closing or
  killing clients, and the idle desktop does not busy-poll.
- RVM checks use JIT and private disk images, including filesystem consistency
  after shutdown. Fuse-X gets its own native integration and visual checks.

## 2 Improve window management and workspace

Delivered foundation: [workspace services](workspace.md), Slate NanoWM frames,
border/grip resizing, minimize/maximize/restore, Alt+Tab, stable window IDs,
enumeration and coalesced notifications. RVM panels reserve edges, preserve
pointer-driven typing focus and release reservations on disconnect. Fuse-X
shares enumeration/actions with an ordinary native panel fallback and explicit
capability limits. Manager disconnection preserves application windows for a
replacement manager. Popup/modal policy, automatic session-manager restart,
native borderless chrome and larger RVM resolutions remain follow-ups.

Continue with transient popup capture and modal rules, keeping decoration,
focus, stacking, work-area reservation and visual/input shape as separate
contracts. Fuse-X uses an ordinary-window panel fallback until native borderless
translation is added. See the
[window-role contract](implementation-plan.md#b-window-roles-borderless-panels-and-a-usable-workspace).

Keep PTY geometry updates and SIGWINCH when terminals resize, and retain focus
and pointer capture checks across dragging, cancellation and client exit as the
workspace grows.

Plan and implement larger RVM guest resolutions with explicit scanout, backing
surface and application budgets. Keep logical guest resolution distinct from
host window scale. Test allocation failure and resource reuse; raise quotas
when the measured desktop workload justifies them.

Acceptance: resize controls are discoverable, windows remain recoverable,
maximized windows avoid reserved panels, and ordinary resource failures leave
the server and other apps usable. Native Fuse-X keeps macOS window management.

## 3 Evaluate Nuklear and establish shared UI

The bounded colour-image path, Slate nine-slice renderer, tintable Lucide icons
and `fdeshowcase` are delivered on both platforms. Continue from that foundation;
the browser study remains an authoring tool. Reusable controls and the complete
Nuklear evaluation are still planned.

Build the file-list prototype defined in [Architecture](architecture.md).
Compare its responsiveness and memory use on RVM JIT and native Fuse-X.
Record the toolkit decision and measurements before building all applications
against it. Keep any required C16 adaptations explicit and reproducible.

Extend the delivered colour image protocol and cached assets with proportional
fonts and the shared theme needed for real controls. Provide a widget showcase
covering focus, keyboard navigation, menus, dialogs, text editing and scrolling.

Acceptance: idle blocking, responsive editing/scrolling, correct repaint after
occlusion/resize, bounded memory, and cleanup after application failure. The
selected toolkit must support our visual design without forcing continuous
full-desktop redraws.

## 4 Build the custom desktop shell

Add wallpaper, a top bar with system menus/clock/status, and an application dock
with launch, running indicators, activate, minimize and restore. Use the shared
theme for controls and RVM decorations. Implement the window-manager interface
before duplicating window state in the shell.

Keep the dock as a separate application (`fdebar`), with horizontal/vertical
layout, edge settings, overflow and keyboard navigation. On RVM it is a
borderless panel reserving workspace; on Fuse-X the same application builds as
an independent applications bar in its own native window. Use capability-based
chrome/shape fallbacks and the hosted activation service. Closing/restarting
the bar must preserve other applications. This does not replace the macOS Dock.

Keep NXDSKTOP selectable as the simpler alternative. The custom shell is an
RVM desktop choice; installing its shared widgets and apps on Fuse-X must not
replace native macOS window management.

Acceptance: multiple independent applications can be launched and switched,
minimized windows can be recovered, panels reserve space, and shell restart
does not unnecessarily kill applications.

## 5 Build the file manager and desktop services

Start with path navigation, list/icon views, selection and opening files. Add
rename, copy, move and delete with progress, error handling and confirmation
where data can be lost. Keep long operations from blocking GUI event handling.
Implement against guest filesystem services so both platforms share the logic.

Add file associations, clipboard transfers and drag-and-drop through explicit
interfaces. Introduce application menu/action registration for a true global
menu bar, leaving older apps usable without it. Map those actions to native
Fuse-X menus as a separate host integration milestone.

Acceptance: operations handle directories, collisions, permission/storage
errors and cancellation correctly; changes persist across orderly sessions;
dialogs and applications remain keyboard-operable and responsive.
