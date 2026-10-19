# Native process continuations and scheduling checkpoints

This compiler/runtime foundation is independent of the Riplix port. Standalone
C fixtures exercise the process model without building or changing Riplix.
The separate [Fuse-X integration](../fuse-x/README.md) uses the hosted context
API below to run Riplix with its own scheduler and syscall interface.

From the repository root:

```sh
cargo build --release -p rcc-driver -p rcc-native-runtime
target/release/rcc compile rcc-native/examples/processes.c \
  --target native -I runtime/include -o build/native-processes
RCC_NATIVE_HEADLESS=1 build/native-processes
target/release/rcc compile rcc-native/examples/kernel-state.c \
  --target native -I runtime/include -o build/native-kernel-state
RCC_NATIVE_HEADLESS=1 build/native-kernel-state
```

`processes.c` forks inside three nested calls and verifies independent locals,
globals, heap allocations and allocator state. `kernel-state.c` shows shared
kernel state alongside private process globals.

## Continuations

Calling a service in `<native_process.h>` opts the whole linked native program
into resumable code generation. Compile multiple translation units with
`--target native --emit-ir`, then combine them with `rcc link-native` as usual.
Programs without process-service calls retain flat RAM and ordinary native
calls. Process programs still execute compiled host arithmetic, branches and
loops; there is no Ripple instruction decoder, host fork, stack copying, or
replay of earlier instructions.

Each suspendable call saves a continuation number and the scalar values needed
after resumption. Liveness follows control-flow edges, including the selected
inputs of phi nodes and loop back edges. Dead temporaries and call-only
arguments are discarded. The new call result is supplied by the callee, and
immutable parameters are recovered from the frame's original arguments.
Resumption reloads only the values needed at that continuation, rather than
every temporary in the function. Slots from an earlier suspension are cleared.

Recursive calls, internal function pointers, variadic arguments, floating
values, and pointers into outer C frames survive suspension. Taking a runtime
binding's address is rejected in this mode; direct runtime calls work as before.

## Private and kernel memory

Process-enabled programs divide compatibility memory into:

| Region | Ownership |
| --- | --- |
| Banks 0–239 | Private process RAM; MMIO remains a shared host service |
| Banks 240–255 | Explicit shared kernel arena |

Small programs retain global bank 1. Larger programs use banks 32–47 for
private globals, reserved from allocation. The stack region and heap start
remain unchanged.
Private `malloc` is bounded below bank 240. Ordinary native programs continue
to use all 256 banks for their existing layout.

Private RAM uses **512-cell (1 KiB) copy-on-write pages**. Fork copies the page
table and continuation/allocator metadata, retaining references to the same
pages. A private write detaches the affected page. Writes that leave the cells
unchanged keep sharing. Initially zero pages share one backing page. The table
contains 30,000 page references (240,000 bytes on a 64-bit host); a fork no
longer copies the complete 32,768,000-byte compatibility RAM image. Process
switches move the address-space object without copying its pages or table.

All writes use the same memory abstraction: scalar stores, wait results,
`memset`, overlapping `memmove`, string copies, reallocations, file reads,
argument setup, and device/DMA output. Device commands use the existing
`GuestMemory` protocol directly; no whole-RAM staging buffer is required.

Allocate long-lived shared kernel objects with:

```c
void *rcc_process_kernel_alloc(size_t cells);
void rcc_process_kernel_free(void *ptr);
```

Sizes are compatibility cells, as for native `malloc`. The arena spans
1,024,000 cells; its initial zero-offset cell is not allocated. Both data and
allocator bookkeeping are shared across process images, so allocation/free
performed by a child remains visible to its parent. Allocation returns null
on exhaustion. Release these objects with `kernel_free`, not private `free`.
Ordinary C globals remain private; kernel code must explicitly place shared
objects in this arena. Native code and the function-token registry are shared.

This separates memory ownership for a future hosted platform. It is a trusted
software model: process code can still address the kernel arena, and no
privilege checks or kernel/user execution-mode transitions are implemented.

## Scheduling and limits

- `rcc_process_fork()` returns a positive child PID to the parent, zero to the
  child, and `-1` at the process limit or after exhausting positive 16-bit PIDs.
- `rcc_process_getpid()` returns the current PID; the initial process is PID 1.
- `rcc_process_yield()` schedules explicitly. Compiler-generated loop checkpoints
  and call boundaries also rotate ready processes at roughly 5 ms intervals.
- `rcc_process_wait(pid, &status)` waits for a direct child; `-1` selects any
  child. It returns the reaped PID, or `-1` if none matches. A null status pointer
  is allowed. Status is the raw 16-bit exit code, without POSIX wait encoding.
- `rcc_process_exit(status)`, external `exit(status)`, or returning from `main`
  ends the current process. The host returns the initial process's status once
  all remaining processes finish. Orphans finish without leaving zombies;
  they are not adopted by an init process.

The default capacity is **256 processes**, including unreaped zombies.
`RCC_NATIVE_MAX_PROCESSES=64` sets a different capacity; valid limits are
1–32767. Reaping releases capacity. PIDs are not reused within one run.

Blocking host calls such as `getchar` still block the entire driver. Console
output, host streams and devices are shared services, without per-process
Riplix descriptor or terminal semantics. Runtime faults still end the host
program. Standalone processes do not provide guest signals or Riplix descriptors.
The hosted integration below supplies its own signals, executable loading and
syscall adapters. A hosted console can accept network or graphical
terminal backends independently.

## Validation and measurement

Build the release runtime before running executable tests:

```sh
cargo test -p rcc-native -p rcc-native-runtime
cargo test -p rcc-driver --test native --test native_process
```

