# RFL on the Brainfuck CPU

The RFL runner uses the same Ripple IR and ISA as RVM. `--target rbf`
selects the execution engine; it does not substitute a host implementation of
the RFL program. The linker exports the CPU and its initialized instruction
memory as Brainfuck, then RBF executes that code. `--jit` selects RBF's JIT.

```sh
target/release/rfl run rfl/examples/comprehensions.rfl --target rbf
target/release/rfl run rfl/examples/comprehensions.rfl --target rbf --jit
```

Lightweight telemetry goes to stderr. Compilation and image construction happen
before its execution counters start. BF dispatches and guest instructions are
different counters: one guest instruction requires many BF operations.

## October 2026 investigation

The recording-console greeting exposed two different costs.

* The first runner used `rlink -f macro`, followed by `bfm expand`. This builds
  and interprets a macro AST for each encoded instruction word. Expansion of the
  full greeting was stopped after roughly five minutes.
* The compiler also emits substantial guest code for the generic effect,
  environment, reactor, task and ownership machinery. Switching exporters
  preserves that program; it cannot remove its runtime work.

The baseline greeting had 398 IR function definitions, 49,792 IR instructions,
578,941 assembled instructions and 600,679 linked instructions. The final image
includes startup, runtime functions and bank layout. Linker collection removed
27 of the application's 403 assembly function entries, including unused device
bridges and drop helpers: dead-code collection is operating, but most of this
particular program remains reachable.

The existing compact linker exporter (`rlink -f bf --standalone --bf-host`)
built the same linked program in approximately three seconds and produced
70,194,294 bytes of BF. It streams instruction-memory initialization, caches
short wrapping/Horner constant builders, and expands only the shared CPU
template. It retains ordinary 16-bit BF execution and the same host hooks.
Its tests independently compare complete tape contents, hook ordering and
output against the macro route, including cross-bank entries and more than
65,535 initialized words.

The runner and shared RFL FP test helper now use this compact route. A CLI
regression bounds a small comprehension's actual exported image below 20 MB;
the old route produced about 283 MB for that fixture. The bound allows ordinary
compiler changes without relying on machine-dependent timing.

### First milestone: scalar frame reloads

RFL's synchronous calls use heap activation records and resumable entry points.
Scalar promotion gave each eligible slot a private local home, but first loaded
every such home from the heap, including temporary slots overwritten before
their first read. These initial reads survive generic optimization because it
conservatively preserves memory reads.

Frame liveness now separates two requirements:

* **Publish before a call/suspension:** preserve the old values needed by its
  continuation. The callee's destination is excluded because the callee writes
  it.
* **Reload on function entry:** retain the incoming values needed by the initial
  entry or any continuation. This includes parameters and callee-written results.

Temporary slots with no incoming value keep their local home without an initial
heap load. Borrowed/shared slots, partial scalar accesses, managed values and
return slots keep their existing rules. The focused regression first observed
five unnecessary heap loads in a tiny arithmetic function and requires zero.
Runtime tests check parameters, call results, loop state and captured values;
the async and resource suites cover suspension and destruction.

Measurements of the unchanged greeting after the reload fix:

| Metric | Before | After |
| --- | ---: | ---: |
| IR instructions | 49,792 | 49,126 |
| Assembled instructions | 578,941 | 572,960 |
| Linked instructions | 600,679 | 597,462 |
| Compact BF bytes | 70,194,294 | 69,784,509 |
| Executed guest instructions | 3,158,109 | 3,120,663 |

Both RVM runs completed with exactly `Hello, Ada!` and `Hello, Ripple!` on
separate lines. The instruction reduction is about 1.2%; it does not solve the
large abstraction costs. Timings collected while release tests were compiling
are not used as a runtime speedup claim. Compact export took roughly 3–6 seconds
across these measurements.

### Runtime baseline and remaining opportunities

Before the reload change, RVM completed both greeting lines in 3,158,109 guest
instructions. A bounded RBF JIT run reached 8,612 guest instructions and
274,828,716 optimized BF dispatches in 60 execution seconds, without completing
the greeting. This is a partial-run observation, not an estimate of total
runtime or a comparison of equally warmed execution phases.

The follow-up frontend work now includes:

* **Continuation-specific reload bridges.** The resume dispatcher selects a
  dedicated entry block which reloads only that continuation's live inputs.
  Internal branches and loop backedges target the ordinary segment blocks, so
  they cannot overwrite current local values with older heap values. Closure
  instructions also name their actual capture operands; creating one closure no
  longer makes every scalar slot appear live.
* **Conservative callback identities.** A small whole-program analysis follows
  function values through copies, moves, immutable captures and inline/ADT
  fields. Direct callers and async constructors can provide monomorphic callback
  parameters. Conflicting callers, control-flow joins, borrowed/shared values,
  opaque results and unknown invocation arguments retain dynamic behavior.
  Proven calls use constant function IDs and frame sizes, and ordinary named
  functions omit unused environment copies. This does not clone procedures or
  remove the CPS driver, allocation, ownership or cancellation protocols.
