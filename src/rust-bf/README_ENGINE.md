# Native execution core

`bf_wasm::engine` provides the reusable 16-bit Brainfuck core. `Program::parse`
validates brackets and optionally enables arithmetic blocks, affine loops with
multiple destinations, cached scans, moving-marker searches, moving update
loops, and guarded loops containing arithmetic and scans. It parses once;
machines hold independent tape and execution state.
The interpreter uses safe Rust and wraps cells modulo 65536. Both the native
`bf` CLI and Ripple's `rbf` host use it. Optional native JIT execution isolates
its unsafe function-pointer call and generated memory accesses in `engine/jit`.

`Machine::new(cells)` uses strict tape bounds. `with_wrapping_tape(cells)` supports
the traditional command-line pointer behavior. Optimizations check the complete
pointer range, including intermediate excursions. When a range crosses an edge,
the retained original instructions preserve wrapping aliases or the partial state
before a bounds error. Loops with zero initial control cells skip their bodies.

Zero-seeking loops such as `[>>>>>>>>]` learn runs of nonzero cells. Later scans
can cross a known run in constant time, in either direction. The cache stores up
to 32 residue classes for the most recently used stride, independently of tape
size. A BF write that creates a zero trims the affected run; taking `tape_mut()`
discards all cached facts. The exit cell is always checked again. Cached skips
also check intermediate pointer excursions, including bodies such as `[>><]`.
Hosts that change individual cells can use `write_cell(index, value)` to preserve
unrelated cached facts while invalidating every newly zero cell correctly.

The nested loop `[>[<-]<[->+<]>]` is a different search: it moves a temporary one
through empty cells until it encounters a nonzero cell. The engine recognizes
the same structure with any fixed stride or direction. It folds the loop only
when the marker is one, the cell behind it is zero, and the complete pointer
range fits. It preserves the found value and leaves the loop's exact final tape
and pointer state. Other states use the original BF instructions.
Intermediate markers cross only verified zero cells and cancel before any event,
so the engine writes only the original cell and the final marker. Budget yields
and bounds fallbacks still materialize the exact literal marker state.
An independent zero-run cache lets later searches cross verified zero cells in
constant time. It also has 32 residue slots for its current stride, checks the
complete body range before skipping, and rechecks the found cell. Every
zero-to-nonzero write trims this cache; `tape_mut()` resets both caches.

Moving update loops such as `[-<<<<<<<<]` execute several complete iterations
per dispatch. Each iteration still applies its writes before testing the next
cell, so updates that create or remove a future stopping point retain their BF
meaning. Both iteration count and total cell updates are bounded. This speeds up
generic marker cleanup.
When a moving loop updates only its own predicate, cache maintenance is batched
over the changed progression. Completely cleared runs also become verified
zero-run facts. A previously verified nonzero run allows a batch to replace
per-cell predicate and bounds checks with one complete excursion check and
strided updates. Every changed cell is still written physically; partial runs,
host mutations and tape-edge crossings retain the ordinary fallback.

Arithmetic loops can also route through nested scans. For example,
`[->>[>]>+<<[<]<]` transfers a value into a destination found by zero sentinels.
Parsing combines consecutive pointer moves into one route step while retaining
every intermediate excursion for bounds checks. Static fill-target mismatches
are rejected at this stage, and valid fills keep precomputed excursion bounds.
An impossible fill candidate can still retry its affine summary after a literal
body, without repeating the rejected fill proof.
The engine first resolves the route without changing the tape, then verifies
that the pointer returns and every scan sees the same zero/nonzero predicates
throughout the loop. Written predicates include their precise partial update
at each scan and a conservative bound across the summarized iterations. For
example, a decreasing scanned cell may be folded only while every tested value
stays nonzero. If a later iteration might change a predicate, the summary stops
before that iteration, materializes every updated cell, and resumes at the loop
predicate. A new proof or ordinary BF handles the boundary, including any host
changes during a budget yield. Exit cells must remain zero at every summarized
evaluation. Cancelled writes remain part of this proof, because their
intermediate values could affect a scan.

Two-write routes have a smaller execution plan when parsing proves that the
counter and scan lanes are independent. The proof uses the greatest common
divisor of the actual BF scan strides, without a fixed tape layout. The second
write must immediately follow a zero-seeking scan, and its lane must be disjoint
from the counter and every later scan. Such a route changes an earlier zero
sentinel, so it summarizes exactly one body. Each invocation traces the route
once with the usual probe limit, checks every pointer excursion and the return
to the counter, then commits the two writes. Failed proofs leave the original
instructions available. This avoids initializing the general multi-iteration
proof's temporary arrays on each invocation, in both interpreter and JIT modes.
The derived execution plan is excluded from checkpoint program identity.

