# Ripple Brainfuck host

`rbf` runs the Ripple CPU implemented in Brainfuck with native host devices.
The CPU's decoding, arithmetic, branches, register transfers and memory seeking
all execute as BF. Rust supplies peripherals at explicit LOAD/STORE boundaries;
it never dispatches Ripple instructions. The guest ISA is unchanged.

## Build and run

From the repository root:

```sh
cargo build --release
cargo build --release --manifest-path src/ripple-asm/Cargo.toml
cargo build --release --manifest-path src/bf-macro-expander/Cargo.toml
BANK_SIZE=512 MEMORY_BANKS=4 scripts/build-bf.sh rbf/tests/fixtures/tiny.c
target/release/rbf build/bf/tiny.bf --headless --timeout 30 --exit-code
```

The workspace release profile uses optimization level 3, ThinLTO across crate
boundaries, and one code-generation unit per crate. This also optimizes the BF
engine linked into `rbf`; the standalone interpreter's own release profile does
not apply when it is built as a workspace dependency.

The helper accepts C or assembly. C uses the ordinary compiler and runtime
startup, assembled separately into the output directory. It writes `.asm`,
`.pobj`, `.bfm` and expanded `.bf` under
`build/bf` (ignored by Git). An optional second argument chooses the output
prefix. `BANK_SIZE` defaults to 64000 and `MEMORY_BANKS` to 16; the helper keeps
the runtime/compiler/assembler/linker bank sizes consistent. Extra arguments
after the output prefix are additional object files. For libc calls, build and
pass the needed runtime objects (for example `runtime/putchar.pobj`). The linker
reads whole archives, then removes unreachable functions and global initializer
code from current compiler objects. Legacy assembly objects remain intact.
Unused C global storage reservations remain to preserve compiled GP offsets.
Linking only needed objects can still reduce memory requirements.

The smoke checks use 512-cell banks to keep real BF memory seeking small.
64,000-cell banks are supported but the BF CPU's bank-seeking algorithms can
take much longer even for a tiny C program; interpreter dispatch speed alone
does not remove that guest work. Choose banks large enough for both the code
and each program's stack/data needs. Code can span banks; the linker still
requires each individual function to fit within one bank.

For an already linked set of objects:

```sh
src/ripple-asm/target/release/rlink runtime/crt0.pobj main.pobj \
  -f macro --standalone --bf-host --bank-size 64000 --memory-banks 16 -o program.bfm
src/bf-macro-expander/target/release/bfm expand program.bfm --strip-comments -o program.bf
target/release/rbf program.bf --headless --timeout 30
```

The export includes a versioned tape-layout header and three annotations:
`?` before a RAM read, `!` after a completed RAM write, and `$` before each
guest instruction. These are host transport markers, not new Ripple opcodes.
An ordinary BF interpreter ignores them and cannot provide these peripherals.
`rbf` deliberately requires the header instead of guessing a tape layout.
See [the export contract](../src/ripple-asm/BF_HOST.md) for exact positions.

## Small RGB565 demo

```sh
scripts/run-bf-rgb565.sh
```

This builds a self-contained 16×12 assembly demo and opens its RGB565 window.
Red, green and blue bands appear first, followed by a white marker moving across
the middle band. The window opens at 320×240 so its pixels are easy to see.
Close the window or press Ctrl-C to stop. It uses 512-cell banks
and has no C runtime or disk dependency. All guest instructions still execute in
the Brainfuck CPU. Cargo rebuilds tools only when their sources have changed.

Both buffers are initialized during the first two frames; subsequent frames
need just four pixel writes. The demo requests one logical tick per animated
frame and defaults to paced, eight-tick-per-second operation. Rendering can run
slower than that rate without skipping animation steps.

For a bounded headless capture of the first moving frame:

```sh
scripts/run-bf-rgb565.sh --headless --clock unthrottled --capture-frame 3 \
  --screenshot build/bf-rgb565/frame3.png --raw-frame build/bf-rgb565/frame3.rgb565 \
  --report build/bf-rgb565/frame3.json --timeout 30
```

`--build-only` prepares `build/bf-rgb565/rgb565.bf` without launching it.
Set `BF_RGB565_OUT` to change that output directory. The larger C graphics
examples perform much more guest work and are unsuitable for checking that the
BF display works promptly.

