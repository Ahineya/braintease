# Native kernel and continuation performance

Fuse-X uses the same runnable/event queues as Riplix/RVM, with
`CONFIG_RIPPLE_SCHED`. Host monotonic samples supply a native 5 ms slice,
scaled by priority and bounded to 5–200 ms. Wakeups can request rescheduling
after a minimum 5 ms slice. Native checkpoints still poll devices and deliver
signals approximately every 5 ms when execution reaches a checkpoint; an
ordinary checkpoint no longer forces a process switch. Blocking calls yield
immediately. Loader yields remain explicit, outside pinned filesystem buffers.
Logical IRQ masking and nonpreemptive kernel execution are unchanged.
The shared RVM default remains 20 ms; Fuse-X sets `RIPPLE_SCHED_QUANTUM_MS=5`
to preserve its previous responsiveness under CPU contention.

Process exit closes descriptors and releases resources without a global sync.
Explicit sync, synchronous I/O and orderly shutdown retain their barriers.
The duplicate outer syscall-exit clock sample is removed. See the
[shared scheduler contract](../ports/fuzix/SCHEDULER.md) for queue invariants.

## Native runtime

Hosted parked contexts are indexed by kernel slot. Selecting an existing context
does not search a second runnable queue, and dropping a context releases its
private memory, continuations and image ownership before that slot is reused.
The standalone cooperative-process API retains its own scheduling policy.

Private memory retains 512-cell COW pages. A fork shares one page-table root;
it no longer increments 30,000 individual page-handle reference counts. A write
copies a shared root directory, its affected 128-entry leaf and the modified
page as necessary. Unchanged writes detach none of these levels. Shared kernel
memory remains separately synchronized and shared across every context.
Wide scalar kernel loads/stores acquire its existing mutex once per scalar
instead of once per C16 cell. MMIO and display accesses keep their own paths.

Compiler-generated continuation slots are numbered densely at each suspension,
including live call results; immutable parameters come from frame arguments.
The runtime keeps a reusable dense slot vector, with a sparse overflow map for
large identifiers used by older packages. Clears retain dense capacity and
discard old values. Fork/setjmp clone the live slot state. Existing helper
names and the executable ABI are unchanged: previously built FXE packages run
without recompilation; recompilation obtains the new slot numbering and calls.

## Bounded direct calls

The compiler finds internal helpers whose CFG and transitive call tree are
acyclic and contain at most 256 IR instructions. They cannot call runtime
services, unknown/indirect targets or resumable functions. Loops and recursion
retain checkpoints and continuations. Pointer/aggregate returns retain their
existing return-copy protocol. This deliberately conservative analysis operates
on optimized shared IR, benefiting both C and RFL inputs.

Qualified helpers get an ordinary native implementation for direct calls.
Their existing public function tokens still point to resumable entry points,
so callbacks, function-pointer equality, dynamic images and argument arities
keep their interface. Logical stack bookkeeping is omitted from ordinary
implementations that need neither local storage nor variadic setup. A direct
call cannot switch processes while host return addresses are live; any call
chain that might suspend remains in the continuation machinery.

This is not a general native-pointer ABI, asynchronous host preemption or a
replacement for blocking host-service adapters. Persistent resident-memory
accounting and broader suspension analysis remain separate work.

## Measurement

From the repository root, with the kernel/bootstrap programs built:

```sh
python3 fuse-x/bench_kernel.py --output build/native-bench
python3 fuse-x/bench_kernel.py --executable SAVED_KERNEL --probe build/native-bench/kernelbench.fxe --output build/native-comparison
python3 fuse-x/bench_kernel.py --source fuse-x/examples/scheduler-bench.c --output build/native-contention
```

The benchmark uses a disposable root, captures raw samples and hashes, and
checks the filesystem after orderly shutdown. It measures cheap syscalls,
clocks, fork/exit/wait, self pipes and blocking round trips. The contention probe
checks timed wakeup and syscalls with two pure-computation children, then signals
and reaps them. Reuse identical FXE bytes to isolate kernel/runtime changes;
recompile separately to measure code-generation changes. Keep repetition counts
equal and report timing variation. Host scheduling and millisecond guest-clock
resolution limit short-operation measurements.

