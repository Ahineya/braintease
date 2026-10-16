# RFL implementation audit

RFL has a useful core, but does not yet implement the complete language described by
[initial.md](initial.md), [methods.md](methods.md), [more.md](more.md), and
[traits.md](traits.md). The failures were broader than missing syntax: evaluation
order, frame reuse, ownership, exhaustiveness, and generated code size all had
correctness problems. This audit distinguishes repaired behavior from remaining
work. Measurements below are from 15 October 2026.

## Repairs

- **Functional Brainfuck interpreter:** `examples/bf-fn.rfl` now prints `A` and
  exits successfully. Frame allocations carry their owning arena chunk and rewind
  checkpoint in a three-cell prefix. Release restores that checkpoint instead of
  guessing ownership from address ranges. The frame payload ABI stays unchanged.
- **ADT destruction:** destructor arguments are consistently `(tag, address,
  bank)`. Previously these were saved in the wrong order, corrupting cleanup.
- **Evaluation order:** operands and arguments hold value snapshots. A later
  argument mutating a binding no longer changes the already-evaluated argument.
- **Tail calls:** tail position survives sibling branches and unit-valued tails.
  Calls borrowing locals keep their owner frame alive; C entry wrappers retain
  their root frame and result. Both value and unit recursion have protocol tests.
- **Patterns:** a pattern matrix checks nested constructors, products, booleans,
  guards, coverage, and shadowed arms. The two redundant fallback arms in
  `bf-fn.rfl` were removed because its nested cases are exhaustive.
- **Early returns in match arms:** returning arms no longer force other arms to
  return `unit`. Segments are emitted in reverse postorder so pointer definitions
  precede blocks they dominate. Dead continuations do not enter resume dispatch.
- **Fixed arrays inside products:** layout records the element width, rather than
  multiplying by the array length twice. This previously produced diagnostics such
  as `expected [i16; 2], found [i16; 2]`.
- **Characters in collections:** vectors, slices, and fixed arrays accept `char`;
  character indexing, mutation, COW, push, and pop have an execution regression.
- **Strings:** concatenation, slicing, character encoding, and equality use shared
  generated helpers. Replacing a string releases its previous value. Inclusive
  slice ends check overflow before becoming exclusive ends.
- **Checking and diagnostics:** `check_source` and `check_file` do not lower RFL
  to executable IR. Libraries can be checked without `main`. File compilation
  maps type errors back from the combined module buffer to the original file.
- **Runner artifacts:** each run removes its own temporary assembly, object, and
  binary files, including on errors.

## Compiler organization

The former 8,844-line checker and 8,804-line lowerer are split by responsibility.
The entry files coordinate compilation and hold shared state; implementation is
in private child modules.

| Area | Files |
|---|---|
| Types, registration, specialization | `src/check/{types,register,functions,inference}.rs` |
| Expressions and calls | `src/check/{expressions,operators,calls,control}.rs` |
| Products, patterns, coverage | `src/check/{products,patterns,coverage}.rs` |
| Methods, references, captures | `src/check/{methods,borrows,closures,captures}.rs` |
| Checked expressions to segmented frame code | `src/lower/build.rs`, `src/lower/expressions.rs` |
| IR emission and call protocol | `src/lower/{emit,frames,dispatch}.rs` |
| Ownership and object representations | `src/lower/{ownership,buffer_ownership,objects,memory}.rs` |
| Collections, text, arithmetic, C ABI | `src/lower/{arrays,vectors,strings,string_runtime,numeric,ffi}.rs` |
| RFL optimization preparation | `src/lower/{optimize,temporaries,vector_fill_build,vector_fill}.rs` |

Struct and tuple layout tables are shared between procedures rather than deeply
cloned for each function. This is a structural cleanup, not a claim that the
checker now has an ideal internal API: several existing helpers still take too
many arguments, and inference and IDE metadata remain coupled to checking.

## Optimization changes

