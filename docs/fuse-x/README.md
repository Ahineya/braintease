# Fuse-X

**Fuse-X** is the name for the planned native, hosted Riplix environment, starting
on macOS. Riplix supplies the operating-system services and applications; the
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
| [FuseDE](../fusede/README.md) | NXDSKTOP companion apps, shared widgets and the custom RVM desktop direction |
| [Storage](storage.md) | Disk images, shared Mac folders, and filesystem integration questions |
| [Networking](networking.md) | Headless access, host attachment, and network controls |
| [Terminal browser](../networking/terminal-browser.md) | HTTP browser porting direction, curses prerequisites and shared select contract |
| [Roadmap](roadmap.md) | Implementation stages, dependencies, acceptance criteria, and open decisions |

## Agreed direction

- Use the name **Fuse-X** for native hosted Riplix. Keep the existing Ripple Riplix
  port useful alongside the hosted platform.
- Execute compiler-generated native code. Retain the current compatibility data
  layout initially, with explicit adapters for host facilities.
- Start headlessly. A network terminal may arrive before graphics if networking
  is ready first.
- Begin graphics with one window, while designing the interface to support
  independent terminal and GUI windows later.
- Let macOS manage native window placement, focus, and chrome. Riplix programs
  supply their window contents.
- Use one macOS application identity and Dock icon. Offer a window list like
  Finder's Dock menu; selecting an entry switches to that window. Separate
  macOS presenter applications are not part of this model.
- Keep the session running when terminal windows close. Provide a way to open
  another terminal from the native menus or Dock menu.
- Expose useful host controls, including networking, through the native UI.

## Proposals to resolve

Storage combines a Riplix root disk image with the gitignored repo-root
`fuse-x-shared/` folder mounted read/write at `/mnt/host` by default. Use `--share`
to select another folder or `--no-share` to disable it. The first filesystem bridge preserves
guest descriptors and supports native executables in the export. Additional
mount policies and broader metadata compatibility remain open. See [Storage](storage.md).

Native window switching, New Terminal, per-window close handling and GUI
terminal-detachment policy are implemented. The shared macOS text clipboard
now connects NetSurf and both graphical terminals through the native Edit menu;
see [clipboard bindings and limits](desktop.md#text-clipboard). Host settings
menus and the Dock-click reopen policy remain open.

A dark visual theme, consistent widgets, optional accelerated drawing, and a
standalone desktop with its own chrome or dock remain later design ideas. The
hosted window API should leave room for a standalone compositor backend.

## Implemented foundation

The native compiler/runtime already has resumable C continuations, cooperative
fork/yield/wait/exit, live-value preservation, copy-on-write private memory, and
an explicit shared kernel arena. Standalone examples cover recursive calls,
independent process memory, shared kernel state, and multiple live processes.

The [hosted kernel](../../fuse-x/README.md) now boots native `/init` and the existing
Bourne shell, with scripts, pipes, redirection, traps and terminal job control. Riplix owns scheduling and process lifetime; native continuations
provide fork, context switching and exec. Kernel globals are shared, while user
memory uses copy-on-write pages. The native loader runs independently compiled
application packages, alongside bootstrap programs selected through manifests.
The integration has 64 process slots and compiler-assisted scheduling checkpoints
in newly compiled applications. A persistent init supervisor reaps children and
keeps the kernel alive across shell exits; the launcher supports console
detach/reconnect and headless startup. Its
[application inventory](../../fuse-x/APPLICATIONS.md) builds the 113 ported
programs and aliases; memory budgets remain future work. File persistence and host timers work through real kernel
syscalls. The hosted network adapter reuses the guest IPv4 stack and provides
independent Telnet shells through Riplix PTYs. The first hosted GX backend now provides independent RGB565 Mac windows,
per-window input, native Window/Dock menus, New Terminal and orderly Quit.
Closing all windows preserves the session. Each native Nano-X top-level has its
own Mac window and private canvas, including correct child rendering and live
resize. The persistent server replaces the hosted NanoWM desktop. See [Desktop integration](desktop.md).

Use [native process documentation](../../rcc-native/PROCESSES.md) for current
APIs, limits, examples, and validation. Use [networking documentation](../networking/README.md)
for the existing device and protocol foundations. The
[Riplix port](../../ports/fuzix/README.md) remains the source of port-specific
build and runtime information.

## Maintaining this design

Update these pages when a decision changes or a milestone is validated. Mark a
feature implemented only when the Fuse-X integration itself works; success in
a standalone runtime fixture is evidence for its foundation. Keep detailed
compiler and device contracts in their existing documentation and link to them.
