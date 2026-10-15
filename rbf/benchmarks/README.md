# Brainfuck CPU benchmarks

These small terminating programs exercise generic Brainfuck interpreter costs
without running Doom. Each succeeds by writing `Y` to TTY and returning RV0=0.

| Source | Bank size | RAM banks | Work |
| --- | ---: | ---: | --- |
| `late-code.asm` | 256 | 1 | A short loop after eight banks of skipped code |
| `far-ram.asm` | 512 | 16 | Sixteen STORE/LOAD pairs at the last RAM word |
| `far-ram-64k.asm` | 64000 | 256 | Sixty-four STORE/LOAD pairs crossing banks 136–137, plus the final RAM word |
| `bank-jumps.asm` | 257 | 1 | Thirty-two calls to a distant code bank and returns |
| `far-call-64k.asm` | 64000 | 256 | Sixteen calls to code bank 30 interleaved with verified RAM-bank-136 reads and writes |

The assembler has no instruction repetition directive. `generate.mjs` expands
the explicitly marked padding in these templates into ordinary assembly, keeping
thousands of identical NOP lines out of the maintained sources. Always generate
the templates before assembling them.

From the repository root, with the release assembler/linker/macro tools built:

```sh
node rbf/benchmarks/generate.mjs build/bf-bench/inputs
BANK_SIZE=256 MEMORY_BANKS=1 scripts/build-bf.sh build/bf-bench/inputs/late-code.asm build/bf-bench/inputs/late-code
BANK_SIZE=512 MEMORY_BANKS=16 scripts/build-bf.sh build/bf-bench/inputs/far-ram.asm build/bf-bench/inputs/far-ram
BANK_SIZE=64000 MEMORY_BANKS=256 scripts/build-bf.sh build/bf-bench/inputs/far-ram-64k.asm build/bf-bench/inputs/far-ram-64k
BANK_SIZE=257 MEMORY_BANKS=1 scripts/build-bf.sh build/bf-bench/inputs/bank-jumps.asm build/bf-bench/inputs/bank-jumps
```

The combined `far-call-64k` fixture has 1,926,005 instructions, almost all skipped
NOP padding. Use the linker's direct BF exporter to avoid expanding millions of
individual instruction macros:

```sh
src/ripple-asm/target/release/rasm assemble build/bf-bench/inputs/far-call-64k.asm \
  --bank-size 64000 -o build/bf-bench/inputs/far-call-64k.pobj
src/ripple-asm/target/release/rlink build/bf-bench/inputs/far-call-64k.pobj \
  -f bf --standalone --bf-host --bank-size 64000 --memory-banks 256 \
  -o build/bf-bench/inputs/far-call-64k.bf
```

Keep those exact generated `.bf` files for both interpreter versions. Rebuilding
the CPU footer between measurements would also measure changes to the program.

## Native BF JIT comparison

Build first, then keep builds and other timing workloads idle while measuring:

```sh
cargo build --release --manifest-path src/rust-bf/Cargo.toml
cargo build --release -p rbf --example check_squares_jit
cargo build --release -p rbf
scripts/run-bf-rgb565-c.sh --build-only
python3 rbf/benchmarks/benchmark_jit.py --squares build/bf/rgb565-demo.bf
target/release/examples/check_squares_jit build/bf/rgb565-demo.bf
```

The Python driver runs Mandelbrot and optionally the original C four-square
demo, alternating interpreter/JIT order for three repetitions. Each process
starts cold, including native compilation. `--repetitions`, `--timeout`, `--frames`
(default four), `--output`, `--bf` and `--rbf` select bounds/artifacts/binaries.
It never builds anything or selects Doom. Mandelbrot has an external process
deadline; square runs additionally have the host execution deadline. Timeouts,
nonzero exits, differing stdout, pixels or stable execution-report fields fail
explicitly. BF dispatch counts may differ because native execution can change
derived scan facts. Reports, stderr and captures remain under `build/bf-jit/`.

The BF CLI timer includes parsing and execution. The RBF execution timer
includes tape initialization, native compilation and host callbacks, excluding
parsing and host setup. Process time is recorded separately. Exact source,
report and frame hashes make the frozen input explicit.
The frame comparison uses no keys. The separate Rust example runs five frames
with Right, Down, Left and Up held in turn, checking exact full tape, front
pixels, stdout and guest instruction counts against the interpreter. It also
independently checks the red player's expected coordinates. This legacy demo
never requests NEXT_TICK, so tick-script replay cannot test its arrow movement;
the checker changes live held-key state at frame boundaries without a window.
Its repeated host runs intentionally warm fresh JIT caches and are correctness
checks, not steady-state performance measurements.