RFL's resumable heap frames hide locals from the backend's ordinary stack-local
SSA promotion. `promote_frame_words` gives nonescaping word, 32/64-bit integer, floating-point, character, and boolean slots local
homes while an activation runs, reloads them on entry, and publishes them before
a call stores its resume state. Results, borrowed slots, managed values, and
shared mutable captures keep their existing storage. The shared backend can then
promote the local homes and optimize arithmetic and loops. Slots with partial or differently typed accesses remain in memory. Sub-word
integer slots are not promoted by this pass yet.

Only actual suspension continuations enter resume dispatch. Resume and function
selection use balanced decision trees. Unreachable RFL functions are excluded
from the driver after checking, including unused standard-library functions.
Closure targets and C exports remain roots or reachable dependencies.

Frame cleanup is generated once per procedure, rather than expanded at every
return and tail call. This matters for correctness as well as speed: duplicated
cleanup and string loops can overflow a code bank in otherwise small programs.
The existing string construction test exceeded the bank limit before these
changes and now assembles, links, and executes.

The shared backend optimizer itself is unchanged; these passes expose RFL's
semantics in a form it already understands. More complete lifetime analysis, checked-access elimination, and call elimination
remain optimization opportunities.

## Measurements

Release compiler built from baseline commit `a71b038` versus this implementation,
using the same backend sources and benchmark inputs. Compilation times are the
median of three compiler invocations, excluding Rust builds. Execution uses the
interpreter with 64,000-cell banks. Guest instruction counts come from RVM reports;
they are not wall-clock speedups.

| Input | Compile before | Compile after | Assembly instructions before / after | Executed instructions before / after |
|---|---:|---:|---:|---:|
| `benchmarks/scalars.rfl` | 83.7 ms | 47.3 ms | 2,981 / 2,591 | 3,699,137 / 709,206 |
| `benchmarks/strings.rfl` | 207.1 ms | 166.4 ms | 25,343 / 21,420 | 63,404 / 55,189 |

The scalar workload executes about 5.2 times fewer instructions. Both versions
print `29994`. Both string workloads print `Hello é` and `true`. These are focused
measurements, not a general performance guarantee.

The original functional BF example failed at runtime with `unreachable state`.
The corrected example prints `A`; its newly recognized exhaustive matches are
rejected by the baseline checker after removing the redundant fallback arms, so
the benchmark does not report a misleading like-for-like speedup for that file.

Reproduce with:

```sh
cargo build --release -p rfl
python3 rfl/tools/benchmark.py --baseline /path/to/old/rfl --execute
```

Omit `--baseline` to measure the current compiler alone. The script preserves
compile, assembly, link, and runtime failures in its JSON output.

## Managed collections and the imperative Brainfuck example

Vectors, owning slices, and fixed arrays now accept managed and product elements:
strings, nested collections, structs, tuples, ADTs, and closures. The typed module
carries element layouts into lowering. Buffer destructors release every live
element when their last vector or slice owner dies; a slice owns the whole backing
buffer. COW detachment retains copied elements, unique growth transfers their
ownership, indexed replacement releases the previous element, and extraction,
`get`, `pop`, array copies, and `to_vec` preserve independent owners. Borrowed
references remain forbidden as stored collection elements, including in tuples.
Forward-declared struct widths are resolved in collection element tables.

The IR revealed a severe lifetime bug in `examples/bf.rfl`: loading `tape[tp]`
first retained an operand snapshot, which stayed alive until frame exit. The
following indexed write saw a shared buffer and copied all 30,000 tape cells.
Repeating that operation drove a Hello World program to **2,003,263,115 guest
instructions**. This was real execution work, separate from compilation.

Single-use managed operands are now released immediately after their consumer.
Simple collection reads use their binding directly when evaluation cannot mutate
it; an index with effects still evaluates against an owning receiver snapshot.
Fat pointers are assembled from SSA integer values, eliminating the temporary
stack union previously used for every address/bank pair. These changes preserve
left-to-right argument and operand evaluation.

The tape's 30,000 individual pushes were another large cost. A conservative
fresh-vector optimization recognizes a constant push loop immediately after an
empty vector initialization and emits one allocation plus a bulk cell fill.
It requires no intervening operation that could expose an alias, a constant
exclusive range, and a single-cell constant value representable by runtime
`memset`. Other loops retain their ordinary semantics. Tests cover bank crossings,
existing contents, aliases, empty ranges, and values outside the fill range.