In particular, the 64×64 `rgb565_interactive_simple.c` demo clears 4096 pixels
before its first flush. Its unchanged 16,384-cell bank layout places the C stack
in bank 2, requiring BF seeks to guest addresses above 32,768. This makes it a
useful compiler/interpreter workload as well as a display demo. The host cannot
show a completed frame until C finishes drawing and requests a flush; telemetry
shows the intervening progress. Use the small compiled-C checks below for quick,
exact-pixel verification.

To rebuild and run that original C demo with progress reporting:

```sh
scripts/run-bf-rgb565-c.sh --telemetry
```

This checks tool freshness and recompiles the demo plus its graphics and MMIO
libraries on every run. It replaces `build/bf/rgb565-demo.bf`, keeping intermediate
files and a matching `.bin` for RVM comparisons alongside it. It uses
three 16,384-cell RAM banks for the display, C globals and stack.
Runtime objects under `runtime/` are untouched. Set `BF_RGB565_C_OUT` to choose
another output directory, or use `--build-only` to prepare the image without
running it. Arrow keys move the red square once frames are being presented.

For a bounded headless first-frame capture:

```sh
scripts/run-bf-rgb565-c.sh --headless --telemetry --capture-frame 1 \
  --screenshot build/bf/rgb565-demo.png --raw-frame build/bf/rgb565-demo.rgb565 \
  --report build/bf/rgb565-demo.json --timeout 120
```

The timeout is a safety bound, not a promised completion time. Missing a
requested frame returns a nonzero status and still leaves a diagnostic report.

On the development machine, the latest changes measured on 2026-10-13 reduced
the median first frame from 0.734 to 0.550 seconds, keeping the same 16,384-cell
banks and exact pixels. Reaching two frames took 1.457 seconds before and 1.098
seconds after. These are medians of five alternating, sequential release runs,
including tape initialization but excluding build, parsing and host setup.
Individual first-frame times ranged from 0.728–0.736 seconds before and
0.549–0.551 seconds after; they are not estimates for Doom. The release-profile
change alone was below 1% on this fixture; the algorithm changes account for
the useful gain.

First-frame work fell from 38,890 to 29,911 guest instructions. The next frame
fell from 38,811 to 29,843. Proven native-word counted loops retain a zero-trip
guard and use a direct equality branch at the bottom, reducing the screen-clear
loop from six to four instructions per pixel. Rotation follows pointer-bank
analysis so the existing range proofs survive. Private stack-home liveness
removes dead preloads and stores around promoted loops; copy cleanup retains
registers needed across branches and avoids per-pixel temporary moves.

The generic BF interpreter rejects impossible fill candidates during parsing
and precomputes static route work. On the frozen baseline image, a bounded
diagnostic profile eliminated all 1.68 million failed fill attempts without
changing its BF dispatch count or physical writes. Demo source, ISA, CPU and
bank sizes are unchanged in this performance pass.

## Doom launcher

```sh
scripts/run-bf-doom.sh --telemetry-lite
scripts/run-bf-doom.sh --build-only
```

This launches the full maintained port in `ports/doom`, using the same C and
guest assembly as the Rust VM build. The wrapper delegates to
`ports/doom/run-bf.sh`, checks release tool freshness, builds and exports the
engine, stages a writable WAD copy, and opens the graphics window. It retains
64,000-cell banks and 256 RAM banks. The BF CPU performs all guest execution;
the host supplies the existing devices. Logical ticks are unthrottled by default.

There is **no default timeout**. Close the window or press Ctrl-C to stop.
A user-confirmed run reached Doom's first title frame after approximately
2,500 seconds (42 minutes); subsequent frames remained slow. This is a manual
milestone, not an automated full-Doom BF validation run. `--telemetry-lite`
shows compact progress while waiting; `--telemetry` provides the detailed
diagnostics. For a bounded attempt:

```sh
scripts/run-bf-doom.sh --headless --telemetry --timeout 60 \
  --screenshot ports/doom/build/run-bf/title.png
```

The timeout covers execution and guest tape initialization, after compilation,
export, parsing and host setup. It is not a promised time to the first frame.
`--headless` alone does not request a capture or stop at a frame. Add capture
options explicitly; a missed capture deadline returns nonzero and leaves a
diagnostic report.

