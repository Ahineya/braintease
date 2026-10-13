# Compiler optimizations

The backend optimizes each function before allocating its stack frame and
lowering instructions. These optimizations use the existing VM instruction set
and preserve the 16-bit C byte and two-cell banked pointer ABI.

## Pass order

1. **Single-store scalar promotion.** An integer or pointer stack object can become a typed
   value when its address never escapes, its only store dominates every load,
   and all accesses are nonvolatile. Pointer promotion aliases an existing SSA
   value of exactly the same pointer type, preserving its address and bank
   together. Explicit-bank initializers, arrays, aliased objects and multiply
   assigned locals retain their memory representation.
2. **Constant propagation and folding.** Arithmetic uses the target integer
   width, including unsigned comparisons, shifts and 32-/64-bit wraparound.
   Typed materializers preserve multiword arguments at calls. Literal extension
   does not guess a missing source type. Division by zero, signed division
   overflow and invalid shifts are not folded.
3. **Arithmetic strength reduction.** Multiplication and unsigned division by
   powers of two become shifts; unsigned remainder becomes a mask. Signed
   division keeps its rounding semantics.
4. **Native GEP indices.** A zero-extension of a known one-word integer can
   supply its original word to pointer arithmetic. Signed extensions and wider
   source values retain their full displacement.
5. **Dead expression elimination.** Unused pure arithmetic and address
   calculations disappear. Loads, stores, calls and inline assembly remain.
6. **Loop invariant code motion.** Dominator analysis identifies natural loops.
   Pure arithmetic and address calculations whose operands are available before
   the loop move into an existing dedicated preheader. Dependent invariants
   move in dependency order. Memory accesses and division are not speculated;
   zero-trip loops retain their observable behavior. Irreducible loops and
   loops requiring phi repair or a new preheader are skipped.

After small-function inlining, pruned SSA promotion also handles mutable integer
and pointer locals when every read is definitely initialized. Pointer stores
must supply an SSA value of exactly the same pointer type; aliases preserve the
address and bank together. Loop and branch joins use typed Phi nodes, lowered
to private homes after optimization. Volatile objects, escaped addresses and
unsupported pointer representations retain their original memory accesses.
This removes intermediate cursor loads/stores in loops such as packed-byte
decoding, without assuming that a pointer stays in one bank.

During lowering, CFG liveness reclaims dead registers and instruction-local
scratch values. Live pointer bank metadata and all companion words survive
branches and calls. Operand registers are reused only after their last use.
Spill locations remain available for alternate control-flow paths.
Each pointer bank has one canonical named spill home, shared with the pointer's
coordinated spill. Different predecessor spill orders cannot create a second
home that is uninitialized on one path through a join.
Call argument materialization emits any required spill before overwriting its
register; pinned arguments that are still live after a call are saved too.

Repeated dynamic bank-tag checks reuse a resolved copy within one basic block.
The original tagged word remains available for pointer stores and call arguments;
the resolved copy participates in liveness and spilling like other companions.
Block entries, calls, inline assembly and value redefinitions invalidate the
cache. This removes repeated bank dispatch from constant-index initialization
without assuming that adjacent elements occupy the same bank.

Spill and reload addresses use `ADDI SC, FP, offset` directly, rather than
copying FP and adding the offset separately. Conditional branches materialize
their predicate before flushing live successor values. A predicate used only
by the branch stays in its physical register without a stack round trip;
predicates reused in successors still receive a spill slot. The flush uses
reserved scratch registers and preserves the condition until the branch.
Execution tests cover wide conditions, register pressure, pointer banks,
alternative successor paths, and loop backedges.

Global initializers reuse their relocated destination cursor between stores,
advancing by the difference between local offsets instead of reloading the GP
base for every cell. The proof is local to one initializer and is discarded at
labels, branches, clobbers and unknown instructions. Memory writes retain their
values, order and count. `global_init_cursor` covers arrays, multiword values,
relocated data pointers and callable function pointers.

The linker omits calls to proven RET-only initializers and places the startup
dispatcher and safely movable compiler initializers near the image entry.
Data reservations and initializer invocation order remain unchanged. Opaque
control flow and functions that cannot safely be separated retain conservative
layout; this optimization does not change the ISA or bank size.

Dynamic pointer arithmetic first scales the displacement with fixed-register
shifts/additions. Positive or negative one-word displacements use native
carry/borrow and bank adjustment. Other displacements use a 32-bit fallback.
All scratch allocation precedes internal branches, so both paths initialize
the same spill state. Non-power-of-two banks such as 64,000 are supported.

A one-cell element with a native unsigned index uses a smaller path with six
working registers, including the result pointer. It normalizes the input offset
and index separately before adding with carry. This supports every nonzero
16-bit bank size, noncanonical integer-to-pointer offsets, and live operands
without reserving registers for a general 32-bit division fallback. The full
64,000-pixel bank-crossing conversion kernel fell from 15,876,529 to 14,980,529
instructions with this specialization; the pointer-walk variant fell from
13,248,403 to 11,072,403. All output pixels remained identical.

Constant 32-bit shifts lower directly to word shifts and merges. Counts of zero,
16, below 16 and above 16 each have a dedicated path, including arithmetic sign
fill. This avoids the dynamic shift dispatch and uses at most two scratch
registers. Tests execute all 32 defined counts for left, unsigned right and
signed right shifts under register pressure, and limit the emitted instruction
count to prevent the dispatch overhead from returning.

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
`test_native_unsigned_gep` additionally checks native unsigned displacements,
noncanonical pointer offsets, and preservation of signed and wider indices.
`test_single_store_pointer_promotion` checks repeated wide stores through global,
stack and heap pointers, including a word straddling the 64,000-cell boundary,
pointer returns, escaped locals and volatile pointer objects. Backend tests
exercise resolved-bank reuse under register pressure and each cache barrier.
`pointer_bank_spill_join` covers both successors of a conditional pointer store
before returning a heap pointer, including the path that skips the store.
`test_mutable_pointer_promotion` covers repeated cursor updates, stack/global/far
pointers, bank crossings, zero-trip null cursors, branches, early returns,
escaped locals and volatile pointer objects.
`make -C ports/doom test` compares title/menu/E1M1 screenshots
against the native build; the fixtures stage checks arithmetic and assets.

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

That initial optimization build passed 410 Rust tests, 157 registered C regressions, three
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

The subsequent gameplay work adds the native unsigned GEP and constant shift
paths above. Its compiler validation passes 414 Rust tests and 199 registered C
regressions. Doom also uses port-local assembly kernels for rasterization,
palette conversion, memory operations and fixed-point arithmetic, all using the
existing ISA. See `ports/doom/README.md` for sustained gameplay measurements and
native pixel comparisons; these include renderer improvements beyond the
general compiler passes.

Mutable pointer SSA promotion (2026-10-13) reduces the shareware directory load
from 1,756,192 to 1,606,410 guest instructions (8.5%). Within it, `wad_read` falls
from 833,645 to 739,658 instructions (11.3%), with the same 10,118 packed disk
word reads. The full title frame falls from 248,188,214 to 236,489,409 guest
instructions. These are instruction counts with unchanged 64,000-cell banks,
not BF wall-time estimates. Native/Rust VM title, menu-to-E1M1 and direct E1M1
pixels match exactly. Pointer Phi homes add entry/exit work, so very short reads
need not improve; the small reader fixture grows from 52,433 to 52,676 guest
instructions. Local measurement artifacts are under `build/pointer-ssa/`.
