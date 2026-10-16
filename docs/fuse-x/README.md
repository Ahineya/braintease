# Fuse-X

**Fuse-X** is the name for the planned native, hosted FUZIX environment, starting
on macOS. FUZIX supplies the operating-system services and applications; the
native host integrates its terminals and graphics windows with the Mac desktop.

The intended experience is one persistent Fuse-X session. It starts with a
terminal and shell. GUI programs launched from that shell, or from another GUI
program, open their own native windows. **All windows belong to one Fuse-X
application, with one Dock icon and a Finder-style window list.**

This is the design baseline from the October 16, 2026 discussion. Product
direction, proposals, and implemented foundations are distinguished below.

## Guide

| Document | Contents |
| --- | --- |
| [Architecture](architecture.md) | Native execution, shared kernel state, process memory, and host boundaries |
| [Desktop integration](desktop.md) | Terminal and GUI windows, Dock navigation, menus, and session lifetime |
| [Storage](storage.md) | Disk images, shared Mac folders, and filesystem integration questions |
| [Networking](networking.md) | Headless access, host attachment, and network controls |
| [Roadmap](roadmap.md) | Implementation stages, dependencies, acceptance criteria, and open decisions |

## Agreed direction

- Use the name **Fuse-X** for native hosted FUZIX. Keep the existing Ripple FUZIX
  port useful alongside the hosted platform.
- Execute compiler-generated native code. Retain the current compatibility data
  layout initially, with explicit adapters for host facilities.
- Start headlessly. A network terminal may arrive before graphics if networking
  is ready first.
- Begin graphics with one window, while designing the interface to support
  independent terminal and GUI windows later.
- Let macOS manage native window placement, focus, and chrome. FUZIX programs
  supply their window contents.
- Use one macOS application identity and Dock icon. Offer a window list like
  Finder's Dock menu; selecting an entry switches to that window. Separate
  macOS presenter applications are not part of this model.
- Keep the session running when terminal windows close. Provide a way to open
  another terminal from the native menus or Dock menu.
- Expose useful host controls, including networking, through the native UI.

## Proposals to resolve

The storage proposal is a FUZIX root disk image plus explicitly shared Mac
folders. The exact filesystem bridge, metadata mapping, and mount behavior
remain open. See [Storage](storage.md).

Window command formats, GUI launch behavior, close handling, terminal session
policy, and the menu layout still need implementation decisions. Dock window
switching through the single application is the target behavior; the exact
AppKit integration is an implementation choice.

A dark visual theme, consistent widgets, optional accelerated drawing, and a
standalone desktop with its own chrome or dock remain later design ideas. The
hosted window API should leave room for a standalone compositor backend.

## Implemented foundation

The native compiler/runtime already has resumable C continuations, cooperative
fork/yield/wait/exit, live-value preservation, copy-on-write private memory, and
an explicit shared kernel arena. Standalone examples cover recursive calls,
independent process memory, shared kernel state, and multiple live processes.

These are runtime building blocks. They do not yet constitute a booted Fuse-X
kernel, a native FUZIX executable loader, or the desktop described here. The
current native display host manages one RGB565 window and treats its closure
as shutdown; Fuse-X needs a different window/session lifecycle.

Use [native process documentation](../../rcc-native/PROCESSES.md) for current
APIs, limits, examples, and validation. Use [networking documentation](../networking/README.md)
for the existing device and protocol foundations. The
[FUZIX port](../../ports/fuzix/README.md) remains the source of port-specific
build and runtime information.

## Maintaining this design

Update these pages when a decision changes or a milestone is validated. Mark a
feature implemented only when the Fuse-X integration itself works; success in
a standalone runtime fixture is evidence for its foundation. Keep detailed
compiler and device contracts in their existing documentation and link to them.