A separate before/after comparison for this follow-up uses the compiler binary
saved immediately before these collection and performance fixes. Both versions
compile the **unchanged** `examples/bf.rfl`. The C variants reuse the existing
`c-test/examples/brainfuck.c` interpreter with a fixed-input `main`, excluding
interactive prompts. All print exactly `Hello World!\n`.

| Interpreter | Guest instructions | VM process time, median of 3 |
|---|---:|---:|
| RFL before this follow-up | 2,003,263,115 | 14.445 s |
| RFL after this follow-up | 5,378,037 | 0.037 s |
| C, original 4,096 byte tape | 499,524 | 0.012 s |
| C, matching 30,000 unsigned 16-bit tape cells | 1,742,865 | 0.026 s |

The RFL result uses about **372 times fewer instructions**. Across measurements,
current RFL VM time ranged from 0.037 to 0.059 s and matched C from 0.019 to
0.029 s. These process times include startup, exclude compilation/assembly/linking,
and use the interpreter without JIT. Wall time is sensitive to host load;
instruction counts stayed stable.

The latest compilation medians were 0.602 s before versus 0.628 s after. Earlier
current-compiler medians ranged from 0.955 to 1.369 s under different host load;
this work does not claim a compilation speedup. Assembly decreased from 37,143
to 30,481 instructions. Wider SSA preparation still merits compiler profiling.

C remains ahead: the matching-tape variant executes about three times fewer
instructions. RFL still emits checked dynamic collection operations, UTF-8
character indexing, COW uniqueness checks, and 32-bit index arithmetic; C uses
static storage, unchecked element access, 16-bit indexes, and explicit wrap
branches. Further optimization should preserve the higher-level semantics while
removing provably redundant checks and arithmetic.

Reproduce and inspect the frontend IR with:

```sh
cargo build --release -p rfl
python3 rfl/tools/benchmark_bf.py --baseline /path/to/rfl-before --runs 3
target/release/rfl compile rfl/examples/bf.rfl --emit-ir
```

Omit the baseline argument to compare the current RFL compiler against both C
variants. This script builds temporary C runners and removes all generated files.
The general benchmark script also accepts repeated `--case` arguments and reports
VM wall time separately from compilation.

## Floating-point workload: `beep.rfl`

The unchanged example generates 44,100 samples using software binary64 `sin`,
then formats each sample as a decimal line. It emits 391,700 bytes of text;
there is no audio-device write in this example.

Baseline compiler and math-library snapshots for this investigation correspond
to commit `0cc8f7b`. A complete headless JIT run, excluding compilation, assembly,
and linking, gave:

| Full 44,100-sample workload | Before | After |
|---|---:|---:|
| Guest instructions | 10,307,734,663 | 7,154,129,521 |
| JIT process time | 23.28 s | 17.47 s |
| Compile time | 1.68 s | 1.25 s |
| Assembly instructions | 30,976 | 26,518 |

These are one complete run per version, with wall time dependent on host load.
Guest work fell by 30.6% and measured execution time by 25%. Both runs completed
successfully, produced 44,100 lines, and had identical output SHA-256 hashes.

Changes responsible for the reduction:

- Immutable scalar propagation folds finite normal binary64 constant expressions
  and exact integer-to-binary64 conversions before they become opaque software
  helper calls. Borrowed, shared, parameter, captured, and multiply assigned
  values do not become constant facts. Operation order is preserved; special
  values and subnormal arithmetic continue through the runtime.
- Frame scalar promotion includes `f32`, `f64`, and 64-bit integers, with the same
  protection for borrowed/shared storage and partial-word accesses. The shared
  optimizer can eliminate more temporary frame loads and stores.
- `sin` and `cos` compute their quadrant reduction once. Previously separate
  helpers each divided by `HALF_PI` and called `floor` for the same input.