* **Grouped frame copies.** Disjoint inline descriptors and aggregates use
  I64/I32/I16 memory operations rather than a fresh pointer calculation for
  every cell. The shared backend's wide-memory cursor advances across banks;
  frames need no alignment or single-bank guarantee. Retains and releases still
  happen at the same ownership boundaries. Operations which require a complete
  snapshot before releasing an aliased destination retain their old ordering.
* **Pointer pairs stay pointers.** Borrowed-field access, borrowed vector headers
  and closure-environment loads read a stored fat pointer directly, avoiding
  two integer extensions, a shift, an OR and a conversion back to a pointer.
* **Balanced closure destruction.** The function-environment destructor uses the
  existing balanced ID dispatcher instead of a linear chain over all capturing
  functions. With 32 scalar-capturing callbacks, the maximum conditional depth
  falls from 34 to eight, including the null/refcount guards. Capture destruction
  and finalizer ordering are unchanged.

The follow-up comparison recompiles the saved pre-change IR and new IR using
one snapshot of `rcc`, `crt0` and `libruntime`. This avoids attributing concurrent
backend/runtime edits to the frontend. The source and the count of 398 IR
function definitions remain the same; generated instructions and metadata
access change.

| Metric | Matched control | Follow-up |
| --- | ---: | ---: |
| IR instructions | 49,126 | 45,280 |
| Assembled instructions | 572,960 | 545,225 |
| Linked instructions | 597,462 | 557,302 |
| Compact BF bytes | 69,784,509 | 66,388,911 |
| Executed guest instructions | 3,120,643 | 3,067,107 |
| Allocation sites with dynamic frame size | 48 | 26 |

Both full RVM runs print the same two greeting lines. Linked code falls by 6.7%,
the BF image by 4.9%, and executed guest instructions by 1.7%. The matched
control differs from the preceding historical runtime count by 20 instructions;
the table uses the matched control throughout. There is still substantial
activation-record and managed-value overhead, so these are incremental gains.
The 32-callback cleanup fixture separately falls from 217,033 to 215,178 guest
instructions with unchanged output.

A bounded JIT run of each image stops at 5,000 guest instruction starts. The
control executes 174,879,031 BF dispatches; the follow-up executes 166,402,641.
Neither completes the greeting. These are different program prefixes after
optimization, and the counts include tape initialization: do not interpret
them as a whole-program speedup. Wall times (31.4 and 44.3 seconds) were measured
under different concurrent release/test loads and are not a timing comparison.
The final run spends about 261 ms compiling its 37 JIT regions.

### Bounded calls and ownership transfers