### Initial native JIT results (2026-10-15)

Three alternating, sequential pairs on macOS 27 ARM64 with rustc 1.97.1,
using the same release executable for both modes and no concurrent builds or
timing workloads. Every JIT process started cold; compilation is included.

| Workload | Interpreter median | JIT median | Result |
| --- | ---: | ---: | --- |
| Classic `mandelbrot.bf` | 5.621 s | 2.399 s | 2.34× faster |
| Original C four-square demo, four frames | 1.406 s | 1.686 s | 19.9% slower |

Mandelbrot uses the standalone CLI timer (parsing plus execution); its ranges
were 5.612–5.645 s and 2.388–2.400 s. The square demo uses the host execution
timer (including tape initialization, excluding parsing/setup); ranges were
1.400–1.408 s and 1.677–1.740 s. The demo retains three 16,384-cell RAM banks,
a 393,480-cell tape and 119,440 observed guest instructions. All compared
stdout, stable report fields and exact frame pixels matched. Its fourth frame
also matched the corresponding RVM binary, and the five-frame arrow checker
matched complete tape state and expected player positions.

Mandelbrot executes about 92% of its 1.326 billion IR dispatches natively.
The square demo executes only about 8% natively: 43 of 55 compiled regions
retire after short runs. Compilation costs roughly 0.1 s in each workload.
Native entry, cell preparation and scan-fact maintenance remain overhead for
the short arithmetic regions between the BF CPU's summarized algorithms.

A separate, instrumented first-frame profile attributes about half of the
**timed operation cost** to guarded routed loops (1.62 million attempts, just
275 fallbacks). This includes profiler overhead and is not an unprofiled wall
time breakdown. These loops already replace large BF traversals with bounded
Rust proofs and deferred updates; lowering arithmetic alone does not improve
them. Compiling/specializing those summaries and reducing transitions across
them is the next useful JIT target. Preserving their scan and deferred-write
algorithms matters more than removing dispatch in isolation.

At this baseline, JIT remains opt-in, with `rbf --jit` explicitly experimental.
No Doom run or Doom speed estimate was made. Frozen binaries and their hashes
are under ignored `build/bf-jit/final-bin/`; commands, hashes, raw measurements
and captures are under `build/bf-jit/final-measured/`. Source SHA256 values:

- Mandelbrot: `dc634e6a22d7bfa260c827e021fa47485b25e6cd736b7e4e476b550e6827ebdd`
- Square BF image: `210c35f844c454c664e7f11766b47127ce96230e3e037afab6e66eaaeed62ff8`

### Guarded-transfer specialization (2026-10-15)

The next change gives two-write routes a smaller proof when the actual scan
strides establish disjoint counter, destination and subsequent scan lanes.
The destination must be a scanned zero exit. Such a body changes that earlier
sentinel, so the general multi-iteration proof could only accept one iteration
anyway. Parsing now certifies independence once; execution traces the scans,
checks all pointer excursions and the return to the counter, then commits two
writes. Cold scans, aliases and failed guards still use retained BF instructions.
No Ripple opcode, CPU lane constant or physical-tape window is involved.

Three serial alternating repetitions on the same machine and frozen four-square
image as above, including cold JIT compilation:

| Four frames | Before median | After median | Execution-time reduction |
| --- | ---: | ---: | ---: |
| Interpreter | 1.405 s | 0.924 s | 34.2% |
| JIT | 1.681 s | 1.168 s | 30.5% |

Interpreter ranges were 1.401–1.431 s before and 0.923–0.927 s after; JIT ranges
were 1.680–1.682 s before and 1.167–1.172 s after. All stable report fields,
stdout, 119,440 guest instruction starts and exact fourth-frame pixels matched.
BF dispatches remained 64,341,508 in the interpreter and 64,353,197 in JIT mode.
The five-frame arrow checker matched full logical tape, pixels, output and
expected player positions. A checkpoint written by the baseline host also
resumed to the same first frame in both candidate modes.
JIT still loses to the interpreter on this workload. Both benefit from the same
small Rust execution plan; this change does not add native lowering for scans.