Tests check actual page sharing/detachment, shared allocator ownership and
exhaustion, overlapping copies across pages and banks, kernel boundaries,
device writes, file reads, live values and loops, 4,096 nested calls, indirect
and variadic calls, separately linked C units, blocking/completed-child waits,
and configurable process limits. The many-process fixture holds 63 children
alive until their parent opens a gate in shared kernel memory.

`examples/process-bench.c` performs 100 fork/wait pairs through 17 nested C
calls, without console, graphics, file, or network work:

```sh
target/release/rcc compile rcc-native/examples/process-bench.c \
  --target native -I runtime/include -o build/native-process-bench
RCC_NATIVE_HEADLESS=1 RCC_NATIVE_NET=off build/native-process-bench
```

Time the executable separately from compilation. This workload measures both
continuation and fork costs; it is not a prediction of hosted Riplix performance.

On the development ARM64 Mac, seven alternating runs after one warm-up per
executable gave median times of **306 ms before** and **55 ms after** these
changes (about **5.6× faster**). The baseline executable was built from the
original eager-copy/all-temporary prototype before changing the backend. Both
executables ran the same benchmark source with graphics/networking disabled;
compilation was excluded and process startup included. Host load was not
controlled, so these are illustrative timings rather than a stable benchmark.

## Hosted kernel contexts

`<native_hosted.h>` provides a separate mode for a kernel that owns process
scheduling. Link shared kernel IR units with repeatable `--kernel` arguments:

```sh
rcc link-native --kernel kernel.ir application.ir -o hosted
```

Only definitions in kernel units become shared globals (bank 240 onwards).
Application units retain private globals in bank 1, or banks 32–47 for larger
images. The linker reserves the
shared global region before allocating other kernel objects. Either process API
opts the linked program into resumable code generation.

- `rcc_hosted_clone(slot)` saves a child context at the requested slot. It returns
  1 to the parent, 0 to the child when resumed, and -1 if unavailable or at capacity.
- `rcc_hosted_switch(slot)` parks the caller and resumes the selected context.
  It returns when the kernel later selects the caller again.
- `rcc_hosted_drop_switch(slot)` discards the caller and resumes another context.
- `rcc_hosted_exec(entry, argument)` discards the current frames, restores private
  global initializers, resets continuation stacks and the private allocator, and
  starts `entry(argument)`. It does not return. The kernel prepares arguments in
  a separate private region before calling it.
- `rcc_hosted_bind_checkpoint(callback)` registers the kernel's scheduling and
  signal-delivery callback. Newly compiled application loops and existing call
  continuations provide checkpoints with a roughly 5 ms time slice. Hosted
  contexts are never rotated by the standalone scheduler. No checkpoint enters
  another kernel callback while a syscall or checkpoint callback is active.
- `rcc_hosted_stopping()` polls an orderly-shutdown request from host SIGTERM.
  The signal handler only sets an atomic flag; guest work stays on its thread.
- `rcc_hosted_lock_image(path, writable)` holds an advisory image lock for the
  runtime lifetime. `rcc_hosted_sync_image()` flushes that image to host storage.
- `rcc_hosted_input()` polls a host byte queue, returning -1 when empty and -2 at
  end of input. `rcc_hosted_idle(milliseconds)` yields host CPU time, capped at
  10 milliseconds per call.

Slot zero is the initial context. Other slot numbers must be below
`RCC_NATIVE_MAX_PROCESSES` (default 256). Slots are storage identities; guest
PIDs, wait status, zombies and readiness belong to the hosted kernel. Saved
contexts are never automatically scheduled in this mode. Runtime `exit` or
return from the root entry ends the entire session; guest exit must enter the
kernel first. The standalone fork/yield/wait services cannot be mixed with
hosted scheduling. Shared arena allocate/free services remain available.

The hosted runtime keeps host file stream handles shared across exec. The kernel
must manage their lifecycle. Resetting the private allocator does not zero every
private RAM bank; the platform loader is responsible for its user image and
argument layout. The hosted image loader adds `rcc_hosted_image_load` to stage
an executable and `rcc_hosted_image_discard` to release a failed exec attempt.
Staging belongs to the calling process and is released on replacement or exit.
Loaded code remains owned by each process that uses it, including forked children.
`rcc_hosted_bind_syscall` registers a kernel continuation that services application
requests made through `rcc_hosted_syscall`. See [Fuse-X executables](../fuse-x/EXECUTABLES.md)
for the package, application compiler and validation. Privilege isolation,
instruction-level preemption and physical memory quotas remain separate work.

## Nonlocal jumps in resumable programs

Native continuation mode implements `setjmp` and `longjmp` as suspension
operations, without capturing host return addresses. A jump buffer stores an
opaque token for the caller's saved program counter, live SSA slots, frame
identity, stack watermark and result slot. Its storage must have at least four
16-bit cells; the FUZIX `jmp_buf[10]` layout has sufficient space.

`setjmp` returns zero initially. `longjmp` unwinds younger continuations and
resumes that call with the requested compatibility int (zero becomes one).
Memory is not restored: globals, heap and volatile automatic objects retain
changes. As in C, changed non-volatile automatic values need not survive.
Multiple buffers and repeated jumps work. Fork copies the saved jump contexts
along with process-private memory. Returning from an activation retires its
buffers; exec discards every saved jump. Expired or foreign tokens fail with a
runtime diagnostic instead of entering a stale host address.

The hosted Riplix adapter validates handler tokens against the current image and
calls handlers after completed syscalls and at user-code checkpoints. Handlers
can make nested kernel calls, fork, exec or leave through `longjmp`. This does
not add asynchronous host-stack unwinding or instruction-level preemption.