- Float formatting extracts its six fractional digits with bounded 32-bit
  arithmetic. Quotients and remainders share a division, using `r = n - q*d`.
  Small unsigned integers have a direct single-digit printing path. Rounding,
  scientific notation, signs, and trimming behavior are preserved.

The bounded controls use the first 128 samples while keeping the original 44,100
sample rate. Math controls accumulate an observable checksum and print it once,
so future pure-call elimination cannot silently remove their computation.

| 128-iteration control | Guest instructions before | Guest instructions after |
|---|---:|---:|
| Original calculation and formatting | 29,435,412 | 20,317,638 |
| Calculation plus final checksum | 23,505,216 | 18,282,825 |
| Repeated formatting of `0.625` | 6,152,956 | 2,276,482 |

The formatter control uses 63% fewer instructions. Calculation remains the
largest cost. Arithmetic checksum controls estimate these incremental costs,
relative to a conversion-and-checksum control:

| Added binary64 operation | Extra guest instructions per iteration |
|---|---:|
| Addition | approximately 1,489 |
| Multiplication | approximately 6,101 |
| Division | approximately 25,007 |

These are differences between controlled loops, including associated lowering
and checksum effects, rather than function self-costs from a PC sampler.
`runtime/softfloat.c` implements multiplication with a multiword 128-bit product
and division with a 64-step 128/64-bit integer division. The JIT compiles those
integer guest instructions; it does not recognize them as host floating-point
operations.

The next priorities suggested by this evidence are:

1. Improve the software multiply/divide algorithms and common-case paths.
   The subsequent audit below implements this using guest integer instructions.
2. Preserve floating-point semantics and purity through optimization, lowering
   to software helpers later. Opaque calls currently prevent ordinary common
   subexpression elimination and loop-invariant motion from seeing arithmetic.
3. Inline small nonrecursive scalar math functions, removing their activation
   frames and driver dispatch from tight loops while retaining general recursion.

Reproduce with:

```sh
python3 rfl/tools/benchmark_beep.py --samples 128 --micro --runs 3
python3 rfl/tools/benchmark_beep.py --full --jit --runs 1
```

For before/after runs, supply both `--baseline /path/to/rfl-before` and
`--baseline-math /path/to/math-before.rfl`. An old compiler alone is insufficient
because the standard library is read from source during compilation. The script
checks output hashes, captures output without dumping thousands of lines, and
keeps compilation time separate from execution time.

Validation passed all 145 RFL tests, including new checks for runtime/constant
binary64 agreement, mutable and borrowed floats across calls, and formatting
edge cases. The imperative BF benchmark remains at 5,378,037 guest instructions.

## Integer software-float and stdlib follow-up

This comparison starts from `95ac58b`, including the preceding JIT improvements.
Both versions use the same RVM binary. The saved baseline compiler, math source,
and runtime archive are all required: replacing only the compiler would link the
new arithmetic implementation into both programs. Execution times are medians
of three serial runs, excluding front-end compilation, assembly, and linking. JIT time
includes cold native compilation.

| Full 44,100-sample beep | Before | After |
|---|---:|---:|
| Executed guest instructions | 7,154,129,521 | 4,497,887,860 |
| JIT process time | 6.091 s | 4.377 s |
| RFL compile time | 1.254 s | 1.146 s |
| RFL assembly instructions | 26,518 | 20,824 |

Guest work falls by 37.1%; measured JIT process time falls by
28.1%. No ISA changes, guest hardware-float instructions, or host-float JIT
substitutions are involved. The Brainfuck VM can execute the same integer
algorithms; its wall time was not measured.

The bounded controls below execute 128 iterations with observable checksums.
Square-root inputs are `i + 0.125`; both versions give identical printed checksums.

| Control | Guest instructions before | Guest instructions after |
|---|---:|---:|
| Beep math plus checksum | 18,282,825 | 10,492,928 |
| Binary64 multiplication | 1,147,367 | 722,394 |
| Binary64 division | 3,567,318 | 2,149,235 |
| Square root | 68,963,660 | 4,770,359 |
| Floor | 1,346,856 | 748,405 |
| Binary32 addition | 281,271 | 298,420 |
| Binary32 division | 600,723 | 633,400 |