These routes may also return through a moving-marker search. A read-only
zero-run trace finds its destination, then the proof requires the marker to be
exactly one, the trailing and intermediate cells to be zero, and the found cell
to remain nonzero at each summarized search. Consuming the initial marker is a
symbolic decrement; all intermediate marker writes cancel before the enclosing
body continues. Repeated searches include their own preceding partial updates.
Failed or cold proofs leave real tape untouched. A closing-bracket retry lets
the first literal body warm a long route before folding later iterations.

Counted first-zero fills such as `[->>[>]+[<]<]` deliberately change their scan
predicates. A separate guard proves that the loop decrements its original
control, fills a zero, and returns over the growing run. The changing scans must
have equal and opposite strides; all other scan predicates must remain disjoint
from the counter and every new marker. Batches check actual zeros, pointer
excursions, and existing holes, and write every marker physically to the tape.
Previously verified zero runs avoid rereading the same fill destinations.
The remaining cells are checked normally, and a read-only cache lookup leaves
facts about other strides intact. Uniform writes use a contiguous fill or a
strided slice, with the same physical tape state in either BF direction.
Invariant scan paths are excluded for the whole batch, and cache invalidation
covers the written progression. The new nonzero cells become scan-cache facts,
so returning through a long freshly filled run needs no second traversal.
They resume at the closing bracket, so a host mutation after a budget yield is
observed by the next proof. Failed proofs retain the original BF instructions.

These optimizations recognize BF algorithms, without Ripple opcodes, register
numbers, memory banks, or CPU lane constants. They do not change exported BF or
its requirements for ordinary 16-bit wrapping interpreters.

```rust
use bf_wasm::engine::{Machine, Program, Stop};

let program = Program::parse(",.", true, false)?;
let mut machine = Machine::new(30_000)?;
loop {
    match machine.run(&program, 10_000)? {
        Stop::Halted => break,
        Stop::Budget => { /* check cancellation or time limits */ }
        Stop::Input => machine.provide_input(0)?, // caller chooses EOF behavior
        Stop::Output(byte) => { /* write this raw byte */ }
        Stop::Hook(marker) => { /* inspect/update machine.tape_mut() */ }
    }
}
# Ok::<(), bf_wasm::engine::Error>(())
```

Set the parser's third argument to `true` only for a host-aware BF export. It
recognizes `?` (before LOAD), `!` (after STORE), and `$` (instruction boundary).
These markers and ordinary input/output are optimization barriers. A hook or
output stop has advanced past its marker; the caller can mutate the tape before
resuming. Input stops repeat until `provide_input` is called. No host logic or
Ripple opcode execution lives in the core.

`Machine::write_checkpoint(&program, writer)` saves a resumable machine to any
`std::io::Write`. `Machine::read_checkpoint(&program, reader, max_tape_cells)`
restores it after checking the format version, parsed IR and allocation limit.
Both return `CheckpointError`. The record includes every logical 16-bit cell,
the pointer, literal instruction position, dispatch counter, pending input and
pointer-wrapping policy. Input still waits for `provide_input`; already returned
output and hooks are not replayed. The host must finish handling a returned
event before saving its machine and matching host device state.

Checkpoints use little-endian integers and compact tape pages: zero runs,
repeating patterns, sparse bitmaps or raw dense data. Saving materializes
deferred writes without executing BF; loading starts with empty derived scan
caches. The saved dispatch counter is exact, but later budgets and dispatch
counts can differ while caches warm up. BF events and logical state retain
their meaning. The program fingerprint includes parse options and all optimized
IR metadata, allowing execution-only performance improvements while rejecting
incompatible optimizer changes. Checkpoints are versioned, not a permanent
cross-version interchange guarantee. The host should checksum the complete
container and separately save source identity, external I/O and device state;
the generic core neither serializes devices nor authenticates files. Reading
stops at the machine record boundary so a host can append those records.

For opt-in execution diagnostics, pass a `Profile` to
`Machine::run_profiled(&program, budget, &mut profile)` and inspect
`profile.report()`. It reports operation timings and attempt/completion/fallback
counts for generic BF operations, plus a bounded table of expensive navigation
loop source offsets. The table tracks searches, routed loops, scans and walks;
category totals also cover arithmetic and literal instructions. Site eviction
and omission counters make incomplete site coverage explicit. Timings include
clock-query overhead and profiling slows execution, so use them to locate costs
and measure actual speed separately with ordinary `run`. The ordinary executor
uses a separate compile-time specialization without timers or profile counters.

