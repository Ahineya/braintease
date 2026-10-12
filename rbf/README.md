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
currently pulls whole archives; the complete runtime archive can exceed the BF
CPU's single code-bank limit even for a small application.

The smoke checks use 512-cell banks to keep real BF memory seeking small.
64,000-cell banks are supported but the BF CPU's bank-seeking algorithms can
take much longer even for a tiny C program; interpreter dispatch speed alone
does not remove that guest work. Choose banks large enough for both the code
and each program's stack/data needs.

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
code that waits for a device response may wait indefinitely. The existing
single-code-bank limit applies in both modes.

## Devices and automation

Supported devices use the same guest interface as `rvm`:

- TTY byte output/input, ready flags and seeded RNG.
- Word-addressed block storage, explicitly selected with `--disk image.img`.
  The image is writable and changes are committed on shutdown. No default user
  disk is opened. Host I/O failures are reported.
- Legacy bank-zero RGB565 double buffering (mode 3).
- RGB565_EXT (mode 4), including strided framebuffers and descriptors crossing banks.
- Device ABI 1: capabilities, PRESENT, NEXT_TICK, GET_TIME, input transitions
  and the 256-key held-state snapshot.

Execution is headless by default. `--visual` opens the shared RGB565 window and
accepts live keys. `--input TEXT` queues TTY bytes; `--tty-stdin` streams TTY input
from stdin. The latter is separate from the keyboard device and cannot share
stdin with the JSON controller. EOF produces an empty input queue.

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

## Execution engine

The reusable core is [`src/rust-bf/src/engine.rs`](../src/rust-bf/src/engine.rs).
It uses a flat instruction stream with resolved jumps, offset arithmetic blocks,
multi-target affine-loop reduction (including odd control-cell deltas), and
bounded scan loops. Safe, checked fallback execution preserves tape-boundary
behavior. Device and byte-I/O hooks are optimization barriers. The host accesses
the RAM lane directly through `GuestMemory`; it does not copy the full tape.
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
scripts/test-bf-host.sh
```

The smoke script runs small assembly fixtures and a compiled C program. It
checks repeated commands, logical clock purity, scripted input, bank-crossing
RGB565 pixels, legacy buffer swapping, TTY input, RNG, disk reads/commit and
separate C stack/global banks.
Artifacts are written to `build/bf-smoke`; set `BF_SMOKE_OUT` to override it.
The interpreter tests compare full state and events against an independent
literal BF oracle. Shared device tests compare flat and strided RAM.
No check builds or executes Doom.

## Remaining BF CPU limits

The host does not remove the BF CPU's **single code-bank restriction**. Multiple
RAM banks work, but a large program such as the full Doom port still needs
multi-bank instruction fetching in the BF CPU before export. LOADC/STORC,
legacy assembler `.data` placement, and invalid entry/layout configurations
are rejected explicitly. C globals initialized through GP-bank stores work.
The BF CPU's supported arithmetic/call subset has regression coverage; shift
counts outside 0..15 and division/modulo by zero remain unverified against RVM.

TEXT40 terminal rendering and CPU interrupts are not implemented by this host.
Nonzero IRQ configuration fails explicitly; IRQ reads report no pending state.
BRK is a no-op in the non-debug BF host. These limitations do not change the
Rust VM or its instruction set. Full Doom BF execution remains a separate
milestone; only the small fixtures above are part of validation here.
