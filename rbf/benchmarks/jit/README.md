# Checked BF CPU workloads

These small programs complement Mandelbrot and the four-square graphics demo.
They exercise the actual BF CPU, with the unchanged ISA and 16-bit wrapping
cells. No Doom build or run is involved.

| Case | Source | What it exercises | Expected checksum | Guest instructions |
| --- | --- | --- | ---: | ---: |
| `alu` | Assembly | High-bit word multiplication, positive signed DIV/MOD, XOR and shifts | 33804 | 713 |
| `gather` | C | Permuted volatile array reads and writes | 25280 | 27147 |
| `stack_calls` | C | Recursive calls, volatile local arrays and stack frames | 62096 | 14333 |
| `wide_math` | C | Unsigned 32-bit shifts, XOR, addition and multiplication | 52319 | 2063 |
| `sort` | C | Insertion sort, comparisons and overlapping array moves | 25056 | 28280 |
| `masked_math` | C | TCP byte-pattern arithmetic with discarded upper words | 4368 | 1411 |

Every program checks its result, prints exactly `Y` and returns zero on success;
it prints `N` and returns one on failure. Sorting also checks ordering. Volatile
storage keeps the intended memory/call work observable. Checksums were calculated
independently of RVM and BF. `alu` limits division operands to the positive domain
so it does not depend on unsigned-versus-signed division lowering.

## Build, freeze, then measure

From the repository root, using Python 3.11 or newer:

```sh
python3 rbf/benchmarks/jit/run.py --build
python3 rbf/benchmarks/jit/run.py --profile
```

Build mode prepares release tools, expanded BF and matching RVM binaries under
ignored `build/bf-jit-cases/`, then exits. C uses 2048-cell banks and assembly uses
512-cell banks; all cases use four RAM banks. A manifest records source/image
SHA256 values, bank settings and static assembly opcode counts. Measurement
refuses changed sources/images. Rebuild explicitly after changing a fixture.

Measurement never builds. Keep builds and other timing workloads idle. It runs
RVM first, then alternates interpreter/JIT order across three cold processes per
mode. It compares stdout, successful checksums, guest return values, guest
instruction counts and stable BF host report fields. BF dispatch counts are
recorded separately because different derived scan facts can change them.
This is end-to-end regression coverage, not a full per-instruction BF/RVM
lockstep comparison; core differential tests cover exact intermediate tape state.

Each run has a 20-second execution limit plus an external process deadline,
a one-million guest instruction ceiling and a 500-million BF-dispatch ceiling.
Timeouts, nonzero exits and mismatches fail explicitly. `--case sort`,
`--repetitions 1`, `--timeout 30`, `--inputs`, `--output`, `--rbf` and `--rvm`
select cases, bounds and paths. `--build` always builds all six cases.
A private empty disk is passed to RVM; these fixtures do not access storage.

To compare a saved older host with a candidate:

```sh
python3 rbf/benchmarks/jit/run.py \
  --baseline build/bf-transfers/before/rbf \
  --output build/bf-transfers/comparison --profile
```

All versions run identical frozen BF images. Results include both host execution
time (initialization and callbacks, excluding parsing/setup) and complete process
time. Reports, commands, stdout/stderr, binary hashes and platform metadata are
retained in the output directory. Compilation is included in cold JIT timings.

`--profile` makes separate, untimed interpreter runs. Each `.summary.json` records
operation totals and the twelve most expensive retained navigation sites, with
bounded BF loop snippets compressed as `>8` for eight right moves. It never dumps
an entire expanded BF image. Site eviction counters remain visible. These
instrumented operation times include profiler overhead and are not ordinary
wall-time percentages. The static assembly histogram describes compiled assembly
instructions, not a dynamic guest opcode profile.

## Guarded-transfer results, 2026-10-15

Three serial alternating repetitions on macOS 27 ARM64, release builds with
rustc 1.97.1; no concurrent builds or benchmarks. The candidate adds the small
single-body transfer proof shared by both executors. Bank settings, C/assembly
sources and BF images are fixed within every comparison. Times are medians of
host execution, in seconds:

| Case | Before interpreter | After interpreter | Before JIT | After JIT |
| --- | ---: | ---: | ---: | ---: |
| `alu` | 0.769 | 0.744 | 0.737 | 0.723 |
| `gather` | 1.090 | 0.994 | 1.116 | 1.022 |
| `stack_calls` | 0.590 | 0.536 | 0.704 | 0.645 |
| `wide_math` | 1.177 | 1.150 | 1.245 | 1.217 |
| `sort` | 0.398 | 0.277 | 0.381 | 0.272 |

