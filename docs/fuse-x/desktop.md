# Fuse-X desktop integration

The desktop model is **one Fuse-X application with multiple native windows**.
Terminal and GUI windows share one Dock icon. People switch between windows
using a Finder-style Dock menu, the menu bar's Window menu, and normal macOS
window controls.

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

The hosted GX backend supports up to sixteen RGB565 surfaces. Native Nano-X
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

Presentation uses RGB565 bitmap surfaces. Further drawing acceleration should
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