A separate native-backedge experiment called the certified Rust summary from
generated code. Its square median was 1.161 s versus 1.168 s without that wrapper,
a difference below 1%; it also made the recursive-call fixture slower. That
backend was left out. Further JIT work should lower useful route/update work
and preserve logical tape facts, rather than merely compile a callback loop.

The [new C/assembly suite](jit/README.md) covers five checked, bounded workloads
and records operation profiles plus compressed hot-loop shapes. Sorting improves
from 0.398 s to 0.277 s in the interpreter; arithmetic-heavy cases barely change.
The profiles identify millions of moving updates and tiny scans as a separate
remaining cost. Saved binaries and comparison evidence are under ignored
`build/bf-transfers/`, with final case reports in `final-cases/` and square reports
in `final-squares/`. No Doom run or Doom speed estimate was made.

## Core replay with exact state comparison

Build and preserve the benchmark binary **before** changing the interpreter:

```sh
cargo build --release --manifest-path src/rust-bf/Cargo.toml --example bench_replay
mkdir -p build/bf-bench/before
cp src/rust-bf/target/release/examples/bench_replay build/bf-bench/before/bench_replay
```

After changing the interpreter, repeat with `after` in place of `before`. Use
the same build profile and compiler flags for both. For each input, run:

```sh
build/bf-bench/before/bench_replay build/bf-bench/inputs/far-ram.bf build/bf-bench/before/far-ram 5
build/bf-bench/after/bench_replay build/bf-bench/inputs/far-ram.bf build/bf-bench/after/far-ram 5
cmp build/bf-bench/before/far-ram.trace build/bf-bench/after/far-ram.trace
```

For careful timing, alternate before/after order across several invocations and
keep other builds or benchmarks idle. The JSON reports median/min/max execution
time, optimized dispatches, parsing time, and event count. Execution timing
includes machine initialization and event collection; parsing, trace generation,
file I/O, and comparisons are outside it. The driver checks a twenty-second limit
and a three-billion-dispatch limit between slices of at most one million
dispatches. Dispatch counts are internal interpreter work and can change with
optimization.

The untimed `.trace` records the pointer and exact complete tape at every input,
output, host hook, and final halt. It stores changes since the previous snapshot,
so byte comparison checks all intermediate observable states without relying on
a hash. Budget yields are deliberately excluded because optimizations can change
their placement. Timed repetitions also check final tape, pointer, and events.

This replay observes host hooks but does **not** emulate devices or write host
responses into the tape. These fixtures require no such responses. Use the real
host measurement below to include device handling and cache invalidation.

## Real host measurement

Preserve `target/release/rbf` before and after interpreter changes. Then run each
saved binary against the same input, for example:

```sh
build/bf-bench/before/rbf build/bf-bench/inputs/far-ram.bf --headless --timeout 20 --exit-code --report build/bf-bench/before/far-ram-host.json
build/bf-bench/after/rbf build/bf-bench/inputs/far-ram.bf --headless --timeout 20 --exit-code --report build/bf-bench/after/far-ram-host.json
```

Both runs must print `Y`, exit successfully, and report RV0=0 with identical
Ripple instruction counts. `elapsed_seconds` measures execution including guest
tape initialization and host device callbacks; it excludes parsing and host
setup. Compare captures as well when using display fixtures. Keep generated
programs, executables, reports, and traces under ignored `build/` directories.

The large `far-ram-64k` fixture uses the real host path rather than the core
replay's full-tape trace. Its 131,072,264-cell tape exceeds that driver's cap:

```sh
target/release/rbf build/bf-bench/inputs/far-ram-64k.bf --headless --timeout 30 \
  --max-tape-cells 131072264 --exit-code --report build/bf-bench/far-ram-64k.json
```

It checks each loaded value, including 16-bit arithmetic wraparound and a RAM
bank crossing, before emitting `Y`. Keep its banks and memory allocation fixed
when comparing interpreter versions.

The combined fixture uses the same tape limit and succeeds after 267 guest
instructions. Its leaf worker is at code bank 30, offset 6000; both sides of each
call read and write RAM bank 136. This exercises interactions between code and
RAM traversals. Its elapsed time also includes initializing the skipped code,
so report it as a complete fixture measurement, not isolated call latency.