Sorting improves by about 30% in execution time. The roughly 2–3% differences in
arithmetic-heavy cases are small enough to treat cautiously. All checks and
guest instruction counts matched. Raw evidence is under ignored
`build/bf-transfers/final-cases/`; files there are not needed to rebuild the suite.

The profiles expose different remaining costs:

- `alu`, `gather`, `stack_calls` and `wide_math` repeatedly reach moving updates
  shaped like `[>8+>16]`, plus scans `[<8]` and `[>8]`. The first shape increments
  a cell eight positions ahead while moving its predicate by 24 positions.
  The same small algorithms run millions of times. These are observed BF
  patterns; the interpreter has no CPU lane constants.
- In `wide_math`, 2063 guest instructions become about **187 million BF IR
  dispatches**, with only **0.14%** native. Guarded routes account for about 0.3%
  of instrumented operation time. More transfer optimization cannot explain a
  large win here. Integrating bounded moving-update summaries with native
  regions, and inspecting the CPU's arithmetic algorithms, are better targets.
- JIT compilation costs roughly 45–100 ms across these cases. Most compiled
  arithmetic regions retire in the memory/call cases; device barriers and
  Rust-only navigation also limit native coverage. JIT still loses to the
  interpreter on gathering, recursive calls and wide math.
- `sort` still spends about 42% of instrumented operation time in routed loops,
  so broader route plans may help it. This percentage includes profiling cost
  and does not estimate a corresponding wall-time speedup.

The four-square comparison and exact input/pixel checks are documented in the
[parent benchmark guide](../README.md#guarded-transfer-specialization-2026-10-15).


## Masked arithmetic compiler result, 2026-10-16

`masked_math.c` reproduces the TCP echo demo's `(offset * 37UL + 11UL) % 256UL`
expression with volatile input and 32 checked iterations. The compiler used to
compute the whole 32-bit product before discarding its high bits. The low-word
mask pass instead computes single-use add/subtract/multiply/bitwise expressions
in 16 bits and zero-extends the masked result to its original IR type. Shared
wide users, division and right shifts retain their full-width operation.

The before/after **compiler** comparison uses different generated BF images
with the same BF host and source, 2048-cell banks and four RAM banks:

| Counter | Before | After |
| --- | ---: | ---: |
| Executed guest instructions | 3098 | 1411 |
| BF interpreter dispatches | 50,887,077 | 22,075,610 |
| BF JIT dispatches | 50,887,158 | 22,075,656 |

Three serial alternating repetitions matched each counter and printed `Y`.
Dispatches fall by about 57%. Timing observations were roughly 0.63 → 0.27 s
for the interpreter and 0.79 → 0.36 s for JIT, but other user work was active;
use the instruction/dispatch counts as the firm evidence, not those timings as
an isolated speed claim. This is a small arithmetic workload, not a measured
speedup for the complete DHCP/TCP exchange.

To build and check the current case with the normal bounded runner:

```sh
python3 rbf/benchmarks/jit/run.py --build
python3 rbf/benchmarks/jit/run.py --case masked_math
```

The runner's `--baseline` option compares BF host binaries on one frozen image;
it is not a compiler-before/after switch. The compiler comparison preserved
separate before/after images and reports under ignored
`build/network/tcp-analysis/bench/`.


## Comparing BF CPU implementations

`--baseline-inputs` compares separately frozen BF exports of the **same guest**.
It checks matching source/binary hashes and bank settings, and validates each
BF image against its own manifest. This complements `--baseline`, which selects
the older host executable. Without `--baseline`, both exports use the current
host; without `--baseline-inputs`, both hosts use the same BF export.

```sh
python3 rbf/benchmarks/jit/run.py \
  --inputs build/bf-cpu-opt/after \
  --baseline-inputs build/bf-cpu-opt/before \
  --rbf build/bf-cpu-opt/after/rbf \
  --baseline build/bf-cpu-opt/before/rbf \
  --output build/bf-cpu-opt/final
```

Freeze matching `.pobj`/`.bin` files before changing the CPU, then export those
same objects with each linker. Preserve both manifests and host binaries. The
runner requires identical guest instruction counts and successful checksums;
BF source hashes are expected to differ and remain recorded in the reports.
See the [CPU performance notes](../../../docs/arch/bf-cpu-performance.md).
