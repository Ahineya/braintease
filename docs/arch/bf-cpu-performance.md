# Brainfuck CPU arithmetic and tape paths

The RBF CPU remains ordinary Brainfuck with wrapping 16-bit cells. The native
BF engine accelerates BF algorithms; it does not decode or execute Ripple
instructions. The same improvements benefit `rust-bf`, RBF interpretation and
RBF's opt-in JIT. There are no new ISA instructions or host floating operations.

## What was expensive

Register operands lived beside the current instruction, while architectural
registers lived near the origin. Fetch/write loops repeatedly scanned between
these locations **once per unit of the transferred value**. LOAD/STORE also
extended their address prefix one cell at a time, repeatedly finding its end,
and moved values through similar scan loops. This made both instruction
location and numeric operand value affect the work.

Bitwise operations converted both operands with repeated *general* division by
two, operated on separate bits, then reconstructed the result by repeatedly
travelling to accumulate powers of two. Shifts converted to bits too, including
left shifts that only need doubling. Division counted through the numerator.
The BF CPU also disagreed with RVM on large shift counts and did not establish
the required zero-divisor behavior.

## Current algorithms

- Register selectors and values travel through temporary START cells using
  local affine transfers. The fast-lane sentinel protects the origin's run
  state. Pointer marks select registers without repeatedly returning to the
  instruction. Zero selectors, source values and results skip unnecessary
  transport. Source registers and other operand snapshots remain intact.
- RAM seeking moves three counters: bank, offset and within-bank distance.
  Bank multiplication never happens in a 16-bit cell, so physical addresses
  can exceed 65,535 words. PTR ends one word after the target, retaining an
  origin sentinel even for address zero. Read copies preserve RAM; writes
  complete before their single host callback. Code/register lanes stay intact.
- Bit extraction uses a nearby four-cell halving state machine. Reconstruction
  uses `result = 2*result + next_bit`, with local affine transfers.
- Left shifts double directly. Right shifts halve directly. Both first reduce
  the count to its low four bits, as RVM does, for register and immediate forms.
- Unsigned DIV/MOD use restoring binary long division. A separate seventeenth
  remainder bit prevents overflow for divisors at or above 32,768. Both return
  zero for a zero divisor. Extraction still has numeric cost on a literal BF
  interpreter; the division itself uses bounded bit operations.

`src/ripple-asm/src/bf_cpu/arithmetic.rs` and `memory.rs` generate these portable
routines at export time. The footer connects them to the existing operand and
result convention. Their temporaries fit inside the existing reserved tail;
REG/CODE/RAM lanes are not borrowed by arithmetic. The superseded conversion,
bit and memory macros have been removed from the footer.

## Why the native engine also needed changes

A shorter literal tape path does not automatically improve an optimized BF
engine. Its old routed-loop proofs could collapse the earlier long transfers,
while a new moving counter initially executed one hop at a time. Benchmarks
caught this regression before it became the final implementation.

Two generic guarded summaries bridge that gap:

1. **Moving state:** derive affine transfers, state ordering, direction and an
   optional marker from the BF body. Prove untouched storage clear and scan
   predicates independent. Move the state directly across a proven prefix;
   otherwise retain the literal operations. Cached facts and bounded cold
   probes avoid unbounded work per dispatch. Occupied destinations, tape edges,
   wrapping aliases, host mutations and budget boundaries remain observable.
2. **Halving:** recognize the four-cell finite-state loop at arbitrary spacing
   and in either direction. For its verified remainder/flag states, compute its
   exact closed form. Other states use the original BF. This turns repeated
   bit extraction into a few summarized operations, without teaching the
   interpreter anything about a Ripple register or instruction.

These summaries also work in standalone `rust-bf`. JIT runs share the summaries;
not every summary is emitted as native machine instructions. JIT compilation
still costs time and may lose on small workloads.

Portability does not imply identical complexity on every BF interpreter. In
particular, moving a unary counter along a long path can still do quadratic
literal work. The native engine can prove and summarize that movement. Neither
BF dispatch counts nor these small benchmarks predict full Doom boot time.

## Validation and measurement

The assembler's independent BF evaluators exercise plain/host exports, strict
bounds, exact hook placement, source preservation, register aliases, full-width
values and memory paths crossing the 65,535-word boundary. Local arithmetic
oracles check bit/division cases, deterministic samples and literal BF execution
while filling every protected lane with nonzero sentinels.