```sh
target/release/rbf build/bf-bench/inputs/far-call-64k.bf --headless --timeout 30 \
  --max-tape-cells 131072264 --exit-code --report build/bf-bench/far-call-64k.json
```

## Measured optimizer changes

Measured on 2026-10-12 against baseline commit `dcb4ce4`, using an Apple M4
(10 logical CPUs, 24 GiB RAM), macOS 27.0 build 26A428, and rustc 1.97.1
(`aarch64-apple-darwin`, LLVM 22.1.6). The candidate adds generic cached zero
scans, nonzero-marker searches, and bounded arithmetic walks. Saved release
libraries were linked into identical benchmark drivers with:

```sh
rustc --edition=2021 -O -C panic=abort -C lto=fat src/rust-bf/examples/bench_replay.rs --extern bf_wasm=RELEASE_RLIB -L dependency=src/rust-bf/target/release/deps -o OUTPUT
```

Core figures are medians of three independent timed runs. Execution order was
baseline/candidate, then reversed, then forward again. Each timed run followed
an untimed trace run. Other builds and timing workloads were idle. All six
candidate traces matched the baseline byte for byte.

| Core fixture | Baseline ms | Candidate ms | Speedup | Baseline → candidate dispatches |
| --- | ---: | ---: | ---: | ---: |
| Legacy RGB565 | 67.531 | 15.061 | 4.48× | 24,960,953 → 3,611,101 |
| Multibank | 5.406 | 0.574 | 9.42× | 801,865 → 118,221 |
| Tiny compiled C | 14.637 | 1.659 | 8.82× | 2,365,155 → 313,369 |
| Late code | 947.681 | 4.052 | 233.91× | 14,071,518 → 651,830 |
| Far RAM | 1,437.186 | 14.819 | 96.98× | 16,278,773 → 3,435,871 |
| Bank jumps | 1,931.305 | 19.861 | 97.24× | 52,632,913 → 3,238,077 |

The first three are existing `rbf/tests/fixtures` programs, built with the bank
settings in `scripts/test-bf-host.sh`. The remaining inputs come from this
directory. Each before/after pair used the exact same expanded BF file.

Actual host results below include device handling and the scan-cache invalidation
caused by host tape access. These are medians of five runs, alternating baseline
and candidate order, using saved release `rbf` executables. All stable report
fields, stdout, guest results, and the legacy RGB565 capture matched. Legacy
stops after the first captured frame; the other programs run to HALT.

| Real host fixture | Ripple instructions | Baseline ms | Candidate ms | Speedup |
| --- | ---: | ---: | ---: | ---: |
| Legacy RGB565 capture | 15 | 65.823 | 16.011 | 4.11× |
| Tiny compiled C | 118 | 14.729 | 1.793 | 8.22× |
| Far RAM | 88 | 1,438.114 | 17.175 | 83.73× |

The distant-code and RAM fixtures deliberately amplify scan costs. These are
small CPU benchmark results, **not estimates of Doom speed or general BF
performance**. Submillisecond timings are especially sensitive to host noise.
The saved binaries, input hashes, exact traces, and raw measurements for this
run are under ignored `build/bf-optimizer-perf/`; their presence is not required
to regenerate the fixtures or run the benchmark.

### Cached marker fills and updates (2026-10-13)

A later interpreter-only change reuses verified zero runs when filling markers
and verified nonzero runs when updating a moving loop's current cells. Both
paths still materialize every write and preserve the original bounds excursions,
wraparound behavior and host-mutation barriers. No CPU, compiler or ISA change
is included in this comparison.

Two pairs, in before/after then after/before order, executed the same frozen
Doom BF image through 300,000 guest instruction starts. Every run used 64,000-cell
banks, 256 RAM banks, unthrottled headless mode, no profiler, and a 90-second
timeout in addition to the instruction limit. Execution times include tape
initialization and exclude source parsing and host setup:

| Version | First run | Reversed-order run | Median |
| --- | ---: | ---: | ---: |
| Before | 23.950 s | 23.927 s | 23.938 s |
| Cached fills/updates | 15.981 s | 16.221 s | 16.101 s |

