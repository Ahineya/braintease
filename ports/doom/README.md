# Doom on Ripple C

This is the real pinned Doomgeneric engine, adapted to 16-bit C bytes and the
existing Rust VM device interface. It uses the repository's shareware
`c-test/examples/doom/doom1.wad`; it does not download game data. Doom8088 is a
reference checkout, not the engine being built.

The first port profile is single-player, 320×200 RGB565, keyboard controls,
35 Hz logical simulation ticks and silent audio. The CPU instruction set and
Brainfuck implementation are unchanged. Development and smoke tests use only
the Rust VM.

The title screen, scripted menu-to-E1M1 startup, and direct E1M1 startup produce
exactly the same RGB565 pixels and frame/tick/game-state logs as the native
reference. Gameplay replays extend the comparison to movement, turning, firing,
use and the automap. These checks run `D_DoomMain` and the real world renderer.
Hot rendering, memory and fixed-point operations now have small guest assembly
implementations using the existing instruction set and C calling convention.
Both VM implementations and the device interface remain unchanged.
See [compiler optimizations](../../rcc-backend/OPTIMIZATIONS.md) for the passes,
validation and earlier measurement details.

## Build and launch

From the repository root, build the tools once:

```sh
cargo build --release
cargo build --release --manifest-path src/ripple-asm/Cargo.toml
python3 scripts/fetch-doom-sources.py
```

The launcher builds the engine, copies the WAD to protect the source fixture,
and opens the VM display:

```sh
python3 ports/doom/run.py
# Skip the title/menu and start E1M1 directly:
python3 ports/doom/run.py --scene e1m1
# Start with Doom's faster low-detail world rendering:
python3 ports/doom/run.py --scene e1m1 --low-detail
# Capture one frame and stop:
python3 ports/doom/run.py --scene e1m1 --headless
```

Use Escape/Enter/arrows in menus. In game use Up/Down or W/S to move forward/back,
Left/Right to turn, and A/D or comma/period to strafe. Control fires, Space uses
doors/switches, Shift runs, Alt plus Left/Right strafes, Tab opens the automap,
and number keys select weapons. Either left or right modifier works. On macOS,
fire uses Control (⌃), not Command (⌘). F5 toggles Doom's stock high/low detail
setting during play. Minus/equal shrink/enlarge the view for another speed and
quality tradeoff. Close the window to quit.

High detail remains the default. Low detail draws the world at half horizontal
resolution and duplicates adjacent pixels; the display stays 320×200 RGB565.
Menus and the status bar retain their normal resolution. `--low-detail` selects
the initial setting without changing the VM's display mode or memory layout.