The next frontend pass emits direct synchronous bodies for proven small acyclic
call chains. Callers reuse fixed frame/result scratch instead of allocating a
heap activation and returning through the driver. The closure ABI retains a
resumable wrapper, so unknown calls still work. Recursive cycles, suspension,
C re-entry and potentially callback-running destruction keep the conservative
path. Selection limits and ownership rules are described in
[lowering.md](../../rfl/lowering.md#bounded-synchronous-calls).

Whole-value moves and managed returns also transfer their existing share,
clearing the source owning pointers rather than retaining and releasing the
same share. Ordinary copies and shared capture storage preserve their previous
ownership protocol.

Using another single snapshot of the shared backend/runtime for both saved and
new IR gives:

| Metric | Previous frontend | Direct calls and transfers |
| --- | ---: | ---: |
| IR definitions (including compatibility wrappers) | 398 | 427 |
| IR instructions | 45,280 | 44,301 |
| Assembled instructions | 545,225 | 536,868 |
| Linked instructions | 557,302 | 539,550 |
| Compact BF bytes | 66,388,911 | 64,783,095 |
| Executed guest instructions | 3,067,112 | 2,916,820 |

The unchanged greeting completes with identical output on RVM. Executed guest
work falls by 4.9%, linked instructions by 3.2%, and the BF image by 2.4%.
Additional definitions are compatibility wrappers, not context-specialized
clones. Full greeting execution in BF remains deliberately bounded; these
counts do not establish its total BF runtime.

### BF transfer proofs

Profiling a small actual RFL export (`step(n) = n + 1`, called 32 times, then
printing `OK`) found repeated moving-transfer loops dominating dispatches.
Examples include `[>>>[->>>>>>>>+<<<<<<<<]>>>>>]` and a migrating countdown
carrying three adjacent values. These are ordinary BF patterns emitted by the
CPU's memory/register routines, not special RFL instructions.

The generic engine already batches these transfers. It now also records the
cleared source progression as an exact uniform-zero tape fact. This survives
eviction of the smaller predicate caches, so later traversals can reuse the
proof. The final live vector is outside the recorded range. Point writes split
the fact, bulk access materializes it, incompatible deferred lattices reject
it, and bounds/wrapping cases retain literal fallback. Both `rust-bf` and RBF,
with or without JIT, share this engine; it assumes no guest layout or opcode.

The same frozen BF image completes with `OK` and 5,929 guest instructions:

| Engine | Before BF dispatches | After BF dispatches |
| --- | ---: | ---: |
| Interpreter | 6,830,112 | 2,375,013 |
| JIT | 6,827,517 | 2,371,977 |

Three alternating repetitions reproduce these counts, a reduction of about
65%. A separately profiled 1,000-instruction prefix reduces moving-loop
dispatches from 790,563 to 19,677; ordinary block/affine counts are unchanged.
Maintenance dispatches rise slightly (942 to 1,079), accounting for the extra
bounded tape facts.

On the full greeting's **same post-frontend BF image**, bounded JIT runs stopping
at 3,000 guest instruction starts fall from 83,818,807 to 13,837,226 BF
dispatches (83.5%). Both stop at the intended instruction limit; neither
completes the greeting. Initialization is included, so this is evidence about
that prefix, not a whole-program speedup. Timing samples were collected with
other tests/user work active and are not an isolated wall-time comparison.
A deeper candidate run reaches 100,000 guest instructions and 48,911,349 BF
dispatches in approximately six execution seconds, again stopping at its
instruction limit rather than completing the greeting.

All five previously frozen C/assembly cases (`alu`, `gather`, `stack_calls`,
`wide_math`, `sort`) also pass in 20 before/after interpreter/JIT runs, with
matching output, return values and RVM guest instruction counts. Their
smaller-bank images show unchanged BF dispatch counts: this optimization helps
long reusable empty ranges, not every workload equally.

Remaining useful work:

1. **Go beyond callback identity.** Returned callback summaries and bounded
   context specialization could expose more targets. Eliminating a nonescaping
   closure/environment could remove additional allocations. Measure
   code size as well as executed work before introducing clones.
2. **Reduce managed lifetime traffic.** Effect plumbing still initializes,
   retains, clears and drops many temporary descriptors. Beyond whole-value
   transfers, lifetime analysis may remove redundant work, but must preserve aliases,
   reusable recipes and cancellation/finalizer order.
3. **Improve frame/pointer range information.** Generic offset calculations must
   handle bank crossings. Small offsets into a proven allocation could use
   cheaper calculations; assuming every heap frame fits one bank is incorrect.
4. **Broaden BF profiling after cleared-range reuse.** Investigate remaining
   interpreted arithmetic/seek work with bounded fixtures and exact guest results.
   The original partial run's 226 ms of JIT compilation did not account for its
   minute-long execution.
5. **Consider specializing the CPU template for immutable code.** Omitting
   unused handlers could reduce fixed image/parse costs, but requires an explicit
   immutable-code guarantee: STORC and device commands that load code can
   introduce instructions absent from the initial image.

These improvements require no additional ISA operations or host floating point.
Small literal-BF tests remain the portability check; RVM guest-instruction
counts help separate compiler work from BF engine work. Do not use a full Doom
run as the development benchmark.

## Verification

```sh
cargo test --release --jobs 2 -p rfl \
  --test direct_calls --test once_functions --test frame_promotion \
  --test callback_specialization --test run_brainfuck --test comprehensions \
  --test async_functions --test compile --test resources
cargo test --release --jobs 2 -p rfl \
  --test applicative --test effects --test reusable_effects
cargo test --release --jobs 2 --manifest-path src/ripple-asm/Cargo.toml \
  --test bf_compact
cargo test --release --jobs 2 --manifest-path src/rust-bf/Cargo.toml --lib
```

The core RFL selection includes native/RVM parity,
callee results and captured loop state across resumable calls, callback identity
joins, 1,600 nested frames crossing RAM banks, raw wide descriptor copies,
logarithmic destructor dispatch, deep async cancellation, resource cleanup on
the BF CPU, and actual CLI execution with plain/JIT RBF. Another 62 tests cover
applicative operations, consuming effects and reusable effects, including
factories, short-circuiting, owned captures and finalizer order.
The unchanged exporter selection contains six independent portability/validation
tests.
The direct-call suite adds nine cases covering selection limits, recursion and
unknown calls, wide/managed results, references, mutable captures, `?`, repeated
scratch reuse, finalizer order and actual BF CPU execution. Generic BF tests
compare long overlapping transfers and occupied holes to an independent
character interpreter, and check cache eviction, point mutation and checkpoint
state in both directions.
The final greeting's complete output was verified on RVM; its full BF execution
was intentionally bounded rather than awaited to completion.