Regression coverage includes runtime COW root/leaf sharing and page boundaries,
legacy/dense slots, high-slot context drop/reuse, protected aggregate returns,
bounded-call analysis, direct/indirect/variadic calls, fork and nonlocal jumps.
The guest suites exercise all 64 process slots, readiness, signals, PTYs,
networking, deadlines, shutdown and file persistence using private disks.

## Recorded comparison (October 19, 2026)

Five repetitions ran with the same application package on the saved original
kernel and the updated kernel. The package SHA-256 starts `ada39cf1092c`;
the final kernel SHA-256 starts `e1f74ebc339f`. Median times per operation:

| Operation | Before | Updated kernel, same application | Recompiled application |
| --- | ---: | ---: | ---: |
| `getpid` | 19 µs | 8 µs | 8 µs |
| monotonic clock query | 22 µs | 11 µs | 11 µs |
| self pipe, one cell | 125 µs | 40 µs | 40 µs |
| self pipe, 512 cells | 135 µs | 50 µs | 45 µs |
| fork + child exit + wait | 700 µs | 100 µs | 100 µs |
| blocking pipe round trip, one cell | 325 µs | 145 µs | 140 µs |
| blocking pipe round trip, 512 cells | 380 µs | 155 µs | 160 µs |

The fork batch contains ten operations and the clock has millisecond resolution,
so its per-operation quantization is 100 µs. Original fork samples ranged from
500–800 µs, versus 100–200 µs after the change. Original `getpid` batches ranged
from 17–22 µs per call, versus 8–10 µs with the same application after the change.
These are local observations, not general speedup or hard-latency guarantees.
Kernel recompilation also obtains direct-helper code generation, so benefits to
an old application include compiler improvements inside the kernel itself.

With two CPU-bound children, median `getpid` time fell from 74 to 32 µs, while
a requested 60 ms sleep returned after 65 ms in both comparisons. An intermediate
20 ms native slice returned after 80 ms in this workload; the final 5 ms native
configuration preserves responsiveness. Those contention comparisons also reuse
identical application bytes. Pure computation remains schedulable and signalable.

Raw measurements, hashes, transcripts, the saved kernels and the intermediate
20 ms measurements are retained under `build/fuse-x-performance` in this
checkout. `before`, `final-old-application`, `final-new-application`,
`contention-before` and `contention-after` contain the final comparisons.

Validation passed: 7 native backend and 38 native runtime unit tests; 27 native
compile/link/execute tests; the new rct case on native and RVM; both scheduler
quantum fixtures; and actual-kernel process capacity, shell/jobs/PTYs, local and
internet readiness, deadlines/pause recovery, heap edges, executable lifetime,
malformed-image rollback and persistence suites. The rebuilt RFL tools also
passed the native comprehension example and Commander shell/terminal integration.

## Kernel I/O (October 19, 2026)

The shared kernel I/O changes are documented in
[KERNEL-IO.md](../ports/fuzix/KERNEL-IO.md). They include supervisor-memory
anonymous pipes, buffered directory enumeration, bounded name/inode/buffer
indexes, larger supervisor block caches, device-local inode refill, grouped
sync barriers, raw PTY queue transfers, and targeted host-folder metadata and
durability operations. Canonical terminal processing and the disk format remain
unchanged. Dirty shared files retain bounded capabilities across close/unlink
until an explicit or batch barrier.

The actual native/RVM kernel probes cover read-only-root pipes, concurrent
atomic writers, signals/select, symlinks, large terminal requests, directory
mutation/reuse/seeking, and reboot/fsck. Standalone tests inject queue-copy,
allocation and device-write/flush failures. See the shared document for commands.

Three repetitions per kernel used identical application bytes on disposable
roots, with no concurrent test/build jobs from this task. The saved original
kernel SHA-256 starts `982d94c5b4e5`; the final kernel starts `71fb9613198e`.
The I/O package starts `430837b05257`; the separate lifecycle/control package
starts `34d4cd793bf9`. Median time per operation:

