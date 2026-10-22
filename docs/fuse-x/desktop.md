# Fuse-X desktop integration

The default desktop model is **one Fuse-X application with multiple native windows**.
Terminal and GUI windows share one Dock icon. People switch between windows
using a Finder-style Dock menu, the menu bar's Window menu, and normal macOS
window controls.

An optional **nested desktop** puts guest applications inside one macOS window,
for developing the same desktop policy that a future Riplix x64 display backend
can use:

```sh
scripts/run-fuse-x.sh --desktop
```

This implies `--gui`, builds and installs the five Graphite applications plus
`fusedeshell` and its application catalog, then starts the guest shell through
`/init`. The shell supplies the dock, searchable application menu and separate
Riplix/clock panels. Applications remain independent guest processes.
`--run-only --desktop` skips building and installation: it needs an already
installed shell or the staged host-share fallback described below.
Direct invocation can select `FUSE_X_DESKTOP=1`, with an optional
`FUSE_X_DESKTOP_SIZE=1280x800` logical initial size. Native-window mode remains
available through the normal launcher.

The first profile has opaque windows, title-bar dragging, a bottom-right resize
grip, focus and pointer capture. Close, minimize and maximize/restore controls
sit on the left of each title bar. Maximize fills the available workspace while
keeping the dock visible; restore returns to the previous bounds. Fixed-size
clients show an unavailable maximize control. Minimized windows return through
the grouped application dock, retaining their maximized state. Dragging a maximized title bar
restores its normal size before moving it.
Titles sit just after the controls, left aligned with a clear gap. Title and
dock menu labels use the same pinned Noto Sans glyph masks as Graphite apps, at
12 logical points with authored 1x/2x coverage.
The outer Mac window's red close button minimizes the desktop; the Dock or
Window menu restores it. An internal close button or Command-W requests that
application to close, allowing its unsaved-work dialog to keep it open. The
desktop uses portable window policy/composition with a separate guest Graphite
shell; application controls remain in their own processes and surfaces.
The [proposed FuseDE desktop contract](../graphics/graphite-roadmap.md#fusede-desktop-contract-proposed)
defines application identity, the grouped dock, Activity Monitor, multiple
workspaces, optional tiling and separate fullscreen spaces. The first grouped
dock and the first shell/catalog profile are implemented below. The current desktop still has one
workspace with maximize/restore; Activity Monitor, tiling and fullscreen spaces
remain planned.

`libraries/graphics/desktop.rs` supplies allocation-free window policy and packed
pixel composition. `hosted_desktop.rs` adapts retained published app frames,
input, density and presentation to one host window. Composition does not read
app pixels back into C16 buffers. Native-window and nested modes use the same
GX client interface; LVGL, SDL and terminal apps need no special desktop UI.
Nested presentation uses each client's logical size and reports 100% effective
presentation zoom; per-client enlargement remains a native-window feature.
The x64 display/authority adapter remains to be implemented independently of
the ongoing kernel and driver work.

## Session registry and window controls

The first session-service profile is implemented by the optional `GX_SESSION`
ioctl, declared in
[`gx-session.h`](../../ports/graphics/include/gx-session.h). This is a service API
inside the existing kernel/desktop adapter; it does not start another daemon.
The same portable policy handles title-bar gestures and programmatic commands.
The guest Graphite shell consumes these identities for its grouped dock and
window menus. The compositor retains fallback controls if the shell is absent.
Workspace switching, tiling, fullscreen spaces and Activity Monitor remain
subsequent milestones.

| Interface | Current behavior |
| --- | --- |
| `GXS_INFO` | Discover registry, watch, child association and nested-control capabilities. |
| `GXS_REGISTER` | Declare the caller's stable application ID, UTF-8 name and icon reference; receive execution and instance IDs. |
| `GXS_ATTACH_CHILD` | A registered parent explicitly associates its actual direct child with its instance. It cannot take over a child already registered as another application. |
| `GXS_NEXT_*`, `GXS_GET_WINDOW` | Read running instances, participating processes and windows through revision-checked snapshots. |
| `GXS_WATCH` | Acknowledge a processed revision and wait for coalesced changes through ordinary `/dev/gfx` `select()` readiness. |
| `GXS_CLAIM_MANAGER`, `GXS_RELEASE_MANAGER` | Claim/release exclusive process-bound manager authority; claiming requires root. |
| `GXS_CONTROL` | Activate, minimize, maximize, restore, move, resize or request close. Owners can control their own windows; cross-owner commands require the live manager authority. |

Window and execution identities are nonzero session-scoped values, never PIDs,
titles, list positions or addresses. An exec removes the old image's membership,
windows, watch subscription and authority; its next registration obtains a new
execution identity. Exit removes those resources too. Explicitly associated
helpers can retain their instance after its original owner exits. An inherited
descriptor or a copied numeric authority does not grant a child control.
Application IDs are self-declared grouping metadata, not package authentication.

Existing applications remain usable without registering: their windows expose
the actual owner PID/execution identity and title, with instance zero. The first
profile reports one ordinary workspace. Ordinary windows have normal roles;
the manager-owned shell overlay reports a panel role. Hidden windows
remain enumerable, while nested policy controls apply to mapped windows. Native
mode exposes registry metadata and client dimensions with placement `(0, 0)`;
it does not advertise guest placement/minimize/maximize control over macOS.

Start cursor reads with revision zero, then retain the returned revision. If
another relevant change occurs, `EAGAIN` requires restarting the snapshot.
Title, membership, geometry, focus and window-state changes advance the revision;
pixel publication and ordinary repainting do not. After reading, `WATCH` the
accepted revision before blocking in `select()`. An intervening change remains
ready until acknowledged. Global wakeups coalesce once per revision, so an
abandoned watcher does not repeatedly wake an otherwise idle desktop.

The C16 adapter explicitly marshals 338-cell records. Native/LP64 and x64
adapters must preserve the semantics with their own layouts; this work does not
install an x64 backend or alter process scheduling, memory ownership or drivers.
Registry storage is dynamic; actual process/window resources retain their
reported backend limits. The registry contains desktop participants, while
Activity Monitor's complete process accounting still comes from `/dev/proc`.

Verification uses disposable offline images:

```sh
python3 fuse-x/test_session_policy.py
python3 fuse-x/cc.py fuse-x/examples/session.c --static \
  --build build/fusede-session/sdk -o build/fusede-session/session-probe.fxe
python3 fuse-x/test_desktop_session.py \
  --kernel build/fusede-session/fuse-x \
  --probe build/fusede-session/session-probe.fxe --build fuse-x/build
```

Build the selected kernel normally, or isolate it with `BUILD=...` and
`NATIVE_RUNTIME=/absolute/path/to/librcc_native_runtime.a` when invoking Make.
The normal launcher tracks portable graphics Rust sources when checking its
host-tool cache. No normal disk image, startup service or application inventory
is changed by these tests. Acceptance receipts are under
`build/fusede-session/`: both guest profiles, the C authority/watch regression,
and the existing full nested-desktop input/pixel/idle/teardown regression pass.
Native GPU presentation remains separate from headless compositor acceptance.

## Graphite shell and application catalog

`fusedeshell` is a guest LVGL application, separate from the window manager and
compositor. It claims process-bound manager authority through `GX_SESSION`,
then creates one regional `GX_PANEL` overlay. Its centered bottom dock starts
with an **Applications** button on the left. That button opens a compact,
category-based menu with search, horizontally centered above the dock. While
typing in search, **Up/Down** selects an available result and scrolls it into
view, wrapping at either end. Holding an arrow repeats after 500 ms, then every
100 ms; releasing it or leaving the menu stops repetition. **Enter** launches
the selected app. Filtering or changing categories
selects the first available result. Unavailable entries are skipped. A fixed
**Run…** action opens a compact command dialog without a file picker. The top
left has a separate **Riplix** menu pill; the top right has a clock pill in the
form `Wed 21 Oct 22:43`, using the guest's local time. The space between them
remains available to client pixels and input. The Riplix menu opens Applications,
About FuseDE and Exit desktop shell; exiting the shell does not shut down the
session or close applications.

The normal `--desktop` build installs `fusedeshell`, `lvcalc`, `lvselect`,
`lvgallery`, `lvdata` and `lvedit` in `/bin`, with the curated INI catalog under
`/usr/share/applications`. Catalog entries also include Terminal, Commander,
Rift, Music, Telegram, Chat, Multiplexer and JavaScript. Missing executables are
marked unavailable; the menu never substitutes an inferred command. The shell
checks the installed absolute executable path first, then the entry's explicit
host-share fallback. Executable flags are respected. Launching uses exact
argument vectors and nonblocking exec-error/exit reporting, without shell
command evaluation. See the [catalog format and launch contract](../../ports/fusede/README.md#graphite-application-catalog).

For isolated host-share testing, stage the shell as
`/mnt/host/fusedeshell.fxe` and the catalog as
`/mnt/host/fusede/applications/*.ini`. `/init` prefers the installed shell and
selects `FUSEDE_APPLICATIONS=/usr/share/applications` when that directory exists,
otherwise `/mnt/host/fusede/applications`. A manually started shell can override
`FUSEDE_APPLICATIONS` explicitly. `--run-only` does not populate any of these
files. The standalone builder keeps `--app all` to the five ordinary apps;
`--app fusedeshell` builds the shell separately.

### Run a program

Applications → **Run…** accepts an application name, executable name or path,
with optional arguments. For example, `lvcalc`, `Commander` or
`/mnt/host/example.fxe "one argument"`. Known catalog apps keep their declared
GUI/TUI launch behavior and identity. Other names resolve through `PATH` (or
`/usr/bin:/bin` when unset); explicit relative paths use the shell's working
directory. **Run in terminal** explicitly wraps the program in gterm.

Quotes and backslash escapes group arguments. There is no implicit shell
expansion, redirection or pipeline evaluation; invoke a shell explicitly when
that behavior is desired. Run/Enter submits, Cancel/Escape dismisses, and
execution errors remain visible in the dialog. The dialog sizes to its controls,
adding space for a status or error only when present. This adds no global shortcut.

## Grouped application dock

The Graphite dock combines pinned launchers and running windows in one icon per
stable application ID, across separate instances. It has no separate region for
recently opened applications. Pinned apps remain after their last window closes;
unpinned apps remain while they have eligible windows. A running indicator and
active tile expose state, hover shows the app name, and the dock scrolls when
needed. Only mapped or minimized ordinary top-level windows appear in window
menus. Hidden windows, the shell's own overlay, embedded file pickers and
windowless helpers do not create entries.

* Click an idle pinned launcher to start its application.
* Click a running app with one window to activate and restore it; several windows
  open a chooser.
* Right-click an app for individual windows, **Open new**, **Pin to dock** or
  **Unpin from dock**, plus group show, minimize and cooperative close actions.
* Escape or an outside click dismisses a menu. Closing remains a request, so an
  editor may retain its unsaved-work dialog.

Pins are user overrides at `$HOME/.config/fusede/pins.ini`, merged with each
catalog entry's `pinned` default. `FUSEDE_PINS` can select a separate file for
isolated tests. Pin/unpin saves through an exclusive temporary file and atomic
rename; unavailable or unknown valid application IDs retain their preference.
Normal image installation updates system catalog entries without replacing user
pins. Guest crash durability awaits a linkable libc `fsync`; host model builds
also flush before rename.

The five Graphite apps declare their identities with
[`gx-application.h`](../../ports/graphics/include/gx-application.h):
`org.riplix.Calculator`, `org.riplix.TextEditor`, `org.riplix.GraphiteGallery`,
`org.riplix.DataViewer` and `org.riplix.FilePicker`. Ordinary `gterm` registers
`org.riplix.Terminal`. The catalog launches TUI programs through gterm's explicit
`--app-id`, `--app-name` and `--app-icon` options. Commander, Rift, Music,
Telegram, Chat, Multiplexer and JavaScript therefore group as their own apps;
their shell and helper processes add no icons. Metadata is a self-declaration,
not privileged authority or authenticated package identity.

Native-window mode retains its single macOS Fuse-X icon and native window
menus. Unregistered clients remain usable under an honest execution identity.
Legacy Nano-X clients such as NetSurf and `nxview` remain available as the earlier
porting experiments. Extending their catalog integration is not a current goal;
the browser choice will be reconsidered separately.

### Regional panels and fallback controls

[`gx-panel.h`](../../ports/graphics/include/gx-panel.h) defines the optional
`GX_PANEL` profile. INFO discovers nested-output support and dimensions.
CONFIGURE requires the live manager authority and an owned output-sized GX
window. It declares bounded paint/hit rectangles separately from top/bottom
work-area reservations. The shell exposes only its dock, two top pills and
current menu; ordinary windows remain visible and clickable through the gaps.
The optional alpha profile retains straight BGRA coverage from LVGL through
`GX_PANEL_UPLOAD` and the existing atomic `GX_PRESENT` publication. Rounded
corners reveal client pixels beneath them, including antialiased edges, rather
than a baked desktop-colored rectangle. Fully transparent dock pixels do not
intercept pointer input; modal menus still consume dismissal clicks.
Maximize respects the reserved edges without painting full-width strips over
floating clients. Open menus temporarily request keyboard/modal input;
dismissal consumes the outside click, while existing client capture still gets
its matching release.

The backend validates geometry, ownership and authority. Hidden overlays do not
reserve or intercept input. Release, destruction, manager revocation, exec or
exit removes the panel and restores the compositor's grouped fallback dock.
`/init` probes this capability before launching Graphite; native-window and
rescue launches retain graphical terminals. A missing or failed shell falls
back to usable controls and a terminal without killing existing clients. The
panel profile is nested-only; it does not add an x64 display or authority
adapter.

The fallback dock still uses portable grouping/controller modules and pinned
Lucide/Noto assets. It supports window menus, activation, group actions,
scrolling and minimized-window recovery. It is suppressed while the guest
shell owns its overlay. Icon alignment and indicator refinements remain visual
polish. Workspaces, tiling, dedicated fullscreen spaces, a settings GUI, file
associations and Activity Monitor remain planned.

A configurable DE shortcut service is also planned, with distinct Ctrl/Shift/
Alt/Super semantics, named actions and possible INI overrides plus a later
settings GUI. No exact bindings are chosen yet; see the
[planned input/action contract](../graphics/graphite-roadmap.md#global-shortcuts-and-desktop-actions-planned)
for global Run-dialog and screenshot shortcut requirements. The existing hosted menu shortcuts
are not a general configurable service.

### Build and verification

```sh
scripts/run-fuse-x.sh --desktop
# Independent host-share builds; these commands do not open a disk image:
python3 ports/lvgl/build.py --app all --stage fuse-x-shared
python3 ports/lvgl/build.py --app fusedeshell --stage fuse-x-shared
```

The normal launcher opts into the cached Graphite application set before
building, installs the six executables/catalog on its normal install path and
starts the shell only in nested mode. Explicit `--disk` retains the launcher's
existing installation policy; use `--install-apps` when updating such a disk.
Native mode and x64 boot behavior remain unchanged.

Independent tests use disposable offline images or temporary host fixtures:

```sh
python3 ports/lvgl/test_shell_catalog.py
python3 fuse-x/test_graphite_integration.py
# After building the normal desktop application set:
python3 fuse-x/cc.py fuse-x/examples/graphite-tui.c --static \
  --build build/fusede-shell -o build/fusede-shell/graphite-tui.fxe
python3 fuse-x/test_graphite_shell.py --kernel fuse-x/build/fuse-x \
  --shell fuse-x/build/apps/fusedeshell.fxe \
  --calculator fuse-x/build/apps/lvcalc.fxe --terminal fuse-x/build/apps/gterm.fxe
python3 fuse-x/test_dock_model.py
python3 ports/graphics/tests/test_application.py
python3 fuse-x/test_desktop_dock.py --kernel build/fusede-dock/fuse-x \
  --probe build/fusede-dock/dock-probe.fxe
python3 fuse-x/test_desktop_app_groups.py --kernel build/fusede-dock/fuse-x
```

The shell suite exercises real app launches, search, pin persistence, grouped
GUI/TUI instances, panel gaps, menus, resize/density and fallback recovery.
Run checks cover names/paths, quoted terminal arguments, cancellation and launch
failures. Pixel checks compare transparent/partial/opaque panel pixels over an
actual white GX client at both densities, including rounded corner input and
modal dismissal. Test-only frame capture writes `<window>.alpha.pgm` beside the
ordinary RGB PPM when `FUSE_X_FRAMES` is enabled for an alpha panel.
Catalog tests cover malformed metadata, dynamic availability, exact arguments
and child lifecycles. Build tests check ABI/source/cache invalidation and the
read-only installation manifest. Earlier grouping and terminal/calculator suites
remain pixel/input regressions. Native GPU presentation remains a separate
acceptance step from the headless production compositor.

## Implemented native windows

Run `scripts/run-fuse-x.sh --gui` for a native terminal and a persistent Nano-X
service. Launch `nxclock &`, `nxeyes &`, or `nxterm &` from the shell; each
Nano-X top-level becomes its own Mac window. `startx` also works from console
mode: it ensures the service is running, opens `gterm`, then
returns to the shell. It reuses an existing service. No NanoWM or composite
desktop is started in hosted mode. The RVM desktop keeps its own window manager.

NXDSKTOP's independent companions `nxcalc`, `nxselect`, `nxmsg`, `nxtetris` and `nxview`
also run as native windows. The selector and message box preserve redirected
stdout for their results; close/Escape returns `[]`, OK returns a path or `OK`.
The NXDSKTOP taskbar itself remains an RVM desktop component. See the
[shared application guide](../../ports/nanox/README.md#nxdsktop-and-companion-apps).

Multiple GX and Nano-X programs share one `Fuse-X.app` identity. File → New
Terminal / Command-N creates a fresh PTY shell, including after the last window
closes. Window and Dock lists activate and restore individual windows. Quit /
Command-Q requests orderly guest shutdown. The reconnectable console remains
available alongside the GUI.

The hosted GX backend supports up to sixteen surfaces, with XRGB8888 preferred
and RGB565 available for compatible clients. Native Nano-X
uses one private canvas per top-level; child widgets draw and clip within that
canvas. Overlapping virtual coordinates never make one Mac window obscure
another's pixels. The virtual root is not displayed. Top-level placement belongs
to macOS; guest top-level move requests are ignored, while child moves retain
normal Nano-X semantics. Titles, map/unmap, reparent, destruction, focus and
close requests are adapted at the server boundary. Explicit raise/focus
requests activate the corresponding Mac window.

Eyes also follows the cursor across the Mac desktop, including outside Fuse-X
windows. The UI thread samples AppKit's desktop cursor relative to the requested
window; `GX_DESKTOP_POINTER` exposes signed guest-pixel coordinates without
injecting motion, clicks or keyboard input. Native `GrQueryPointer` uses the
calling client's first mapped top-level as its coordinate origin. Eyes polls
this query at 20 Hz through the existing Nano-X protocol. Normal input routing
and the RVM client remain unchanged.

Resizable Nano-X windows receive content-size updates, resize their canvases,
and notify clients through normal Nano-X events. The maximum accepted size is
1920×1080 guest pixels; allocation failure restores the previous size without
terminating the application. Growth reserves the canvas and backing store
together; shrinking compacts existing storage without a second canvas.
The SDK can use the native 192-bank heap described in
[native executables](../../fuse-x/EXECUTABLES.md). Native presentation ignores
stale frame geometry while a newer resize is being acknowledged.
Both `nxterm` and `gterm` resize their terminal models, update PTY rows/columns
and deliver `SIGWINCH` to foreground applications. `gterm` rounds its content
size to whole 8×16 cells; `nxterm` also supports spare edge pixels.
Fixed-size clients such as Clock remain fixed. The sixteen-PTY pool is shared by native terminals, Nano-X
terminals and Telnet sessions; surfaces also consume bounded guest memory.

Host-folder/network menus, Finder launch configuration, shaped
transparent host windows and a high-DPI layout policy remain follow-up work.
See the [runtime guide](../../fuse-x/README.md#native-graphics) for checks.

## Text clipboard

Edit → Copy, Cut, Paste and Select All (Command-C/X/V/A) route to the focused
hosted window. They are typed `GX_ACTION` events in the ordered input queue;
they never synthesize printable shortcut letters or terminal Ctrl-C. Ordinary
Ctrl-C still belongs to the PTY and foreground job. Apps must implement these
commands for their own editing/selection model; the host cannot infer text from
arbitrary RGB565 pixels.

`nxterm` and `gterm` accept Command-V and Ctrl-Shift-V. Drag to select visible
terminal text, then Command-C or Ctrl-Shift-C to copy; Command-A selects the
visible screen. Terminal Cut copies the selection without deleting output.
Selection is cleared when output changes or the terminal resizes. There is no
scrollback selection yet. Pasting preserves UTF-8, normalizes line endings and
filters embedded control characters other than tabs/newlines. An app requesting
DEC mode 2004 receives bracketed paste. Input is queued in order, with partial
PTY writes and backpressure; a paste can exceed the PTY's input ring.

NetSurf supports these commands in its address editor and through its core
clipboard callbacks for page selections and HTML fields. Bare web addresses
use HTTPS; explicit HTTP and local file addresses remain available.

`GX_CAP_CLIPBOARD` advertises `GX_CLIPBOARD_READ/WRITE`. Requests require a
window lease owned by the caller. Plain UTF-8 text is limited to 32,768 octets,
one octet per C16 cell, with no embedded NUL. Writes validate the complete text
before publication. Buffers are borrowed synchronously; the host keeps owned
strings, never guest pointers. AppKit's main thread synchronizes the macOS
text pasteboard and publishes guest copies. Non-text or oversized host content
is unavailable as text; images, rich text and file promises are outside this
contract.

Nano-X private request 251 adds `nx_clipboard_get/set`, using 512-cell chunks
and per-connection transfer state. A read is a stable snapshot, a write commits
atomically, disconnect frees staging, and a client must own the named window.
The common RVM server supplies a local text clipboard; macOS sharing is a
Fuse-X backend feature and adds no VM/MMIO requirement. Other GUI apps need
clipboard hooks before their own editors can copy/paste.

Checks: `python3 ports/graphics/tests/run.py`,
`python3 ports/nanox/tools/test_clipboard.py`, runtime hosted-graphics tests,
`python3 ports/nanox/tools/clipboard_guest.py` (native and RVM/JIT),
and `python3 fuse-x/test_clipboard.py` against disposable guest disks.

## Launching and using windows

Start the graphical session with a Riplix terminal running a shell. A GUI program
launched from that shell, or from another GUI program, creates its own native
window. Riplix owns the program and its content; macOS manages window placement,
focus, resizing, minimizing, and native chrome.

Each window needs a stable handle, an owning process, a title, dimensions, a
presentation surface, and an event queue. Route keyboard, pointer, focus,
resize, and close events to that window. Clear held input state on focus loss
so switching windows cannot leave keys or mouse buttons stuck.

Closing a window delivers a close request to its owner. Applications need an
opportunity to handle unsaved work. Process exit releases its remaining windows;
closing an arbitrary window must not stop the whole kernel.

Presentation preserves the negotiated surface format. Further drawing acceleration should
follow measured needs and preserve this window/surface boundary.

## One Dock icon and a window list

The agreed Dock behavior uses one application identity. The earlier idea of
separate macOS presenter applications is superseded by the window-list model.

An illustrative menu is:

```text
Terminal — shell
Terminal — build log
Editor — notes
Paint — untitled
--------------------
New Terminal
--------------------
Show All Windows
Hide
Quit Fuse-X
```

Window entries reflect currently open terminal and GUI windows. Selecting an
entry activates Fuse-X, restores the window if minimized, and brings it forward.
Use meaningful, distinct titles and update entries when windows change title
or close. Keep the Window menu consistent with the same registry.

macOS Dock menus support lists of open windows and custom actions. AppKit's
`applicationDockMenu(_:)` supplies custom entries where needed. Use normal
window registration and system-provided actions where they already provide the
desired behavior, without duplicating them. See Apple's
[Dock menu guidance](https://developer.apple.com/design/human-interface-guidelines/dock-menus)
and [Dock menu delegate API](https://developer.apple.com/documentation/appkit/nsapplicationdelegate/applicationdockmenu(_:)).

## Terminal and session lifetime

New Terminal creates a separate terminal endpoint and a new Riplix shell, rather
than attaching several windows to one global input stream. Opening terminals
must work after every earlier terminal window has closed.

Closing all terminal windows leaves the kernel, persistent Nano-X service and
GUI clients running. Native Clock, Eyes, Applications and Nano-X Terminal drop
inherited terminal standard descriptors after connecting, replacing only TTY
stdio with `/dev/null`; redirected files and pipes remain available. They ignore
launching-shell hangup while keeping their original PID/job identity, so explicit
termination still works. Nano-X Terminal restores normal hangup handling in its
child shell. Its close button hangs up that shell and its jobs, while GUI clients
launched there survive. Use `&` to return immediately to a launching shell.
The `startx` supervisor also releases inherited TTY descriptors in its children.
Set `NANOX_KEEP_TTY=1` to retain terminal diagnostics during development.

Keep Fuse-X available with no open windows. The Dock's New Terminal command and
the menu bar can create another terminal. Reopening Fuse-X from its Dock icon
should restore an existing useful window or create a terminal when none remain;
the exact selection rule is still open. AppKit supports keeping an application
alive after its last window closes through its
[termination delegate](https://developer.apple.com/documentation/appkit/nsapplicationdelegate/applicationshouldterminateafterlastwindowclosed(_:)).

Quit Fuse-X is a session shutdown request: finish guest shutdown, flush storage,
and then release the host resources. It is distinct from closing a terminal or
GUI window.

## Native menu proposal

| Menu area | Candidate commands |
| --- | --- |
| Fuse-X | About, settings, orderly shutdown |
| Terminal | New Terminal, close the current terminal |
| Window | Current window list, focus and arrangement actions |
| Shared Folders | Add a folder, manage exports and access modes |
| Network | Disconnect, host-only/shared attachment, connection information |

Expose New Terminal through both the menu bar and Dock menu. Menu actions must
use the same guest-side lifecycle operations as the shell and GUI launchers.
Final menu labels and shortcuts remain open; Command-N is the proposed shortcut
for New Terminal.

## Later visual work

Guest widgets and application contents can use a consistent dark theme while
native mode keeps macOS window chrome. A standalone RVM desktop may add its own
chrome, compositor, and dock-like launcher behind the same surface interface.
Native widgets, high-DPI behavior, and drawing
acceleration remain separate design tasks.

## Shared graphics contract

The [GX architecture](../graphics/architecture.md) defines the C16 guest-facing
window/surface/event boundary and the first RVM terminal. Fuse-X implements
native top-level windows, capability discovery, hidden creation, resize
acknowledgement and activation. Native build adaptations are generated under
`fuse-x/build/`; shared Nano-X and RVM application sources stay unchanged.

Headless acceptance checks set `FUSE_X_FRAMES` for published snapshots and
`FUSE_X_TEST_EVENT` for an atomic one-command mailbox. A JSON object containing
`window` and one of `resize: [width, height]`, `close: true`, `input` (an input
transition), or `action: 1..4` (copy/paste/cut/select-all) follows the same
host event path as a native gesture. An action may supply a bounded UTF-8
`clipboard` string to test pasting without touching the OS pasteboard. The mailbox is enabled only with
`RCC_NATIVE_HEADLESS`; normal sessions do not read it.