Budgets and `dispatches()` count **IR dispatches**, not original BF characters or
Ripple instructions. One dispatch performs at most 128 summarized arithmetic
instructions, 256 scan or moving-marker search probes, or 256
combined moving-update iterations and cell writes. A scan probe can skip a
previously verified run in constant time, regardless of its
length. Long uncached scans and infinite loops yield to the caller; an exhausted
budget preserves resumable state. A zero budget does no work. Dispatch counts
can change when optimizations improve, and are not a stable virtual clock.
Routed loops examine at most 128 parts per proof, perform at most two proofs of
256 scan probes plus bounded predicate checks, and fill at most 1024 cells in a
dispatch. This work also stays independent of tape size and counter value.

The native `bf` CLI uses this core for its default wrapping 16-bit configuration
and writes raw output bytes. Other cell sizes, saturating cells and clamped tape
pointers use explicit literal compatibility execution. The existing WASM API is
unchanged.

## Optional native JIT

```sh
cargo build --release --manifest-path src/rust-bf/Cargo.toml
src/rust-bf/target/release/bf src/rust-bf/mandelbrot.bf --jit --stat
```

`--jit` is opt-in. The default `jit` build feature includes Cranelift for native
AArch64 and x86-64 hosts; `--no-default-features` builds the interpreter alone.
WASM builds exclude native code generation even with default features enabled.
Requesting JIT in an unsupported build fails explicitly. The CLI requires its
wrapping 16-bit configuration and rejects incompatible cell/pointer modes.
`--stat` or `--verbose` writes compile time, native code size, native dispatches,
entry/fallback counts and retired regions to stderr; stdout remains guest bytes.

Library callers create `engine::jit::Jit::new(&program)` and use
`jit.run(&mut machine, budget)` in place of `machine.run`. The cache borrows its
immutable program, preventing accidental reuse with different code. It compiles
hot control-flow regions after 32 samples, leaving affine/block summaries intact.
Only graphs containing a native cycle compile; inner loops exclude their exit
continuations. Regions averaging fewer than 16 dispatches over their first 64
native entries retire to Rust. Cached scans, moving-marker searches, deferred
fills and routed-loop proofs continue using the existing Rust implementations.
Cold stretches execute in batches of at most 64 IR dispatches, sampled between
batches and at host events. The ordinary interpreter has no JIT bookkeeping.

Each region covers at most 96 operations, 128 distinct relative cell offsets and
512 updates, with at most 256 compilations per executor. Dense dispatch/heat
metadata uses two bytes per parsed instruction, allocated only for JIT callers.
Graphs with a fixed pointer offset at every instruction prepare at most 128
exact cells and prove their complete pointer excursions before entry. Native
writes invalidate predicate facts only when zero/nonzero state changes. Other
graphs use a bounded 1024-cell physical window with per-operation bounds guards.
Both paths preserve deferred tape runs outside the prepared cells/window.
Native code allocations are released with the executor. Compilation failures
are explicit errors.

Every native operation consumes the same IR budget unit as its Rust equivalent.
Bounds failures and wrapping aliases return to retained original instructions;
I/O, hooks, pending input and host mutations retain synchronous behavior. Derived
cache changes can change future optimized dispatch counts, as with checkpoint
restoration. Native code is never serialized: a checkpoint can resume through
either executor and warms new native regions when requested. No guest opcode,
CPU register layout, device or host floating-point operation is recognized.

Native JIT is workload-dependent: arithmetic loops can benefit substantially,
while workloads dominated by existing scan/route summaries and device barriers
may remain faster in the interpreter. See the bounded, serial comparison driver
and measurements in [rbf/benchmarks](../../rbf/benchmarks/README.md).

Run differential and native CLI tests with:

```sh
cargo test --manifest-path src/rust-bf/Cargo.toml
cargo run --release --manifest-path src/rust-bf/Cargo.toml --example bench_engine
```

The `bench_engine` benchmark compares the older library interpreter and this
engine on small synthetic kernels, alternating order across five repetitions and checking
identical tape/pointer results first. It is not a Doom speed estimate.

It also accepts a terminating BF CPU fixture and its exact tape size:

```sh
cargo run --release --manifest-path src/rust-bf/Cargo.toml --example bench_engine -- build/bf-smoke/legacy.bf 16648
```

Both interpreters ignore host markers for this comparison, so use fixtures whose
execution does not depend on host device responses. Parsing is excluded from the
timings; both paths include tape allocation.

For comparisons between revisions of this native core, `bench_replay` records
the exact tape, pointer and event stream at every host hook and I/O stop. Save a
baseline executable before rebuilding, run both on the same frozen BF image,
and compare their `.trace` files. Timing excludes parsing and trace generation,
and includes machine allocation. Reproducible small CPU workloads and commands
are in [rbf/benchmarks](../../rbf/benchmarks/README.md). They exercise distant
instruction pointers, far RAM addresses and cross-bank calls without running Doom.