The square-root control executes 14.5 times fewer instructions. Relative to the
conversion-and-checksum control, the estimated incremental binary64 multiplication
cost drops from about 6,101 to 3,362 instructions, and division from about 25,007
to 14,510. Binary32 addition/division controls increase by approximately 6%/5%
while gaining proper rounding; their short wall times are sensitive to startup
and host load.

**Runtime arithmetic.** Binary64 division now generates a normalized quotient
with 53 value bits, guard, round, and a sticky remainder. It performs 54 iterations
on two 32-bit remainder/quotient limbs, avoiding the old shifted 128-bit numerator
and its extra bookkeeping. Exact powers of two bypass general multiplication and
division while still using the rounding path for underflow and overflow. Widening
32x32 multiplication is expressed with 16-bit pieces instead of requesting four
general 64x64 products for the 106-bit significand product. Exact 32-bit integer
to binary64 conversion also bypasses generic 64-bit normalization and rounding.

The audit found correctness defects as well as excessive work:

- Binary32 arithmetic and integer conversions truncated, lost subnormal values,
  and generated too few quotient bits. It now uses a 27-bit working significand
  with guard, round, and sticky bits, and round-to-nearest-even. Addition and
  division stay in 32-bit integers; multiplication uses a 48-bit product assembled
  from 16-bit pieces.
- Binary32-to-binary64 widening discarded the sign into a 32-bit variable.
- Adding binary64 subnormals could incorrectly set the normal exponent bit.
  Subtraction must also treat a discarded alignment tail as a subtraction.
- Binary64-to-binary32 conversion rounded before handling gradual underflow.
- Binary32-to-unsigned and 64-bit integer casts incorrectly passed through a
  signed 32-bit conversion. The integer ABI retains saturating casts and NaN-to-zero.
- The RFL checker rejected valid constant float-to-`u64` conversions above `i64::MAX`.

NaNs remain canonicalized by arithmetic helpers. These routines have a fixed
round-to-nearest-even arithmetic contract, without an exception-flag API.
Binary32 correctness has a small instruction cost in some controls, shown above.

**RFL frontend.** Immutable propagation now folds binary32 arithmetic and
float-width conversions before helper lowering, with the existing safeguards for
mutation, sharing, borrowing, and special values. Operations retain their source
order and binary32 precision. A separate scalar liveness analysis follows branches,
loops, and call continuations. Only values needed after a resumable call are
published to the heap frame; consumed temporaries no longer get saved at every
subsequent call. Callee-written results, borrowed values, managed storage, and
captured values retain their required behavior. Closure creation conservatively
keeps scalar inputs live because capture operands are held in lifted metadata.

**Standard math.** Sine and cosine reduce to `[-pi/4, pi/4]` using multiplication
by `2/pi` and a split `pi/2` for ordinary angles, avoiding software division and
flooring. Inputs already inside that interval skip reduction. Large finite angles
use a 1152-bit fixed-point `2/pi` table and integer multiplication to recover the
quadrant and fractional remainder; they no longer saturate a quadrant into `i32`
or hit the arbitrary `1e300` finite cutoff. The Taylor polynomials approximate sine
and cosine with an absolute error bound of `2e-10`; their results can differ from
correctly rounded libm results.

`floor` and `sqrt` are small wrappers around integer helpers in
`runtime/softmath.c`. Floor works beyond the old signed-32-bit boundary. The root
uses a digit-by-digit integer algorithm with a remainder for correct rounding,
replacing sixteen Newton divisions with an unsuitable initial guess. Tests cover
positive infinity, negative zero, the smallest subnormal, and the largest finite
binary64 input. Ceiling, rounding ties, large integral values, and signed zero
also have corrections and execution tests.

The beep output intentionally changes: the maximum difference from the old
printed output is `0.000001`. All 44,100 current printed samples were checked
against native sine, with maximum absolute error `4.987e-7`, within six-decimal
rounding. The unformatted trigonometry tests check against native results at
quadrant boundaries, adjacent representable values, audio angles, and very large
positive and negative angles. The benchmark continues to require identical output
by default; numeric tolerance must be requested explicitly.

