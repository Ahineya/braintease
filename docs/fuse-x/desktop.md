# Fuse-X desktop integration

The desktop model is **one Fuse-X application with multiple native windows**.
Terminal and GUI windows share one Dock icon. People switch between windows
using a Finder-style Dock menu, the menu bar's Window menu, and normal macOS
window controls.

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

For the first graphics milestone, one window is enough. Establish window
handles and event routing then, so supporting several windows extends the same
interface. Begin with bitmap presentation, including RGB565. Add blitting or
other accelerated drawing only through an explicit, measured graphics contract.

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

Closing all terminal windows leaves the kernel and detached GUI applications
running. Terminal hangup rules, foreground jobs, and GUI detachment need an
explicit policy so closing a shell does not accidentally end an independently
launched GUI application.

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
window/surface/event boundary and the first RVM terminal. Fuse-X should implement
that contract with native top-level windows and capability discovery. Its native
GX backend remains planned; importing Nano-X does not supply that backend. See
the [milestones](../graphics/roadmap.md) for local IPC, multiple windows and
Nano-X lifecycle adaptation.