This prefix was **1.49× faster (32.7% less execution time)**. Both versions
performed 262,862,555 BF dispatches, and every stable execution-report field
matched. Reaching the requested instruction limit intentionally returns a
nonzero status. The source SHA256 was
`f265975aa96b03496d14e846a51b6f2a243184695b1fdfb9c72820d57fd9c86c`.
The small late-code, far-RAM and cross-bank-call fixtures also produced identical
full tape/event traces. A separate 300,000-instruction comparison against the
Rust VM checked every register, ordinary RAM store and final RAM contents.
The small fixtures' submillisecond-to-millisecond timings were noisy;
the reported speedup is specific to the larger fixed startup prefix, not a
general BF or full Doom boot estimate. Saved binaries, reports and traces are
under ignored `build/bf-traversal-perf/`.

### Deferred uniform tape runs (2026-10-13)

The generic engine can represent proven constant progressions without eagerly
writing each cell. Fill/cleanup cycles can cancel when they restore known backing
values; retaining clean facts also avoids repeated scans. Point observations
remain exact, and bounded materialization handles storage pressure. The CPU and
ISA are unchanged.

Three alternating before/after pairs ran the same frozen `far-ram-64k.bf` with
release `rbf` binaries, a 30-second timeout and no concurrent timing workloads.
Both versions printed `Y`, returned RV0=0 and executed 528 guest instructions:

| Real host fixture | Before median | After median | Speedup |
| --- | ---: | ---: | ---: |
| Far RAM, 64,000-cell banks × 256 | 1.455 s | 0.0298 s | 48.9× |

BF dispatches fell from 10,443,018 to 316,268. The source SHA256 was
`fd2dda6edb2db79e111fa8b9926af84159e9d293dd26868a835d82315f344823`.
These are execution times including tape initialization and device callbacks,
excluding parsing and host setup. This deliberately amplified far-access test
does not predict Doom's overall speed. Reports and saved binaries are under
ignored `build/bf-far-access/`.

### Uniform proofs reused by scans and searches (2026-10-13)

Scans and moving-marker searches now recover zero/nonzero facts directly from
represented uniform tape runs when their small predicate cache has lost them.
Compatible larger strides can reuse a subset of the same progression. The
original sentinel and pointer-excursion checks remain in place; no Ripple
instruction, register layout or Doom behavior is recognized by the interpreter.

A saved release host from **before deferred tape runs** and the final host ran
one sequential comparison on the exact same frozen Doom BF image and staged
disk. The final host includes deferred runs, point-access filters and this scan
reuse; this comparison does **not** isolate the latest scan change. Both used
64,000-cell banks, 256 RAM banks, headless unthrottled execution, seed 1, telemetry
and no profiler. Each stopped at 1,000,000 observed guest instruction starts,
with an additional 240-second execution timeout and a 270-second external bound.
No other build or benchmark ran concurrently during these two timing runs.

| Version | Execution time | BF dispatches |
| --- | ---: | ---: |
| Saved host before deferred tape runs | 161.805 s | 1,850,568,793 |
| Final host with uniform scan/search reuse | 104.951 s | 1,268,907,747 |

This fixed prefix was **1.54× faster**, with 31.4% fewer interpreter dispatches.
Execution time includes tape initialization and device callbacks, excluding
parsing and host setup. Stdout, guest result (50106), source/disk hashes and
guest instruction count matched. Both executions reached 518 disk-word reads;
the instruction-limit exit intentionally returned a nonzero status.

These are single runs, not medians. The prefix includes early WAD directory
reading but does not finish WAD loading or produce a frame, so this is neither
a full Doom BF boot result nor an estimate of its eventual frame rate. The BF
source SHA256 was
`08a29f5821794ce25c87faeb85b21afe94db6d7aa72bffc48d74e43d741ac09c`.
The disk SHA256 was
`d05372d873f1cdcbf80c8126c84fdf182a749d17cf866c3ab959d6f58b09c282`.
Saved inputs, executables, reports and telemetry are under ignored
`build/bf-far-access/dropout/`; `comparison.json` records this pair.

The combined host and mutable-pointer compiler changes also passed a separate
1,000,000-instruction BF/Rust VM lockstep check: all registers at each boundary,
each ordinary RAM store, final RAM and 299 console-output bytes matched. That
check used a 180-second deadline and a 210-second external bound; its additional
checking overhead and rebuilt guest make it a correctness check, not another
timing comparison. Evidence is in ignored
`build/pointer-ssa/bf-validation/lockstep.log`.
