# Native execution core

`bf_wasm::engine` provides the reusable 16-bit Brainfuck core. `Program::parse`
validates brackets and optionally enables arithmetic blocks, affine loops with
multiple destinations, cached scans, moving-marker searches, moving update
loops, and guarded loops containing arithmetic and scans. It parses once;
machines hold independent tape and execution state.
The core uses safe Rust and
wraps cells modulo 65536. Both the native `bf` CLI and Ripple's `rbf` host use it.

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

Arithmetic loops can also route through nested scans. For example,
`[->>[>]>+<<[<]<]` transfers a value into a destination found by zero sentinels.
The engine first resolves the route without changing the tape, then verifies
that the pointer returns and that no write touches any scan predicate, including
its exit cell. Only then can it fold the entire wrapping affine transfer.
Cancelled writes remain part of this proof, because their intermediate values
could affect a scan.

Counted first-zero fills such as `[->>[>]+[<]<]` deliberately change their scan
predicates. A separate guard proves that the loop decrements its original
control, fills a zero, and returns over the growing run. The changing scans must
have equal and opposite strides; all other scan predicates must remain disjoint
from the counter and every new marker. Batches check actual zeros, pointer
excursions, and existing holes, and write every marker physically to the tape.
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

Budgets and `dispatches()` count **IR dispatches**, not original BF characters or
Ripple instructions. One dispatch performs at most 128 summarized arithmetic
instructions, 256 scan or moving-marker search probes, or 256
combined moving-update iterations and cell writes. A scan probe can skip a
previously verified run in constant time, regardless of its
length. Long uncached scans and infinite loops yield to the caller; an exhausted
budget preserves resumable state. A zero budget does no work. Dispatch counts
can change when optimizations improve, and are not a stable virtual clock.
Routed loops examine at most 128 parts per proof, perform at most two proofs of
256 scan probes plus bounded predicate checks, and fill at most 128 cells in a
dispatch. This work also stays independent of tape size and counter value.

The native `bf` CLI uses this core for its default wrapping 16-bit configuration
and writes raw output bytes. Other cell sizes, saturating cells and clamped tape
pointers use explicit literal compatibility execution. The existing WASM API is
unchanged.

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
are in [rbf/benchmarks](../../../rbf/benchmarks/README.md). They exercise distant
instruction pointers, far RAM addresses and cross-bank calls without running Doom.
