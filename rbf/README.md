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
before its first flush. Its 16,384-cell bank layout also places the C stack in
bank 2, requiring BF seeks to guest addresses above 32,768. Before compiler
inlining, frame reduction and pointer optimizations, a bounded 40-second trace
reached only 28 background writes and no flush. The black window did not indicate
a failed display. The compiler now removes much of that overhead, but the larger
frame and bank layout still make this a heavier workload than the assembly demo.
The same 40-second local check after those changes reached 487 background writes
without flushing a frame. Use the small compiled-C checks below for bounded,
exact-pixel verification; the larger demo still has a long startup in BF.

To rebuild and run that original C demo with progress reporting:

```sh
scripts/run-bf-rgb565-c.sh --telemetry
```

This checks tool freshness and recompiles the demo plus its graphics and MMIO
libraries on every run. It replaces `build/bf/rgb565-demo.bf`, keeping intermediate
files and a matching `.bin` for RVM comparisons alongside it. It defaults to
16,384-cell banks and uses three RAM banks for the display, C globals and stack.
Runtime objects under `runtime/` are untouched. Set `BF_RGB565_C_OUT` to choose
another output directory, or use `--build-only` to prepare the image without
running it. Arrow keys move the red square once frames are being presented.

`BF_RGB565_C_BANK_SIZE` can select a smaller bank, for example
`BF_RGB565_C_BANK_SIZE=8320 scripts/run-bf-rgb565-c.sh --telemetry`. Smaller banks
place the globals and stack closer to the beginning of the BF tape. The accepted
range is 8,224–65,535 cells; 8,224 is exactly the 32 MMIO cells plus two 64×64
framebuffers. Banks need not be powers of two. All objects are rebuilt with the
selected bank size.

For a bounded headless first-frame capture:

```sh
scripts/run-bf-rgb565-c.sh --headless --telemetry --capture-frame 1 \
  --screenshot build/bf/rgb565-demo.png --raw-frame build/bf/rgb565-demo.rgb565 \
  --report build/bf/rgb565-demo.json --timeout 600
```

The timeout is a safety bound, not a promised completion time. Missing a
requested frame returns a nonzero status and still leaves a diagnostic report.

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

Add `--telemetry` for progress on stderr while a run is still in flight:

```sh
scripts/run-bf-rgb565.sh --telemetry --telemetry-interval 1
```

Telemetry reports loading/parsing/startup, elapsed time, BF and guest instruction
counts/rates, the last validated guest program counter, frame/tick counts,
clock waits and recent memory accesses. In legacy RGB565 mode it also reports
back-buffer writes, which helps distinguish drawing from waiting for a flush.
That write count includes repeated stores; it is not a count of unique pixels.
The sampled idle durations and rates describe observed work, not completion
percentages or predicted finishing times. Telemetry is off by default and works
with headless or visual runs; it cannot share the TEXT40 terminal screen.
`--telemetry-interval` requires `--telemetry` and accepts 0.01–3600 seconds
(default 1). Guest TTY output remains on stdout.

## Execution engine

The reusable core is [`src/rust-bf/src/engine.rs`](../src/rust-bf/src/engine.rs).
It uses a flat instruction stream with resolved jumps, offset arithmetic blocks,
multi-target affine-loop reduction (including odd control-cell deltas), and
cached tape scans. Guarded folds handle transfers through invariant scan routes
and counted loops that fill successive zero cells. Fills use bounded batches
and preserve every actual tape write. These optimizations inspect BF operations and tape cells,
with no knowledge of Ripple opcodes or register layouts. Checked fallback
execution preserves tape-boundary behavior. Device and byte-I/O hooks are
optimization barriers. The host accesses the RAM lane directly through
`GuestMemory`; individual device writes invalidate only affected scan-cache
entries instead of clearing the cache on every memory access.
Frame publication reuses the existing immutable display buffers.

`--no-optimize` disables the loop/block optimizations for comparison. It still
combines adjacent arithmetic and monotonic pointer runs. All host cells wrap at
16 bits; the tape pointer has strict bounds, with a leading zero guard for the
CPU's left-seeking macros. The existing `bf` CLI uses the same core for its
default 16-bit wrapping configuration. The browser/WASM API is unchanged.

## Small checks

```sh
cargo test -p rbf
cargo test --manifest-path src/rust-bf/Cargo.toml --lib engine
cargo test --manifest-path src/ripple-asm/Cargo.toml --test bf_host
cargo test --manifest-path src/ripple-asm/Cargo.toml --test bf_multibank
scripts/test-bf-host.sh
node scripts/test-bf-compiler.mjs
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
No check builds or executes Doom.

The compiler check builds ordinary C graphics-API and direct-pointer examples,
then compares exact 4×2 and 8×2 RGB565 frames on RVM and BF. It also checks guest
instruction ceilings; wall time is recorded but never used as a pass/fail limit
apart from a generous execution timeout. Run it after building the release tools
listed above. `--rvm-only` skips BF execution. Reports, tool hashes, reproduction
commands and captures go to `build/bf-compiler-regression`; `BF_COMPILER_OUT`
overrides that directory. No runtime rebuild or Doom source is involved.

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
Rust VM or its instruction set. Full Doom BF execution remains a separate
milestone; only the small fixtures above are part of validation here.