| Operation | Before | Final kernel |
| --- | ---: | ---: |
| Create/write/close one file | 337.5 µs | 300.0 µs |
| Enumerate one directory entry | 12.3 µs | 6.8 µs |
| Repeated pathname lookup | 77.0 µs | 34.0 µs |
| 512-character PTY round trip | 1710.0 µs | 405.0 µs |
| Three-character shared-file write | 60.0 µs | 20.0 µs |
| Pipe creation, one-character transfer and close | 85.0 µs | 75.0 µs |

Sample ranges (before versus after):

- Create/write/close one file: 312.5–343.8 versus 281.2–312.5 µs.
- Enumerate one directory entry: 12.3–13.0 versus 6.2–6.8 µs.
- Repeated pathname lookup: 76.0–79.0 versus 30.0–139.0 µs.
- 512-character PTY round trip: 1695.0–1715.0 versus 390.0–415.0 µs.
- Three-character shared-file write: 60.0–60.0 versus 20.0–20.0 µs.
- Pipe creation, one-character transfer and close: 80.0–125.0 versus 75.0–80.0 µs.

The lifecycle probe's cheap getpid control moved from 9 to 8 µs. These are
whole-kernel observations, including code layout and host CPU/scheduling
variation, not isolated attribution to each algorithm or universal speedups.
The millisecond guest clock quantizes a 100-operation batch in 10 µs steps.
Earlier development runs were faster on several operations; the table above
uses the final kernel, including retained pipe activity timestamps. Warm
pipe transfers and file creation retain syscall and timestamp-accounting costs.
The stronger pipe property is that no disk allocation is required, including
on a read-only root.

Raw samples and transcripts are under `build/kernel-io-performance/io-before`,
`io-after`, `lifetime-before` and `lifetime-after`. `before/fuse-x` retains the
comparison executable. To reproduce with saved application bytes:

```sh
python3 ports/fuzix/tools/test_kernel_io.py --benchmark-only --probe build/kernel-io-performance/io-before/iocheck.fxe --executable build/kernel-io-performance/before/fuse-x --repetitions 3 --output build/io-before
python3 ports/fuzix/tools/test_kernel_io.py --benchmark-only --probe build/kernel-io-performance/io-before/iocheck.fxe --repetitions 3 --output build/io-after
```

Validation passed: production C helper tests with UBSan; all 42 native-runtime
unit tests; all 19 host disk tests; full native/RVM I/O targets, including
select/signals/terminal modes, symlink limits, read-only-root pipe timestamps,
large terminal requests and reboot/fsck; live Commander move/rename on both
kernels; 64-process exhaustion/high-slot timers/recovery on both kernels;
Bourne shell/jobs/editor, local IPC and host-folder integration on Fuse-X; and
RVM shutdown/reboot with a live pipeline and no orphan disk inode.

## Native terminal rendering

The macOS `gterm` path sends changed text rows through the owned GX surface
instead of rasterizing each glyph into guest RGB565 pixels. CoreText masks are
cached per glyph; painting and presentation retain two-times-resolution RGBA.
The renderer uses the default iTerm profile's font/palette. Other GX clients
retain RGB565 semantics, including when an owner switches between text and
pixel operations on one surface.

A warm 160×55 frame using MesloLGS NF 13 measured about 6.8 ms in the release
renderer test. Disposable-guest resizes measured 46–93 ms across runs for 100×30 through
180×60 cells, including RGB screenshot generation and observation. These are
fixture measurements, not a guarantee for every application or host load.
The mux sparse-layout check sends a 240×60 screen in 2,238 ANSI octets across
two bounded rounds and verifies every decoded character/style against the
source grid. Blank-run compression and synchronized output avoid a visible
row-by-row full redraw.

Reproduce without touching an existing guest disk:

```sh
cargo test --release -p rcc-native-runtime hosted_terminal::tests -- --nocapture
python3 fuse-x/test_terminal_native.py
python3 ports/fuzix/tools/test_mux_render.py
```
