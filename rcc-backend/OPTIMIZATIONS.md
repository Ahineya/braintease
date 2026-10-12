# Compiler optimizations

The backend optimizes each function before allocating its stack frame and
lowering instructions. These optimizations use the existing VM instruction set
and preserve the 16-bit C byte and two-cell banked pointer ABI.

## Pass order

1. **Single-store scalar promotion.** A scalar stack object can become a typed
   value when its address never escapes, its only store dominates every load,
   and all accesses are nonvolatile. Arrays, pointers, aliased objects and
   multiply assigned locals retain their memory representation.
2. **Constant propagation and folding.** Arithmetic uses the target integer
   width, including unsigned comparisons, shifts and 32-/64-bit wraparound.
   Typed materializers preserve multiword arguments at calls. Literal extension
   does not guess a missing source type. Division by zero, signed division
   overflow and invalid shifts are not folded.
3. **Arithmetic strength reduction.** Multiplication and unsigned division by
   powers of two become shifts; unsigned remainder becomes a mask. Signed
   division keeps its rounding semantics.
4. **Dead expression elimination.** Unused pure arithmetic and address
   calculations disappear. Loads, stores, calls and inline assembly remain.
5. **Loop invariant code motion.** Dominator analysis identifies natural loops.
   Pure arithmetic and address calculations whose operands are available before
   the loop move into an existing dedicated preheader. Dependent invariants
   move in dependency order. Memory accesses and division are not speculated;
   zero-trip loops retain their observable behavior. Irreducible loops and
   loops requiring phi repair or a new preheader are skipped.

During lowering, CFG liveness reclaims dead registers and instruction-local
scratch values. Live pointer bank metadata and all companion words survive
branches and calls. Operand registers are reused only after their last use.
Spill locations remain available for alternate control-flow paths.
Call argument materialization emits any required spill before overwriting its
register; pinned arguments that are still live after a call are saved too.

Dynamic pointer arithmetic first scales the displacement with fixed-register
shifts/additions. Positive or negative one-word displacements use native
carry/borrow and bank adjustment. Other displacements use a 32-bit fallback.
All scratch allocation precedes internal branches, so both paths initialize
the same spill state. Non-power-of-two banks such as 64,000 are supported.

## Volatile memory

The frontend retains `volatile` on the correct type layer, including typedefs,
arrays, aggregate members and pointer targets. IR loads and stores record the
qualifier explicitly; old serialized IR defaults to nonvolatile. Device
accesses retain their order and count, including discarded-value reads and
read-modify-write expressions. `volatile` does not promise atomic multiword
accesses or introduce a new synchronization primitive.

## Validation

Run the Rust suites with:

```sh
cargo test -p rcc-backend -p rcc-frontend -p rcc-preprocessor
```

The C fixtures `test_optimizer_semantics`, `test_register_lifetime_reclamation`,
`test_call_argument_spill_order`, `test_va_arg_register_pressure` and
`test_volatile_semantics` cover loops, aliases, calls, branch lifetimes,
bank crossings and observable volatile MMIO reads. The call spill test
reproduces a wide parameter overwritten while materializing a null argument;
this was caught by the first E1M1 snapshot after enabling scalar promotion.
Run the fixtures with the Rust VM backend.
`ports/doom/test.py --stage engine` compares title/menu/E1M1 screenshots
against the native build; the foundations stage checks arithmetic and assets.

The frontend IR dump precedes these backend passes. Unit tests in
`src/optimize/tests.rs` inspect the optimized IR directly. No Brainfuck execution
is required for these checks.

## Doom measurements

A consecutive before/after run of the same scripted menu sequence, using the
unchanged Rust VM, bank size 64,000 and the shareware WAD, produced:

| Work | Before | After |
| --- | ---: | ---: |
| Startup through first title frame, instructions | 3,420,847,362 | 880,669,255 |
| Steady menu frame, median instructions | 267,967,742 | 26,110,899 |
| Steady menu frame, median wall time | 2.615 s | 0.224 s |
| Indexed-to-RGB565 kernel, instructions | 238,618,108 | 15,876,529 |

The steady-frame medians use frames 3–7 from one run per compiler. Wall time
depends on host load; the instruction reduction is 10.26×. All seven captured
frames match byte-for-byte. The conversion kernel checks all 64,000 pixels with
varying palette entries and both source and destination crossing a bank.

The final build passes 410 Rust tests, 157 registered C regressions, three
focused C checks, all nine Doom foundation checks, and exact native pixels and
state logs for title, menu-to-E1M1, and direct E1M1 startup. These are startup
and menu measurements, not a sustained gameplay benchmark.

Local ignored artifacts are in `ports/doom/build/performance-after/`: the
`results.json` and `manifest.json` record measurements, pixel comparisons and
binary/fixture hashes; `timing.rs` uses the full VM runner without profiling;
`frame4-comparison.json` records a separate instruction profile. Engine and
foundation results are in `ports/doom/build/optimized-fixed-engine/` and
`ports/doom/build/optimized-fixed-foundations/`.

In the menu profile, frame conversion still consumes about 59% of instructions
and patch drawing about 40%. Spill loads/stores and their address setup fell
from 91.2 million to 6.54 million instructions per sampled frame, but still
account for about 25% of the remaining work. Further renderer and register
allocation work is needed for playable speed.