The launcher defaults to paced time for a window and unthrottled logical time
for captures. `--input-script FILE.json` supplies deterministic keyboard events;
`--clock stepped` accepts the VM's JSON control protocol on standard input.
See [the platform guide](../../docs/doom-platform.md#input-replay-and-stepping).
`--build-only` stops after linking. `--wad PATH` selects an existing local IWAD;
the automated expectations are specifically for the bundled shareware WAD.

Headless runs write a PNG, raw RGB565, build manifest and JSON execution report under
`ports/doom/build/run/`. `--output DIR`, `--frame N`, `--timeout SECONDS`, and
`--max-instructions N` control artifacts and bounds. Hitting a limit before
the requested frame is a failure.

## Automatic checks

```sh
python3 ports/doom/test.py --stage foundations
python3 ports/doom/test.py --stage portability
python3 ports/doom/test.py --stage engine --max-instructions 100000000000 --timeout 300
python3 ports/doom/test.py --stage kernels --timeout 300
python3 ports/doom/test.py --stage gameplay --timeout 300
```

Choose a separate `--artifacts DIR` to retain results from each stage.

| Stage | Evidence |
| --- | --- |
| Foundations | Native/Rust arithmetic and complete trig-table hashes; target libc; actual WAD/zone/hash lookup through the final disk block; decoded graphics cache and malformed patches; independent Python WAD/geometry/texture oracle; exact 320×200 TITLEPIC pixels |
| Portability | Native isolated engine tests for map parsing and malformed records, world/rendering arithmetic, and 66,000 consecutive logical tick grants |
| Engine | Native/Rust exact pixels and matching frame/tick/game-state logs for title, scripted menu-to-E1M1 startup, and direct E1M1 startup |
| Kernels | Independent Rust-VM oracles for RGB565 conversion, patch copying, high/low-detail columns and spans, cell-based memory operations, and signed fixed-point arithmetic; bank crossings, pointer tags, ABI preservation, guards and instruction budgets |
| Gameplay | 180 E1M1 frames with movement, turning, firing, use, running and automap toggles, plus 32-frame arrow-key and F5-toggle replays; every frame and frame/tick/game-state record compared with native C |

Engine and foundation runs use bank size 64,000, 256 memory banks, RNG seed 1, and a copied
WAD padded with zeros to a 65,536-octet storage-block boundary. The current VM
storage reader needs complete blocks; the logical WAD length remains unchanged.
Reports record source WAD and padded disk-image identities separately, along
with executable/replay hashes, bounds and termination reason.
Engine smoke-test image differences retain expected, actual and difference
PNGs; gameplay failures retain raw frames, logs and the first differing frame
in `results.json`. Native reference captures alone do not demonstrate that the
target engine works.

## Performance workflow

Rendering helpers convert the indexed frame to RGB565, copy patch columns and
draw textured columns/spans without repeated compiler-generated address and
spill work inside each pixel loop. Memory helpers preserve the target's 16-bit
C byte semantics and banked pointers. `FixedMul` and `FixedDiv` calculate the
same signed 16.16 results as the native C reference, including Doom's division
saturation guard. These routines execute ordinary Ripple instructions on the
Rust VM; they are also representable by the unchanged Brainfuck CPU.

Build both engines with the same initial detail setting, then measure an E1M1
replay and compare every frame against native C:

```sh
python3 ports/doom/build.py engine --target both --start-map
python3 ports/doom/tools/benchmark.py \
  --output ports/doom/build/perf-high \
  --profile-frame 20 --archive runtime/libruntime.par

python3 ports/doom/build.py engine --target both --start-map --low-detail
python3 ports/doom/tools/benchmark.py \
  --output ports/doom/build/perf-low \
  --profile-frame 20 --archive runtime/libruntime.par
```

The default replay is `tests/gameplay.json`, with 32 frames of movement,
turning and firing. For the longer workload, add
`--input ports/doom/tests/gameplay-long.json --frames 180`. The benchmark uses
already-built engines and never rebuilds them. Its native executable must
match the guest's initial scene and detail setting. Low-detail output is
compared with native low-detail output, not with high-detail pixels.

`results.json` records per-frame guest instruction counts, production-runner
wall times, exact native comparisons and artifact hashes. Steady statistics
exclude the startup frame. Optional `--profile-frame N` measurements run in a
separate replay, so PC-counting overhead is excluded from reported timings.
The analyzer recovers static function boundaries from matching `.pobj` labels;
omitted symbols are not silently charged to the preceding public function.
Keep the corresponding object files and runtime archive when retaining a
profile for later analysis. Avoid concurrent builds or benchmarks when
comparing wall times; guest instruction counts are independent of host load.

Consecutive local 32-frame E1M1 replay measurements (frames 2–32), recorded
before the keyboard-binding correction below:

| Build | Median instructions/frame | Median frame time | Frames/second |
| --- | ---: | ---: | ---: |
| Before guest-kernel optimization | 48,973,057 | 347.0 ms | 2.88 |
| Optimized high detail | 10,537,063 | 75.1 ms | 13.32 |
| Optimized low detail | 6,743,326 | 47.6 ms | 21.01 |

These runs used the same production VM driver and input script, sequentially,
with no concurrent agent builds. Other host activity was not controlled.
The measured speedups were 4.62× and 7.29×; the corresponding guest instruction
reductions were 4.65× and 7.26×. Timings vary considerably with host load and
are not a guarantee of windowed frame rate. Normal detail remains below Doom's
35 Hz target in this measurement. Artifacts and hashes are under
`build/playable-perf/verified-timing/`, with exact native comparisons in its
`high/` and `low/` subdirectories. The earlier binary predates the correctness
fixes below; both optimized binaries include them. The separate
`long-high-fixed/` and `long-low-fixed/` replays each match all 180 native frames,
and `detail-toggle-fixed/` matches all 32 F5-toggle frames. The final validation
also passes 12 foundations, title/menu/map smoke checks, four kernel suites,
414 compiler Rust tests and 199 C regressions (with a focused IRQ fixture rerun
after removing its obsolete reserved-mailbox check).

Those historical timing runs exercised movement and turning; Control/Space
events were present but their engine action bindings were missing. The current
port binds them explicitly and checks the actual movement, attack and use
commands in the `keyboard_controls` regression, in addition to pixel parity.

## Source layout and memory model

`vendor/doom-sources.json` pins pristine ignored checkouts. `build.py` verifies
them, applies the tracked `patches/*.patch` in filename order to an ignored
working copy, then builds native and/or Ripple binaries. All port changes belong
in patches or `src/`, never only in `vendor/` or `build/upstream/`.

- `src/` supplies the VM adapter, explicit WAD/patch decoders, private libc and
  existing-ISA assembly helpers. Native builds use the C reference paths.
- `include/` is only used by the Ripple build. Its legacy `uint8_t` spelling is
  a 16-bit cell containing an octet value, not an exact eight-bit C type.
- `tools/generate_tables.py` preserves upstream table values while splitting
  initialization into bounded functions and allocating tables on the heap.
- `tools/generate_info.py` does the same for all 967 animation states and 137
  object definitions, preserving action callbacks.
- `tests/` and `tools/check_assets.py` provide independent numeric, structural,
  malformed-input and image checks.

Ordinary C sizes count cells. Serialized WAD positions and record sizes count
eight-bit octets and are explicitly decoded. `fixed_t`/angles use 32-bit types;
large renderer arrays and the 8,388,608-cell Doom zone live on the banked heap.
The indexed screen and RGB565 output each occupy 64,000 target cells. Function
code can span multiple banks, while each function and the combined static data
must fit their respective 64,000-cell limits.

The assembly helpers are specific to this port profile: 64,000-cell banks,
320-cell framebuffer rows, normalized C pointers, and decoded texture samples
in `0..255`. Low detail relies on adjacent framebuffer columns. The wall-loop
cache relies on the stock drawing callbacks not inspecting `rw_x` or clipping
arrays during a column draw. Changing those contracts requires updating the
helpers and their oracles. The compiler's general pointer paths continue to
support other bank sizes.

Movement tests also caught two target-width bugs. The sky texture midpoint
uses `fixed_t`; storing `100 * FRACUNIT` in the
target's 16-bit `int` previously truncated it to zero. The movement replay and
`sky_widths` fixture cover the exposed sky rows that revealed this width bug.
Automap notifications use a 32-bit event field and explicit 32-bit message
constants, so entering/leaving the map refreshes the status bar correctly;
`event_widths` covers queue copying and discrimination from normal key events.

Short composite textures have a full 128-row sampling window, filled by
repeating their completed columns. Uncovered composite cells start at zero.
The original STEP6 cache had only 16 rows, allowing projection rounding to read
past its final column into heap metadata. Both builds now use defined texture
data at that boundary. `texture_columns` tests the actual cache routines with
short/tall textures, allocation guards, holes and unchanged masked posts.
The existing 65,536-cell composite-cache limit also applies to padded columns.

## Remaining scope

Save/load, demo recording, multiplayer, sound, mouse, host configuration files,
PWADs and melt wipes are deferred. Persistent file operations return errors;
they do not pretend to write saves. The bounded E1M1 replays provide repeatable
control and rendering coverage. Longer sessions, other maps, demo playback and
compatibility overflow cases still need broader validation.

No Brainfuck execution is part of these commands. Its device adapter and
multi-bank code export remain separate future milestones.