Validation passed all **151 RFL tests**, six C/runtime tests (including bit-pattern
regressions), and **13,369,400** seeded reference checks for arithmetic, comparisons,
integer conversions, width conversions, floor, square root, and large-angle
reduction. The native test adapter fixes C integer widths to match Ripple; it does
not replace the VM execution tests. The oracle independently regenerates all 1152
table bits using high-precision arithmetic.

Reproduce after saving all three baseline artifacts:

```sh
cargo build --release -p rfl
make -C runtime
python3 runtime/tests/softfloat_oracle.py --samples 300000 --math
cargo test --release -p rfl -- --test-threads=4
./rct test_softfloat_bits test_softmath_bits test_float_basic test_printf_float test_sscanf_float test_static_float_to_integer --timeout 20
python3 rfl/tools/benchmark_beep.py --baseline /path/to/rfl-before \
  --baseline-math /path/to/math-before.rfl --baseline-runtime /path/to/runtime-before.par \
  --full --jit --runs 3 --max-output-error 0.000001
```

`--micro --f32 --samples 128` adds observable arithmetic, square-root, floor, and
formatting controls. In paired micro runs, use a tolerance appropriate to the
accumulated checksum, such as `--max-output-error 0.0001`. The JSON records output
hashes and the measured maximum error. On macOS the host oracle needs a matching
compiler and SDK; it respects `CC` and `SDKROOT`.

Further opportunities are semantic float operations or explicit purity through
backend optimization, known-width widening multiplication in the shared backend,
and inlining small scalar RFL functions. Common-entry scalar reloads remain a
source of register pressure in large procedures; selecting reloads at each resume
entry is a separate optimization from the live-value publication implemented here.

## Static traits and library printing

The [traits specification](traits.md) is implemented for static traits:
required method signatures, concrete and generic implementations, explicit
`T: A + B` bounds, inferred call type arguments, qualified calls, module imports
and aliases, lexical method visibility, coherence, and orphan checking. Inherent
methods win by name; traits are considered before receiver-first free functions.
Ambiguity is diagnosed without overloading by argument type.

Generic declarations are checked with rigid nominal placeholders in an isolated
checker, including unused declarations. Each bound method selection is retained
for concrete specialization, so an eventual inherent method cannot change a
generic function's meaning. Placeholder layouts and signature-only calls never
reach IR. Existing numeric generic operations retain their specialization-time
operand checks; operator traits are outside this specification.

`std/io` defines `Printable` and ordinary generic `print`/`println`. The prelude
and explicit imports share the same definitions. Implementations cover signed
and unsigned integers, `usize`, bool, char, String, f32/f64, Vec, Option, and
Result, with recursive payload bounds. The compiler supplies only the
`__write_value` scalar/text formatting leaf and byte output. New user types need
no changes to the checker or lowerer. `println` writes its newline with `putc`,
without re-entering generic printing. See [examples/traits.rfl](examples/traits.rfl).

Borrowed matches bind shared payload references backed by an owned snapshot;
managed payloads remain alive and are released through ordinary frame cleanup.
Guards receive initialized references too. Safe `*reference` reads and mutable
reference writes reuse the existing ownership-aware field operations. Shared
receiver borrows of temporary values use frame-local snapshots. Explicit borrows
still require names; field and index borrowing needs a common place representation.