The BF engine compares new summaries with a character-by-character oracle,
including reversed/scaled loops, occupied destinations, malformed shapes,
wrapping, bounds faults, pauses, host mutation and checkpoint restoration. The
halving oracle exhausts every 16-bit input and both valid remainder states.
RBF differential tests compare all registers, ordinary RAM and loaded code with
RVM, plus exact interpreter/JIT tapes and paused/resumed execution.

Use the [checked C/assembly suite](../../rbf/benchmarks/jit/README.md) to freeze
matching compiled guests and compare CPU exports and host versions. Keep builds
outside timing runs. Source/image/binary hashes, raw reports and cold JIT costs
are recorded. No full Doom run is part of these checks.


## Measured results, 2026-10-16

Release builds on macOS ARM64; three serial alternating repetitions per mode,
with cold processes and no builds/tests from this task during measurement.
Both CPU exports use the same frozen guest objects/binaries and bank settings.
The baseline host was rebuilt from the original BF engine sources with the
same release profile; its dispatch counts matched the initially saved baseline
measurements in all ten case/mode combinations. All checksums, guest instruction
counts and stable host report fields matched.

Host execution medians in seconds (including initialization and cold JIT
compilation, excluding BF parsing and host setup):

| Case | Interpreter before | Interpreter after | JIT before | JIT after |
| --- | ---: | ---: | ---: | ---: |
| ALU | 0.732 | 0.100 | 0.723 | 0.071 |
| Gather | 0.992 | 0.151 | 1.013 | 0.186 |
| Stack calls | 0.834 | 0.077 | 0.902 | 0.166 |
| Wide math | 1.348 | 0.016 | 1.440 | 0.064 |
| Sort | 0.278 | 0.251 | 0.271 | 0.267 |

The stack-call and wide-math baselines varied noticeably: interpreter ranges
were 0.535–0.835 s and 1.200–1.589 s, respectively. Other user work may have been
active; treat these timings as observations, not precise universal speedups.
The roughly 1.5% JIT sorting difference is too small for a strong speed claim.

Interpreter dispatch counts provide deterministic evidence of reduced work:

| Case | Before | After |
| --- | ---: | ---: |
| ALU | 127,410,295 | 15,698,116 |
| Gather | 131,799,308 | 10,039,844 |
| Stack calls | 75,930,150 | 5,497,628 |
| Wide math | 186,807,227 | 1,066,444 |
| Sort | 12,803,582 | 12,240,571 |

The more explicit binary-division implementation increases the expanded CPU
size (these images grew from about 4.0 to 7.3 MiB). Parsing is outside the host
execution timer. Complete process medians still improved substantially in four
cases; wide math improved from 1.377 to 0.067 s in the interpreter. Sorting's
interpreter process time barely changed (0.303 → 0.299 s), and its cold JIT
process regressed slightly (0.297 → 0.314 s). Reducing exported microcode size
and cold parse/JIT costs remains useful; these are not steady-state regressions
hidden by the execution timer.

A separate 13-instruction kernel combines full-width register copies, XOR,
unsigned division, shifting and repeated RAM access with 64-cell banks and four
RAM banks. It returns checksum 22105 in RV0:

```asm
start:
LI T0, 65535
MOVE S0, T0
XORI S1, T0, 43690
DIVI S2, T0, 257
SRLI S3, T0, 13
LI T1, 127
STORE T0, R0, T1
LOAD RV0, R0, T1
ADD RV0, RV0, S0
ADD RV0, RV0, S1
ADD RV0, RV0, S2
ADD RV0, RV0, S3
HALT
```

Running both exports with the same host and `--no-optimize` reduced dispatches
from **645,955,356 to 482,422,222** (25.3%). Observed times were 5.15 → 3.88 s,
with other checks active, so the counts are the stronger evidence. This mode
still coalesces adjacent additions/pointer moves; these are unoptimized IR
counts, not counts of individual BF characters. The independent character
oracles establish portability separately.

Raw comparison reports, manifests, source/image/binary hashes and profiles are
under ignored `build/bf-cpu-opt/final/`; the literal-kernel evidence is in
`build/bf-cpu-opt/literal/`. These generated artifacts are not required to rebuild
the test suites. Regenerate existing BF exports with the updated linker to use
the new CPU routines; an old BF file retains its old algorithms.
