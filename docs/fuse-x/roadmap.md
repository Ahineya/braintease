# Fuse-X implementation roadmap

Build Fuse-X in independently verifiable stages. Keep native compiler/runtime
work separate from FUZIX platform integration when another change owns the
port. These pages record the intended integration without changing port code.

## Available native foundation

The standalone native process milestone is implemented: resumable C calls,
fork/yield/wait/exit, liveness-based continuation storage, copy-on-write private
pages, and an explicit shared kernel arena. The current runtime defaults to
256 process slots and supports a configurable limit; this is a prototype limit,
not a promised FUZIX kernel capacity.

Validation includes private stack/global/heap state, shared kernel allocator
state, recursive calls, indirect and variadic calls, device/file write paths,
and 63 live children coordinated through shared memory. Current commands and
limitations live in [native processes](../../rcc-native/PROCESSES.md).

## Hosted kernel and storage

Create the hosted platform adapters alongside the existing Ripple target. Resolve
context switching, syscall entry, shared kernel globals, user-memory access,
process ownership, and the first native application-loading strategy. Wire the
disk-image adapter and a nonblocking console.

Acceptance: boot the hosted kernel into a headless guest shell, run a guest
program, exercise fork/wait/exit through the integrated kernel, and verify file
persistence after orderly shutdown and restart. Native IR generation for some
kernel files alone does not satisfy this stage.

## Headless network terminal

Connect a network terminal to the guest console/session machinery once the
required networking is ready. This stage can precede graphics and should not
block graphics work if networking takes longer.

Acceptance: connect, use the shell, disconnect without stopping the kernel,
and reconnect. Unrelated sessions continue to run. See
[Networking](networking.md).

## First graphics window

Introduce the surface/window interface and one native graphics window. Use
bitmap presentation first. Establish handles, input events, resize/focus/close
events, and a window lifecycle separate from session shutdown.

Acceptance: a guest program draws and receives input through the new interface.
Its window can close without treating every close request as a host-wide exit.

## Multiple windows and desktop controls

Support separate terminal and GUI windows inside one Fuse-X application. Add
the Window menu and a Finder-style Dock window list using the same registry.
Provide New Terminal in the menu bar and Dock menu.

Acceptance:

- A shell launches a GUI program into its own window.
- A GUI program can launch another GUI program.
- Input, focus, and resizing affect the intended window.
- Selecting a Dock or Window-menu entry activates the correct window, including
  a minimized window.
- All terminal windows can close while independent GUI work and the kernel
  continue running.
- New Terminal creates another usable shell afterwards, including when no
  windows remain.
- Quit Fuse-X performs orderly guest shutdown and storage flush.

Use one Dock icon throughout. Separate macOS presenter applications are not a
dependency or a later requirement for window switching.

## Shared folders and later graphics

Implement the selected host-filesystem bridge with explicit metadata and mount
rules. Acceptance includes reading files from both systems, agreed write and
rename behavior, bounded export paths, and correct handling of host-side edits
or a removed mount. See [Storage](storage.md).

Later graphics work may add accelerated blits, richer widgets, clipboard and
high-DPI integration, and a standalone compositor backend. Preserve the guest
surface interface across host and standalone presentation.

## Decisions still open

| Question | Needed before |
| --- | --- |
| How FUZIX scheduling uses the native continuation engine | Integrated process execution |
| Shared kernel globals and kernel/user execution boundaries | Hosted kernel integration |
| Native executable format, linking, and loading | Running independently built native guest applications |
| Terminal hangup, GUI launch/detachment, and initial shell policy | Multiple independent sessions |
| Window command/event formats and buffer ownership | First graphics window |
| Reopen behavior and final menu/shortcut layout | Desktop lifecycle completion |
| Image format compatibility and host filesystem bridge semantics | Persistent storage and shared-folder mounts |
| Network setting persistence and live attachment changes | Native Network controls |

Record decisions here and in the relevant topic page as they are settled.
Mark stages complete only after their integrated acceptance behavior works.