Use `--scene e1m1`, `--low-detail`, `--wad FILE` and `--jobs N` for port settings.
Other `rbf` options pass through, including capture flags and optional limits.
The image is `ports/doom/build/ripple/engine/engine.bf`. The linker's direct BF
export emits compact ordinary BF initializers, avoiding a huge macro expansion
for millions of guest instructions. The guest ISA is unchanged.
The default report and staged disk go under `ports/doom/build/run-bf/`; change
that with `--output DIRECTORY`, or select a report using `--report FILE`.
`DOOM_BUILD` and `DOOM_WAD` select another build tree and source WAD.

The launcher reads the image's tape requirement and supplies the allocation
limit, because Doom exceeds `rbf`'s ordinary default. Allow hundreds of megabytes
for the tape alone, plus expanded source and interpreter data. Use
`make -C ports/doom -j4 bf` or `TARGET=bf` for a direct export build after building
the tools. See [the port guide](../ports/doom/README.md#brainfuck-host) for details.

## Ordinary Brainfuck interpreters

The portable CPU remains standard Brainfuck with 16-bit wrapping cells. Build
without host instrumentation when targeting an ordinary interpreter:

```sh
BF_HOST=0 BANK_SIZE=512 scripts/build-bf.sh rbf/tests/fixtures/tiny.c build/bf/tiny-plain
```

This is equivalent to omitting `--bf-host` from the standalone linker command.
The resulting `.bf` runs the CPU, including C startup, globals and stack, and
prints `Y` through the standard `.` command. No Rust callbacks execute guest
instructions. Host exports are a separate mode: they disable native `.` output
because `rbf` owns TTY output, so simply ignoring their annotations does not
restore console output.

For [copy.sh](https://copy.sh/brainfuck/), select **16-bit cells**, leave
**Dynamic Memory off**, set **Memory overflow behaviour to wrap**, and allocate
enough cells. **65,536 cells** is sufficient for the tiny example above. The
plain CPU starts at cell zero and uses the wrapping tape's trailing zero word
as its left guard; the tape size must be divisible by eight. More generally,
the conservative allocation `8 * (max(N*B, 40+4*I+36) + 33)` covers `N` RAM
banks of size `B`, `I` linked instructions, and scratch/guard space. For 16
64,000-cell RAM banks and one code bank, 8,388,608 cells is sufficient. This is
the interpreter's cell count, not a byte count or the guest's bank size.

Without a host, text output works, but MMIO input, keyboard events, disk, RNG,
graphics and logical clocks are unavailable. The CPU does not translate input
MMIO into `,`; an interpreter's EOF option cannot supply those services. Guest
code that waits for a device response may wait indefinitely. Multiple code
banks work in both plain and host exports, with all bank seeking done in BF.

## Devices and automation

Supported devices use the same guest interface as `rvm`:

- TTY byte output/input, ready flags and seeded RNG.
- Word-addressed block storage, explicitly selected with `--disk image.img`.
  The image is writable and changes are committed on shutdown. No default user
  disk is opened. Host I/O failures are reported.
- TEXT40 (mode 2): 40×25 character/attribute cells, the shared terminal color
  palette, display enable/clear/flush, and legacy arrow/Z/X key registers.
- Legacy bank-zero RGB565 double buffering (mode 3).
- RGB565_EXT (mode 4), including strided framebuffers and descriptors crossing banks.
- Device ABI 1: capabilities, PRESENT, NEXT_TICK, GET_TIME, input transitions
  and the 256-key held-state snapshot.

Execution is headless by default. `--visual` opens the shared RGB565 window and
accepts live keys. `--input TEXT` queues TTY bytes; `--tty-stdin` streams TTY input
from stdin. The latter is separate from the keyboard device and cannot share
stdin with the JSON controller. EOF produces an empty input queue.

For TEXT40, `--terminal` uses an alternate terminal screen and accepts live
keys. It requires terminal stdin/stderr and cannot share input with
`--tty-stdin` or `--control-stdin`, or be combined with `--visual`/`--headless`.
Terminal key state follows RVM's existing short pulse convention because a
terminal does not reliably deliver key-release events. Deterministic replay
continues to use logical tick grants.

```sh
BANK_SIZE=2048 MEMORY_BANKS=1 scripts/build-bf.sh rbf/tests/fixtures/text40.asm
target/release/rbf build/bf/text40.bf --terminal --timeout 30 # Press Z to exit
# Exact headless snapshot, including foreground/background attributes:
target/release/rbf build/bf/text40.bf --headless --text-frame build/text40.cells --timeout 30
```

`--text-frame` captures the requested completed TEXT40 flush and stops. It
writes 1000 little-endian 16-bit cells in row order (2000 bytes): low octet is
the character, high octet is the attribute. Attribute low/high nibbles select
foreground/background colors. It shares `--capture-frame` with RGB captures
but cannot be combined with `--screenshot`, `--raw-frame`, or `--expect-frame`.
Reports record 40×25 dimensions for TEXT40; PNG capture remains for RGB modes.

Logical time advances only when the guest requests NEXT_TICK. `--clock
unthrottled` grants immediately, `paced` waits at most one tick interval, and
`stepped --control-stdin` accepts the same ordered JSON commands as `rvm`.
Slow BF execution never accumulates wall-clock catch-up ticks.
`--input-script keys.json` replays key events at tick grants; see
[the replay format](../docs/doom-platform.md#input-replay-and-stepping).

```sh
target/release/rbf program.bf --headless --seed 1 --clock unthrottled \
  --input-script keys.json --screenshot build/frame.png --raw-frame build/frame.rgb565 \
  --capture-frame 1 --report build/run.json --timeout 30 --max-dispatches 100000000
```

PNG and raw captures represent the completed native-resolution front frame.
`--expect-frame baseline.rgb565` compares exact pixels and writes expected,
actual and difference PNGs on mismatch. Raw files contain two little-endian
host octets per pixel. Missing requested frames and budget/time failures return
nonzero status. `--exit-code` returns the low octet of RV0 after a normal halt.

Reports identify the BF source, initial disk and input-script hashes, seed,
tape layout, elapsed execution time, tick/frame counts and guest return value.
They distinguish **BF dispatches** (optimized interpreter operations) from
**Ripple instructions** (observed starts). Neither count pretends to measure
literal BF characters. `--max-instructions`, `--max-dispatches`, `--max-ticks`
and `--timeout` allow bounded runs. Ctrl-C, window close and timeout are polled
even during tape initialization and long BF loops, before any device hook.

For compact progress on stderr, add `--telemetry-lite`:

```sh
scripts/run-bf-doom.sh --telemetry-lite
```

It prints a dot every five seconds and a compact summary every thirty seconds:

```text
......
[00:00:30] BF 1,234,567 (41,152/s) | Guest 50,000 (1,666.7/s)
```

The time is elapsed execution time (`HH:MM:SS`). Counts are cumulative; rates
cover the interval since the previous summary. BF counts optimized interpreter
dispatches, and Guest counts observed Ripple instruction starts. A final summary
includes the stop status even when execution ends before thirty seconds.
Compact mode omits detailed loading/parsing, program-counter, memory and disk
diagnostics. Its fixed cadence cannot be combined with `--telemetry`,
`--telemetry-interval` or the TEXT40 `--terminal` screen. Guest output stays on
stdout; telemetry is off by default.

For detailed progress, add `--telemetry`:

```sh
scripts/run-bf-rgb565.sh --telemetry --telemetry-interval 1
```

Telemetry reports loading/parsing/startup, elapsed time, BF and guest instruction
counts/rates, the last validated guest program counter, frame/tick counts,
clock waits and recent memory accesses. In legacy RGB565 mode it also reports
back-buffer writes, which helps distinguish drawing from waiting for a flush.
That write count includes repeated stores; it is not a count of unique pixels.
Storage progress adds cumulative `disk_read_words` and `disk_write_words`, a
per-interval `disk_read_words/s` rate, the last successful read's `disk_last_read`
position, the currently selected `disk_pos`, and sampled `disk_idle~` time since
the last read or write. Disk positions are `block:word`, with 32,768 16-bit
words per block; they are independent of RAM bank size. These count successful
device transfers, including repeated reads and defined out-of-range zero reads,
not unique WAD bytes or actual host disk reads. Blocks are cached after their
first access. Address-selection writes do not count as data progress, and no
disk counters are collected when telemetry is disabled.
The sampled idle durations and rates describe observed work, not completion
percentages or predicted finishing times. Telemetry is off by default and works
with headless or visual runs; it cannot share the TEXT40 terminal screen.
`--telemetry-interval` requires `--telemetry` and accepts 0.01–3600 seconds
(default 1). Guest TTY output remains on stdout.

For diagnosing execution costs, add `--profile FILE`:

```sh
scripts/run-bf-doom.sh --headless --telemetry --timeout 60 \
  --profile ports/doom/build/run-bf/profile.json
```

The JSON report is written on completion, cancellation or an execution error,
including a timeout. It records attempts, successful summaries, fallbacks and
time for generic BF operations (arithmetic, scans, moving updates and routed
loops), plus a bounded table of navigation-loop source offsets. Offsets are
zero-based bytes in the expanded `.bf` file, including its header. Site eviction
and omitted-attempt counts are explicit; operation totals remain complete.
The profiler does not decode guest instructions. Profiling adds overhead and
its operation times exclude host callbacks and profiler bookkeeping, so use
ordinary runs for before/after performance measurements. Normal execution does
not collect these timings.

## Save and resume execution

Save a booted Doom session once, then resume it without repeating startup:

```sh
scripts/run-bf-doom.sh --telemetry-lite \
  --save-state ports/doom/build/run-bf/boot.state --save-at-frame 1
scripts/run-bf-doom.sh --telemetry-lite \
  --load-state ports/doom/build/run-bf/boot.state
```

`--save-state FILE` saves on normal exit, Ctrl-C, window close, or an execution
limit. `--save-at-frame N` additionally stops immediately after presentation N;
it requires `--save-state` and does not require a screenshot. Without that flag,
closing the window saves wherever execution has reached. A guest/device/output
error does not overwrite a good checkpoint. Saving uses an atomic replacement
and a checksum so a partial file cannot be mistaken for a valid save.

The checkpoint includes the logical BF tape, pointer and execution position,
BF/guest counters, clock and pending tick, RNG, keyboard queues and held keys,
script position, TTY queues, display buffers, and storage position/cache state.
The sparse binary tape encoding avoids writing every zero cell. Native windows,
terminal handles and external input connections are recreated. Accepted input
and controller credits are preserved; unread future stdin is not a recording.
Previously printed output is not replayed. Diagnostic profile/telemetry samples
start fresh; execution counters, ticks and frame numbers remain cumulative.

The original **unchanged BF image and disk are required**. Their identities and
the parsed BF program are checked on restore. A guest/compiler rebuild generally
requires a new checkpoint; interpreter execution improvements remain compatible
when the parsed program and state format are unchanged. The Doom launcher skips
its guest build and disk restaging when `--load-state` is supplied, while still
checking the release host build. Keep the checkpoint together with its image
and staged disk. A running process started without checkpoint support cannot be
saved retroactively.
The disk is external rather than embedded in the checkpoint; preserve a copy at
the checkpoint's contents if the guest may write to it. Interpreter caches start
cold, so future optimized BF dispatch counts may differ from an uninterrupted
run even though logical guest execution is identical.

For a bounded profile starting at the saved frame:

```sh
scripts/run-bf-doom.sh --headless --load-state ports/doom/build/run-bf/boot.state \
  --timeout 30 --profile ports/doom/build/run-bf/after-boot-profile.json
```

`--timeout` starts fresh after restore. Instruction/dispatch/tick limits and
capture/save frame numbers are absolute cumulative limits. To capture the next
frame from a frame-1 checkpoint, use `--capture-frame 2`. The saved tick rate and
RNG state are retained; `--seed` and `--tick-hz` cannot be supplied with restore.
The clock pacing policy and profiling/telemetry options may be changed.

By default the saved input script resumes at its saved position. Supplying
`--input-script FILE` explicitly replaces it; events must be later than the
current tick. Add `--input-script-relative` to interpret tick 1 as the first
tick after restore. This makes a short Escape press reusable for opening the
menu from any boot checkpoint. Framebuffer/device semantics and the BF CPU/ISA
are unchanged.

## Execution engine

The reusable core is [`src/rust-bf/src/engine.rs`](../src/rust-bf/src/engine.rs).
It uses a flat instruction stream with resolved jumps, offset arithmetic blocks,
multi-target affine-loop reduction (including odd control-cell deltas), and
cached tape scans. Separate caches track nonzero scan runs and zero runs used
by moving-marker searches. Guarded folds handle transfers through scan routes,
including nested moving-marker return searches, while their predicates remain
unchanged. They verify exact marker, zero-cell and sentinel conditions and stop
before a changing boundary. Other folds handle counted loops that fill
successive zero cells. Two-write transfers use a smaller single-body proof when
the scan strides establish independent counter, destination and later scan
lanes. This retains logical tape access and cached scans, with no physical
window preparation or general multi-iteration proof arrays.
Parsing rejects statically impossible fill routes,
combines pointer moves while retaining their full boundary excursions, and
precomputes the valid fills' excursion bounds. Fills check route exclusions once
per bounded batch,
invalidate cached runs together, and record the verified nonzero cells for
subsequent scans. Moving loops with one updated cell per iteration also batch
cache maintenance, including writes at an offset from the predicate cell.
Cached predicate runs can skip repeated reads only when their stride residue
is disjoint from the writes; dependent predicates are checked in order.
Proven uniform strided fills and moving updates can retain their values as
deferred tape runs instead of writing each physical cell. Restoring a run's
known backing value cancels the writes, and retaining that clean fact avoids
rescanning the same range on its next use. Reads always see the exact logical
value; point writes split runs, and incompatible patterns use bounded ordinary
execution. Run storage is capped through incremental maintenance, with at most
1024 physical cells committed per maintenance dispatch. Profiling reports this
work as `materialize`. Fallback execution preserves partial bounds failures.
Moving updates can also target a parallel progression when the write residue is
disjoint from the predicate and both ranges are proved. This folds copy-like
marker walks and their cleanup without knowing why the BF program uses them.
Bounded physical walks retain newly proved uniform values for reuse. Conservative
range/residue filters let unrelated cell accesses bypass the run list; clean
facts already agree with physical storage and need no read indirection.
Scans and moving-marker searches also reuse these uniform tape facts after a
different stride evicts their predicate cache. A scan may follow a subset of a
proved progression: its stride must be an exact multiple of the progression's
stride, and its starting cell must belong to that progression. The ordinary
sentinel checks and full pointer-excursion bounds still apply. This avoids
repeated logical-cell lookups across a range whose predicate is already proved.
These optimizations inspect BF operations and tape cells,
with no knowledge of Ripple opcodes or register layouts. Checked fallback
execution preserves tape-boundary behavior. Device and byte-I/O hooks are
optimization barriers. The host accesses the RAM lane directly through
`GuestMemory`; individual device writes invalidate only affected scan-cache
entries instead of clearing the cache on every memory access.
Frame publication reuses the existing immutable display buffers.

Core callers should use `read_cell`, `write_cell` and `tape_len` for individual
observations. `tape(&mut self)` and `tape_mut` return an exact dense snapshot by
materializing deferred writes and discarding the retained run facts. They are
useful for bulk inspection, but calling them at every host hook defeats this
optimization. The peripheral adapter uses point access, so device reads and
writes preserve unrelated runs. Input, output, hooks and budget yields expose
the same logical state as ordinary BF execution.

`--no-optimize` disables the loop/block optimizations for comparison. It still
combines adjacent arithmetic and monotonic pointer runs. All host cells wrap at
16 bits; the tape pointer has strict bounds, with a leading zero guard for the
CPU's left-seeking macros. The existing `bf` CLI uses the same core for its
default 16-bit wrapping configuration. The browser/WASM API is unchanged.

### Optional native Brainfuck JIT

```sh
target/release/rbf program.bf --jit --headless --timeout 30 --report build/jit.json
scripts/run-bf-rgb565-c.sh --jit
```

`--jit` compiles hot generic BF loops on AArch64/x86-64. Ripple instructions
still execute through the BF CPU; the ISA, exported programs and devices are
unchanged. The flag is opt-in and cannot be combined with `--profile`.
The execution report gains a `jit` object with compilation time, native code
size, compiled/retired regions, native dispatches, entry counts and guard
fallbacks. Other reports retain their existing shape.

The JIT retains the interpreter's scan/search/route and deferred-tape algorithms.
It preserves bounds failures, 16-bit wrapping, host hooks, cancellation slices
and checkpoint behavior. Native code and heat counters are temporary: each
`Host::run` or restored process starts with a cold native cache. Checkpoints can
switch between interpreter and JIT. See the [core guide](../src/rust-bf/README_ENGINE.md#optional-native-jit)
for compilation limits and memory/coherence details.

JIT is not automatically faster for the BF CPU. Its scan/route summaries already
replace substantial BF work with efficient Rust algorithms, and device barriers
limit how long execution can remain native. Use the [bounded comparison driver](benchmarks/README.md)
on the actual workload; Mandelbrot results do not predict Doom performance.
The [checked C/assembly suite](benchmarks/jit/README.md) adds arithmetic, indexed
memory, recursive calls, wide integers and sorting, with matching RVM images
and bounded BF loop profiles.

## Small checks

```sh
cargo test -p rbf
cargo test --manifest-path src/rust-bf/Cargo.toml --lib engine
cargo test --manifest-path src/ripple-asm/Cargo.toml --test bf_host
cargo test --manifest-path src/ripple-asm/Cargo.toml --test bf_multibank
cargo test --manifest-path src/ripple-asm/Cargo.toml --test bf_compact
scripts/test-bf-host.sh
node scripts/test-bf-compiler.mjs
node scripts/test-bf-compiler.mjs --large-banks
node scripts/test-bf-compiler.mjs --demo
```

The smoke script runs small assembly fixtures and a compiled C program. It
checks repeated commands, logical clock purity, scripted input, bank-crossing
RGB565 pixels, legacy buffer swapping, TTY input, RNG, disk reads/commit and
separate C stack/global banks.
It also checks TEXT40 character/attribute snapshots and code-bank transitions,
including entry in a later bank and calls/returns across a boundary.
The public RGB565 demo is checked pixel-for-pixel at frames 1, 3, 4 and 19,
covering its first flush, both buffers and marker wraparound.
Artifacts are written to `build/bf-smoke`; set `BF_SMOKE_OUT` to override it.
The interpreter tests compare full state and events against an independent
literal BF oracle. Shared device tests compare flat and strided RAM.
The CPU comparison tests cover unsigned 65535 and signed 32767 in both host
and ordinary standalone BF. The shared comparator guards its increment against
maximum-value wraparound; a direct literal oracle checks all 8-bit operand
pairs plus 16-bit boundary/sample pairs, including scratch cleanup.
LI copies its immediate directly using ordinary BF; it no longer converts the
value to bits just to OR it with zero. Literal-interpreter tests cover boundary
values, overwrites, R0, other registers, scratch cleanup and preserved operands.
No check builds or executes Doom.

The compiler check builds ordinary C graphics-API and direct-pointer examples,
then compares exact 4×2 and 8×2 RGB565 frames on RVM and BF. A 12×8 rectangle
fixture covers overlap, clipping, empty dimensions, maximum dimensions and high
unsigned coordinates outside the screen, including 65534 and 65535. It also
checks guest
instruction ceilings; wall time is recorded but never used as a pass/fail limit
apart from a generous execution timeout. Run it after building the release tools
listed above. `--rvm-only` skips BF execution. Reports, tool hashes, reproduction
commands and captures go to `build/bf-compiler-regression`; `BF_COMPILER_OUT`
overrides that directory. `--large-banks` runs the same checks with 64,000-cell
banks and stores them in `build/bf-compiler-regression-large`, exercising distant
globals and stack without changing the demo's bank size. No runtime rebuild or
Doom source is involved.

`--demo` checks the original 64×64 interactive C demo at its unchanged
16,384-cell bank size. It compares the first frame with independent expected
geometry and enforces a guest-instruction ceiling, covering the interaction of
counted-loop rotation, safe graphics assembly and register-copy cleanup. This
bounded check stops at the first frame; it does not need a window or key input.

## Remaining BF CPU limits

Multiple code and RAM banks are supported. Fetch uses `PCB*BANK_SIZE+PC`,
matching RVM even for raw jump/return offsets beyond a bank's size. Sequential
rollover, cross-bank calls/returns, and nonzero-bank entry execute in BF.
The linker's one-bank-per-function limit still applies. LOADC/STORC,
legacy assembler `.data` placement, and invalid entry/layout configurations
are rejected explicitly. C globals initialized through GP-bank stores work.
The BF CPU's supported arithmetic/call subset has regression coverage; shift
counts outside 0..15 and division/modulo by zero remain unverified against RVM.
General ALU/LOAD writes directly to PC/PCB still do not reliably reposition the
BF instruction pointer; control flow should use branches, JAL and JALR.

CPU interrupts are not implemented by this host.
Nonzero IRQ configuration fails explicitly; IRQ reads report no pending state.
BRK is a no-op in the non-debug BF host. These limitations do not change the
Rust VM or its instruction set. Doom's first title frame has been confirmed by
a user; full BF gameplay remains outside automated validation. Only the small
fixtures above are part of validation here.
