# FuseDE roadmap

The next implementation milestone is a usable NXDSKTOP environment and its
companion applications. The custom FuseDE shell follows the widget evaluation;
it is not a prerequisite for useful GUI apps on either platform. All stages
below are planned unless explicitly identified as an existing foundation.

## 1 Port NXDSKTOP and companion apps

Use the pinned source and existing adaptation/build layers. RVM already has
Nano-X, NanoWM and the first applications; Fuse-X already has native Nano-X
windows. Preserve those working paths while adding a selectable NXDSKTOP
session. Keep `nxlaunch` available as a minimal fallback. Make the working
NXDSKTOP session the basic RVM desktop once its lifecycle checks pass.

| Program | Port scope |
| --- | --- |
| `nxdsktop` | Start menu, clock, resource display, application launching and platform session actions |
| `nxmsg` | Message and confirmation dialogs |
| `nxselect` | File selection and editor/viewer launch workflow |
| `nxcalc` | Standalone calculator |
| `nxmine`, `nxtetris` | Small interactive games |
| `nxjpeg` | Image viewer, including decoder and image protocol dependencies |
| `nxworld` | World-map demo, including required data and drawing operations |
| `nxterm`, `nxclock`, `nxeyes` | Reuse existing ports and integrate launcher entries |
| `nxroach` | Retain the existing RVM root-desktop effect; native desktop overlays are separate work |

Deliver the shell, dialogs, selector and calculator first, then games, viewer
and map. The viewer/map are part of the companion app scope but must not block
the initial lightweight desktop: image decoding, asset imports and optional
drawing operations may require more work than the shell itself. Track each app
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

Add visible resize grips and larger border hit areas, then maximize/minimize,
restore, usable work-area reservation and window activation/list notifications.
Retain PTY geometry updates and SIGWINCH when terminals resize. Confirm focus
and pointer capture across dragging, cancellation and client exit.

Plan and implement larger RVM guest resolutions with explicit scanout, backing
surface and application budgets. Keep logical guest resolution distinct from
host window scale. Test allocation failure and resource reuse; raise quotas
when the measured desktop workload justifies them.

Acceptance: resize controls are discoverable, windows remain recoverable,
maximized windows avoid reserved panels, and ordinary resource failures leave
the server and other apps usable. Native Fuse-X keeps macOS window management.

## 3 Evaluate Nuklear and establish shared UI

Build the file-list prototype defined in [Architecture](architecture.md).
Compare its responsiveness and memory use on RVM JIT and native Fuse-X.
Record the toolkit decision and measurements before building all applications
against it. Keep any required C16 adaptations explicit and reproducible.

Add the minimum color image protocol, cached assets, proportional fonts and
shared theme needed to demonstrate real controls. Provide a widget showcase
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
