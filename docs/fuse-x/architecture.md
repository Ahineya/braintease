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

The hosted platform must replace Ripple-specific context switching, traps,
syscall entry, and executable loading with native equivalents. Existing Ripple
machine-code executables cannot be entered as native code.

## Kernel and process ownership

Kernel state is shared across the session. User memory, user stacks, and
process-local allocator state belong to individual processes. Fork shares
private pages until writes detach them, while shared kernel allocations remain
visible to all processes.

The current runtime exposes an explicit shared kernel arena. Ordinary C globals
are still private in process-enabled programs. The hosted port must deliberately
place kernel globals and kernel-owned objects in shared storage; recompiling
the existing kernel does not perform that migration automatically.

The standalone runtime scheduler demonstrates continuation and process
mechanics. Fuse-X must reconcile that mechanism with FUZIX's process table and
scheduler, with one authority for scheduling and process lifetime. Avoid two
independent schedulers disagreeing about the same process.

Current native ownership separation is a trusted software model. Privilege
checks, syscall execution modes, preemption, signals, and per-process fault
handling need separate design and validation.

## Host application

```text
Fuse-X.app
  macOS main thread
    native windows, Dock menu, menu bar, presentation, host input
              | owned commands, events, and frame snapshots
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

Receive/wait operations must suspend only the calling process. The current
blocking host `getchar` implementation is insufficient for a responsive
multi-terminal Fuse-X session.

The application-facing surface API should also support a future backend that
composites windows into one RVM display. Native macOS window management and a
standalone desktop can then share guest applications and GUI libraries.

## Existing contracts

- [Native target and private ABI](../../rcc-native/README.md)
- [Continuations and process memory](../../rcc-native/PROCESSES.md)
- [Ripple MMU](../arch/mmu.md), for the separate VM translation model
- [Device interface](../arch/mmio.md)
- [Networking architecture](../networking/README.md)
