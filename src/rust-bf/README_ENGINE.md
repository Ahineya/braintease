# Native execution core

`bf_wasm::engine` provides the reusable 16-bit Brainfuck core. `Program::parse`
validates brackets and optionally enables arithmetic blocks, affine loops with
multiple destinations, and scans. It parses once; machines hold independent tape
and execution state. The core uses safe Rust and wraps cells modulo 65536.

`Machine::new(cells)` uses strict tape bounds. `with_wrapping_tape(cells)` supports
the traditional command-line pointer behavior. Optimizations check the complete
pointer range, including intermediate excursions. When a range crosses an edge,
the retained original instructions preserve wrapping aliases or the partial state
before a bounds error. Loops with zero initial control cells skip their bodies.

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
instructions or 256 scan steps. Long scans and infinite loops yield to the caller;
an exhausted budget preserves resumable state. A zero budget does no work.

The native `bf` CLI uses this core for its default wrapping 16-bit configuration
and writes raw output bytes. Other cell sizes, saturating cells and clamped tape
pointers use explicit literal compatibility execution. The existing WASM API is
unchanged.

Run differential and native CLI tests with:

```sh
cargo test --manifest-path src/rust-bf/Cargo.toml
cargo run --release --manifest-path src/rust-bf/Cargo.toml --example bench_engine
```

The benchmark compares the previous library interpreter and the new engine on
small synthetic kernels, alternating order across five repetitions and checking
identical tape/pointer results first. It is not a Doom speed estimate.

It also accepts a terminating BF CPU fixture and its exact tape size:

```sh
cargo run --release --manifest-path src/rust-bf/Cargo.toml --example bench_engine -- build/bf-smoke/legacy.bf 16648
```

Both interpreters ignore host markers for this comparison, so use fixtures whose
execution does not depend on host device responses. Parsing is excluded from the
timings; both paths include tape allocation. The small Ripple fixture above ran
about 1.8 times faster with the new core, with identical final tape and pointer.
