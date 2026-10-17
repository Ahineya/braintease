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

The first integrated [hosted kernel](../../fuse-x/README.md) boots native
`/init`, starts the existing Bourne shell and runs independently compiled native applications.
FUZIX owns process scheduling and lifetime; hosted runtime calls clone, select,
discard and replace saved native continuations. Kernel globals use the shared
arena and private application memory uses copy-on-write pages.

The console polls host input without blocking guest execution. A raw C16 image
provides persistent storage. Native executable packages load after kernel path and permission checks;
bootstrap manifests also select registered functions. The first independently
compiled utilities are cat, ls and cp. See [native executables](../../fuse-x/EXECUTABLES.md)
for the ABI and loader. Ripple RXE files require recompilation for this target.

Acceptance checks cover shell commands, arguments, fork/wait/exit, private memory,
shared file offsets, concurrent children, timers, failed exec rollback, successful
exec, process-slot reuse, input backpressure, EOF and persistence after restart.
`make -C fuse-x test` runs the bootstrap and integrated session checks.

The application inventory builds 113 programs and aliases. Native nonlocal
jumps, custom signal handlers, standard devices and PTYs support the real
shell. Application behavior checks distinguish compilation from runtime
coverage. Follow-up work remains for further application validation, complete
libc, instruction-level preemption and enforced memory budgets. Current bank-window
validation and RAM accounting are provisional; the Ripple port's variable
executable allocation is not yet implemented in the hosted platform.

## Headless network terminal

The hosted integration reuses the Ripple port's IPv4/TCP/UDP stack, resolver
tools and Telnet server. Protocol state uses hosted shared kernel allocation;
polling runs at idle and syscall boundaries. Remote connections use the existing
FUZIX PTYs and independently loaded Bourne shells. The launcher exposes host-only,
shared NAT, offline and external Ethernet attachment. Graphics work can proceed
independently without changes to `ports/fuzix/` from this integration.

Acceptance: connect, use the shell, disconnect without stopping the kernel,
and reconnect. Unrelated sessions continue to run. See
[Networking](networking.md).

## Session lifecycle and scheduling fairness

The init supervisor keeps the kernel alive independently of its console shell,
reaps adopted children and starts a new console shell after exit. The launcher
supports console detach/reconnect, background startup, headless startup and an
explicit Telnet service. Native SIGTERM and `--stop` request orderly process
termination and storage flush. A one-shot mode retains finite console/EOF runs.

New application packages include loop-edge scheduling checkpoints, while calls
reuse existing continuation boundaries. A roughly 5 ms slice enters a kernel
callback for timers, input, networking, signal delivery and ready-task selection.
Kernel callbacks and their signal handlers remain nonpreemptible. Existing
packages need recompilation for loop checkpoints; memory budgets and per-process
fault containment remain open.

Acceptance checks cover console replay/reconnection and terminal restoration,
shell respawn, orphan reaping, busy-loop Ctrl-C and stop/continue/termination,
signal-handler longjmp, headless Telnet, image locking and clean shutdown.

## First graphics window — implemented

The hosted GX backend, native graphical terminal and Nano-X drawing/desktop
builds are implemented. Runtime and real-kernel checks cover ownership, drawing,
fork/exit/exec cleanup, PTYs, clients and session survival. Native Mac UI checks
cover commands, New Terminal, closing all windows and reopening, and orderly Quit.

Introduce the surface/window interface and one native graphics window. Use
bitmap presentation first. Establish handles, input events, resize/focus/close
events, and a window lifecycle separate from session shutdown.

Acceptance: a guest program draws and receives input through the new interface.
Its window can close without treating every close request as a host-wide exit.

## Multiple windows and desktop controls — implemented

Independent GX windows, the application bundle, Window/Dock lists, Command-N,
last-window survival and orderly Quit are implemented. Each Nano-X top-level
has a private native surface, correctly clipped children, independent input,
resize, title and close handling. GUI clients release their launching terminal
and survive its closure. Host settings menus remain open.

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

The first host-filesystem bridge exposes the gitignored repo-root
`fuse-x-shared/` folder read/write at `/mnt/host` by default; `--share` overrides
it and `--no-share` disables it. It preserves the root disk and guest
descriptor ownership, and supports independently compiled executables in the
export. Acceptance includes reading files from both systems, agreed write and
rename behavior, bounded export paths, and correct handling of host-side edits
or a removed mount. See [Storage](storage.md).

Later graphics work may add accelerated blits, richer widgets, clipboard and
high-DPI integration, and a standalone compositor backend. Preserve the guest
surface interface across host and standalone presentation.

## Decisions still open

| Question | Needed before |
| --- | --- |
| Enforced user/kernel memory boundaries and process budgets | Untrusted applications |
| Further libc and application-specific compatibility | Remaining application modes and options |
| Reopen behavior and final menu/shortcut layout | Desktop lifecycle completion |
| Image format compatibility and host filesystem bridge semantics | Persistent storage and shared-folder mounts |
| Network setting persistence and live attachment changes | Native Network controls |

Record decisions here and in the relevant topic page as they are settled.
Mark stages complete only after their integrated acceptance behavior works.
