# Cooperative native processes

This compiler/runtime foundation is independent of the FUZIX port. Standalone
C fixtures exercise the process model without building or changing FUZIX.
It does not yet boot a hosted kernel or implement its syscall/exec interface.

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

The existing private global bank, stack region and heap start remain unchanged.
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
- `rcc_process_yield()` schedules cooperatively.
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
FUZIX descriptor or terminal semantics. Runtime faults still end the host
program. Signals, timer preemption, executable loading and syscall adapters
remain separate work. A hosted console can accept network or graphical
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
continuation and fork costs; it is not a prediction of hosted FUZIX performance.

On the development ARM64 Mac, seven alternating runs after one warm-up per
executable gave median times of **306 ms before** and **55 ms after** these
changes (about **5.6× faster**). The baseline executable was built from the
original eager-copy/all-temporary prototype before changing the backend. Both
executables ran the same benchmark source with graphics/networking disabled;
compilation was excluded and process startup included. Host load was not
controlled, so these are illustrative timings rather than a stable benchmark.
