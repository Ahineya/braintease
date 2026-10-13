# Brainfuck CPU benchmarks

These small terminating programs exercise generic Brainfuck interpreter costs
without running Doom. Each succeeds by writing `Y` to TTY and returning RV0=0.

| Source | Bank size | RAM banks | Work |
| --- | ---: | ---: | --- |
| `late-code.asm` | 256 | 1 | A short loop after eight banks of skipped code |
| `far-ram.asm` | 512 | 16 | Sixteen STORE/LOAD pairs at the last RAM word |
| `bank-jumps.asm` | 257 | 1 | Thirty-two calls to a distant code bank and returns |

The assembler has no instruction repetition directive. `generate.mjs` expands
the explicitly marked padding in these templates into ordinary assembly, keeping
thousands of identical NOP lines out of the maintained sources. Always generate
the templates before assembling them.

From the repository root, with the release assembler/linker/macro tools built:

```sh
node rbf/benchmarks/generate.mjs build/bf-bench/inputs
BANK_SIZE=256 MEMORY_BANKS=1 scripts/build-bf.sh build/bf-bench/inputs/late-code.asm build/bf-bench/inputs/late-code
BANK_SIZE=512 MEMORY_BANKS=16 scripts/build-bf.sh build/bf-bench/inputs/far-ram.asm build/bf-bench/inputs/far-ram
BANK_SIZE=257 MEMORY_BANKS=1 scripts/build-bf.sh build/bf-bench/inputs/bank-jumps.asm build/bf-bench/inputs/bank-jumps
```

Keep those exact generated `.bf` files for both interpreter versions. Rebuilding
the CPU footer between measurements would also measure changes to the program.

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