Calls lower to ordinary monomorphized functions, preserving the current frame
ABI and all targets. There are no vtables, runtime trait tags, or new VM opcodes.
Generic trait parameters, generic trait methods, default methods, and dynamic
dispatch remain outside this implementation. Associated types are implemented
as the 0.3 extension in [traits.md](traits.md#23-associated-types-03): declarations,
per-impl definitions, bounds, Self and qualified projections, nested projections,
and declaration-time rigid checking. Concrete outputs normalize to existing types
before lowering; no new runtime representation is needed. Resolution rejects
cycles and ambiguity, and checks unused implementations. Equality constraints,
associated defaults, and generic associated types remain deferred.

Associated type tests include a manually polled future with String output,
generic impls, nested managed values, wide struct layouts, module aliases,
projection inference without changing borrow order, and invalid contracts.
The language server supplies completion, navigation, normalized signatures and
conservative workspace rename; the IntelliJ protocol checks exercise these over
stdio. See `examples/associated-types.rfl`.

Trait conformance and rejection cases live in `tests/traits.rs`, including the
spec's acceptance program, declaration-time bounds versus inherent methods,
nested managed printing, borrowed guards, modules/aliases/orphans, conservative
overlap unification, and C calls from traits on `#[repr(C)]` types. The complete
release suite passes 221 tests, including 21 trait tests and a small compiled
Brainfuck CPU execution test (16 October 2026).

## Remaining specification gaps

These are still unsupported; passing regression tests do not imply conformance.

| Requirement | Current behavior | Relevant implementation |
|---|---|---|
| `defer`, including cleanup on `?`, return, break, and continue | No defer AST or lowering | `src/ast.rs`, `src/parser.rs`, `src/lower/build.rs` |
| General reference places | Explicit field/index borrowing still requires a common place representation; safe named dereferences are implemented | `src/check/borrows.rs`, `src/check/ffi.rs` |
| Constructors as callable values | `let cons = Cons` is rejected for a payload constructor | `src/check/constructors.rs`, `src/check/control.rs` |
| Polymorphic immutable local callables | A local identity lambda used first on an integer cannot also be called on a boolean | `src/check/closures.rs`, `src/check/inference.rs` |
| Full unsigned 64-bit literal range | `18446744073709551615` is rejected because integer tokens store signed `i64` | `src/lexer.rs`, `src/ast.rs` |
| Machine limits hidden from source values | Fixed-array lengths, frame metadata, reference counts, and root-frame storage still have machine-width or stack constraints | `src/ty.rs`, `src/lower/{frames,ownership}.rs` |
| Source locations on runtime faults | Fault reasons are printed, but faults do not carry the original expression location | `src/typed.rs`, `src/lower/faults.rs` |
| Complete build/link CLI | Only `check`, `compile`, `run`, and `lsp` exist; documented `build --link` is absent | `src/main.rs`, `src/run.rs` |

## Follow-up order

1. Introduce a common place representation for names, fields, indexes, and
   dereferences, with explicit borrowing and write semantics.
2. Extend lifetime analysis beyond single-use temporaries and optimize checked
   collection accesses while preserving evaluation order and aliasing.
3. Unify scope exits and cleanup before implementing `defer`.
4. Design writer-oriented I/O and the Future/polling/executor contracts on top
   of static traits and associated types before introducing async syntax.
5. Complete generic local-value inference, numeric literal representation, and
   large-value storage; add conformance cases alongside each extension.

For code quality, replace argument-heavy checker/emitter helpers with focused
context and operation types. `cargo clippy -p rfl --all-targets --no-deps` still
fails on existing argument-count lints and IDE/LSP style issues; those lints were
not suppressed. Module loading for IDE analysis and compilation also still has
duplicated graph preparation, and checking C imports still constructs C IR.

## Validation

```sh
cargo test --release -p rfl -- --test-threads=4
cargo check -p rfl
```

The suite includes execution tests for the supplied functional BF example,
argument snapshots, ADT cleanup and frame churn, native and C calls, borrowed writes,
nested pattern coverage, fixed-array fields, character collections, string
helpers, real tail-call protocol, and original-file diagnostics. Optimization
regressions check that frame scalars reach SSA and repeated string operations
share helper bodies. Managed-collection tests cover ownership after return or
clear, COW, growth, get/pop, nested products, closures, borrowed-element rejection,
and receiver snapshots across an index expression with effects. Allocation-size
overflow is checked before adding buffer headers or multiplying element widths.
Reference counts also fail explicitly at their 16-bit limit instead of wrapping
and freeing live collection elements. That collection follow-up covered 140 passing
tests (3 library, 105 original compile tests, 24 regressions, and 8 managed-collection
tests).
