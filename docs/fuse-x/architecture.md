# Fuse-X architecture

Fuse-X combines a native execution platform for FUZIX with a macOS application
that owns its windows and menus. A single session shares kernel state,
filesystem state, and devices across its guest processes.

## Native execution

RCC's native backend generates host machine code while retaining the current
Ripple compatibility layout: 16-bit C cells and integers, 32-bit logical
bank/offset pointers, and 64,000 cells per bank. Its private calling convention
passes a runtime context and scalar carriers. Host libraries require explicit
adapters; ordinary host C objects do not share this ABI.

Resumable native frames preserve return continuations and live values across
suspension. This provides the mechanism for cloning nested C execution without
copying host thread stacks or using macOS `fork()` as the guest process model.
See [native processes](../../rcc-native/PROCESSES.md) for the implemented contract.

The hosted platform replaces Ripple-specific context switching, traps,
syscall entry, and executable loading with native equivalents. Existing Ripple
machine-code executables cannot be entered as native code.

## Kernel and process ownership

Kernel state is shared across the session. User memory, user stacks, and
process-local allocator state belong to individual processes. Fork shares
private pages until writes detach them, while shared kernel allocations remain
visible to all processes.

Current SDK applications select a 192-bank heap at banks 48–239 before their
first allocator growth. Frames use 2–4, arguments bank 31, extended globals
32–47, and the shared kernel 240–255. Older packages keep the bounded legacy
heap at 8–30. Both windows stay addressable, but heap growth cannot cross the
argument/global gap. The new heap is private and copy-on-write across fork;
the reported capacity is not a physical memory reservation.

The hosted linker places globals from `--kernel` translation units in the
shared arena. Application globals remain private. Kernel-owned runtime file
handles also live in shared storage, so exec cannot invalidate the root disk
stream. Successful exec restores application global initializers and replaces
the active continuation stack.

FUZIX is the authority for scheduling and process lifetime. The runtime's hosted
API only clones, saves, selects, discards and replaces contexts indexed by FUZIX
process-table slot. It does not run its standalone scheduler in this mode.
The loader validates native packages in the guest filesystem and loads their
host code through an explicit runtime helper interface. Function tokens are
relocated per image, private global initializers are restored on exec, and code
is retained while any process still needs it. Bootstrap entries also support
prelinked manifests. See [native executables](../../fuse-x/EXECUTABLES.md).

Current native ownership separation is a trusted software model. User-copy
helpers validate bank windows, but compiled code can still directly address the
kernel arena. Physical memory budgets, stronger isolation, instruction-level
preemption and per-process runtime faults remain future work. Continuation-aware nonlocal
jumps and custom signal handlers support the existing Bourne shell and editors;
handlers run after completed syscalls and user-code checkpoints. Newly compiled
applications check loop edges and existing call continuations; a roughly 5 ms
slice hands control to a registered kernel callback. That callback advances
timers, polls input/networking, handles signals and invokes FUZIX's scheduler.
The return-to-caller boundary defers callbacks until the caller continuation
has copied aggregate return storage. Otherwise a callback could reuse the
released callee frame before that copy.
No Rust borrow or host call stack survives a switch. Kernel callbacks are not
asynchronously preempted, including handlers entered from them. Executable loading
has explicit safe scheduling points: after filesystem buffers are released,
streaming services timers/devices and yields at roughly 5 ms intervals. Two host
loader workers validate manifests, build relocation templates, write native
libraries and resolve exports without accessing guest memory. Polling installs at
most 128 function bindings before yielding again. Construction and staging belong
to the loading process; only the final exec commit replaces its address space.
Signals remain deferred during that resource-owning loader continuation. Other
long synchronous kernel/host operations still need their own safe yield points.
The hosted configuration has 64 process slots.

## Host application

```text
Fuse-X.app
  macOS main thread
    native windows, Dock menu, menu bar, presentation, host input
              | owned commands, events, and frame snapshots
  native loader pool (two workers)
    immutable executable preparation; no guest/kernel memory access
              | owned results, bounded function-table installation
  guest execution worker
    hosted FUZIX kernel and process continuations
    terminals, filesystems, networking, guest applications
              | explicit host adapters
    disk image, shared folders, Ethernet attachment, display services
```

Keep native UI work on the host event loop. Run guest execution independently
so handling a Mac window event does not require the guest to finish drawing or
return from a long operation. The existing native host already separates these
roles; it needs a registry of windows and session-level control messages.

Host background workers exchange owned buffers and queued events. They must not
retain a pointer into whichever process image happens to be active. Publish
stable frame snapshots; copy input or device results into the correct process
under the guest execution context.

## Window and console services

The conceptual window interface covers creation, title and size changes,
presentation, event delivery, and destruction. Handles are opaque guest-visible
identifiers, rather than host object addresses. Each window has an owning guest
process and its own event queue; a process may own several windows.

Terminal windows connect to separate terminal endpoints and shell sessions.
Window actions and menu commands request operations from the guest side.
Closing a terminal, creating a shell, and shutting down the kernel are distinct
operations.

The initial console uses a bounded host input queue and nonblocking polling.
FUZIX terminal reads suspend the calling process through the kernel scheduler.
A persistent host broker owns one console PTY and the native kernel. Console
clients can detach/reconnect without changing the guest terminal or stopping
its foreground program. PID 1 reaps children and restarts the console shell;
headless mode omits that shell and can explicitly start Telnet. Host SIGTERM
requests guest process cleanup and storage flush on the execution thread.
Graphical terminals now use separate guest PTYs and owned GX windows. The Mac
menu enqueues bounded New Terminal requests; PID 1 performs the guest fork/exec.
Quit requests guest shutdown through the existing SIGTERM path.

The application-facing surface API should also support a future backend that
composites windows into one RVM display. Native macOS window management and a
standalone desktop can then share guest applications and GUI libraries.

## Existing contracts

- [Native target and private ABI](../../rcc-native/README.md)
- [Continuations and process memory](../../rcc-native/PROCESSES.md)
- [Ripple MMU](../arch/mmu.md), for the separate VM translation model
- [Device interface](../arch/mmio.md)
- [Networking architecture](../networking/README.md)

## Shared graphics boundary

The [GX architecture](../graphics/architecture.md) specifies shared window,
surface and event operations for RVM and the native hosted backend. The
hosted `/dev/gfx` now implements this interface through `rcc_hosted_gx`, with
process ownership checked in FUZIX and bounded surface storage in the runtime.
The private adapter uses twelve 32-bit request fields, counts memory in C16
cells and copies borrowed buffers before returning. UI and guest threads share
owned snapshots/events, never guest pointers. The native build includes `gterm`,
the drawing engine, Nano-X server, launcher and clients. The native server
allocates a private canvas and host window per top-level, shares that canvas
with its child widgets, and routes host input only through that window tree.
macOS supplies window management; hosted sessions do not start NanoWM. Resize,
focus, title and close updates use the existing Nano-X client protocol.
`--gui` starts the persistent server before the terminal; `startx` can start it
on demand. Closing a client window does not stop the service or kernel. See [Desktop integration](desktop.md).
