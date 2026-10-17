# Fuse-X desktop integration

The desktop model is **one Fuse-X application with multiple native windows**.
Terminal and GUI windows share one Dock icon. People switch between windows
using a Finder-style Dock menu, the menu bar's Window menu, and normal macOS
window controls.

## Implemented native windows

Run `scripts/run-fuse-x.sh --gui` for a native terminal and a persistent Nano-X
service. Launch `nxclock &`, `nxeyes &`, or `nxterm &` from the shell; each
Nano-X top-level becomes its own Mac window. `startx` also works from console
mode: it ensures the service is running, opens Applications and Terminal, then
returns to the shell. It reuses an existing service. No NanoWM or composite
desktop is started in hosted mode. The RVM desktop keeps its own window manager.

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

Resizable Nano-X windows receive content-size updates, resize their canvases,
and notify clients through normal Nano-X events. The maximum accepted size is
1024×768; allocation failure restores the previous size. Terminal clients
update their PTY rows/columns. Fixed-size clients such as Clock remain fixed.
Standalone GX programs such as `gterm` keep their fixed guest resolution and
scale on host resize. The four-PTY pool is shared by native terminals, Nano-X
terminals and Telnet sessions; surfaces also consume bounded guest memory.

Clipboard, host-folder/network menus, Finder launch configuration, shaped
transparent host windows and a high-DPI layout policy remain follow-up work.
See the [runtime guide](../../fuse-x/README.md#native-graphics) for checks.

## Launching and using windows

Start the graphical session with a FUZIX terminal running a shell. A GUI program
launched from that shell, or from another GUI program, creates its own native
window. FUZIX owns the program and its content; macOS manages window placement,
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

New Terminal creates a separate terminal endpoint and a new FUZIX shell, rather
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
Native widgets, clipboard integration, high-DPI behavior, and drawing
acceleration remain separate design tasks.

## Shared graphics contract

The [GX architecture](../graphics/architecture.md) defines the C16 guest-facing
window/surface/event boundary and the first RVM terminal. Fuse-X implements
native top-level windows, capability discovery, hidden creation, resize
acknowledgement and activation. Native build adaptations are generated under
`fuse-x/build/`; shared Nano-X and RVM application sources stay unchanged.
