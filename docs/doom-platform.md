# Doom platform boundary

The Rust VM is the development and playable-game target. The first Brainfuck
milestone is one correctly rendered frame; running full compiled C programs on
the Brainfuck backend is outside the current validation loop. No Brainfuck
execution is needed to develop or test this interface.

## Stable guest interface

The CPU instruction set stays unchanged. Host-device ABI 1 uses MMIO addresses
27–31 for banked descriptors. It supplies an RGB565 framebuffer, explicit
simulation-tick grants, a read-only logical clock, keyboard transitions, and
held-key state. The wire contract is in [the MMIO reference](../rvm/mmio.md#device-command-abi-version-1);
the C interface is [`vm_devices.h`](../runtime/include/vm_devices.h).

Mode 3 remains the existing bank-0 double-buffer device. Mode 4 (`RGB565_EXT`)
presents a banked, row-major buffer with 16-bit width/height/stride. A 320×200
RGB565 frame occupies 64,000 cells and may cross banks. Presentation copies the
completed frame, so the caller can reuse its buffer when the call returns.

Ripple C has 16-bit bytes. A framebuffer pixel and a descriptor word each occupy
one C byte; `long` and pointers occupy two. Serialized WAD octets still need
explicit decoding. Never reinterpret a WAD header as an in-memory C structure,
and never infer external file offsets from Ripple `sizeof` values.

## Game loop and input

Request one tick with `vm_next_tick()`, then process input and advance simulation.
The default is 35 ticks per second. A paced host waits for real time, an
unthrottled host grants ticks immediately, and a stepped host waits for explicit
permission to grant ticks. `vm_get_time()` reports logical time without advancing
it. Rendering and screenshot capture are independent of tick grants.

For a doomgeneric adapter, use the clock reading for its time callback and RGB565
presentation for its draw callback. Tick acquisition belongs in the surrounding
game loop, not in the clock callback: repeated time queries must remain harmless.
Keep simulation and presentation counts distinct, since a game can draw zero or
multiple frames during a logical tick.

Input events use USB HID keyboard usage IDs; translate them to Doom key codes in
the adapter. Both press and release are delivered. If the queue overflows, read
`vm_input_state()` and resynchronize controls from its 256-bit held-key snapshot.
That recovery discards remaining stale events and clears the overflow flag;
an ordinary state read leaves queued events untouched.
The same input protocol supports a windowed host and deterministic replay.

```c
#include <vm_devices.h>
#include <mmio.h>

/* Allocate pixels in banked guest memory before entering the loop. */
int run_game(unsigned short *pixels) {
    VmClock clock;
    VmInputEvent event;
    if (vm_devices_version() != VM_DEVICE_ABI_VERSION) return 1;
    display_set_mode(DISP_MODE_RGB565_EXT);
    for (;;) {
        if (vm_next_tick(&clock) != VM_DEVICE_OK) return 1;
        for (;;) {
            if (vm_input_pop(&event) != VM_DEVICE_OK) return 1;
            if (!event.available) break;
            /* Translate event.key and event.down into game input. */
        }
        /* Advance one simulation tick, then render pixels. */
        if (vm_present_rgb565(pixels, 320, 200, 320) != VM_DEVICE_OK) return 1;
    }
}
```

This sketch omits game-specific input-overflow recovery, shutdown and resource
cleanup. Production code must check capabilities and handle those cases.

## Visual validation

Capture after a successful presentation, at native resolution. A screenshot must
represent the front frame actually published by the guest, not a host window's
scaled contents. Frame limits and instruction budgets make infinite game loops
suitable for batch runs. Reaching a budget before a requested frame is a failure,
not an empty successful screenshot.

The initial smoke checks are the existing TITLEPIC demo and
[`test_vm_devices.c`](../c-test/tests/runtime/test_vm_devices.c). The latter checks
capabilities, clock purity, tick progression, empty input, and a 320×200
framebuffer spanning a bank boundary. Its image has a red upper half, blue lower
half, and distinct white/green/yellow/cyan corner pixels. Use fixed binaries,
disk fixtures and RNG seeds for image comparisons; compare decoded pixels and
save expected, actual and difference images on failure.

Build the toolchain, then run the Rust-only visual checks:

```sh
scripts/test-visual-smoke.sh
# Also build and capture the existing TITLEPIC demo twice:
scripts/test-visual-smoke.sh /path/to/DOOM1.WAD
```

Artifacts go to `c-test/build/visual-smoke`; set `VISUAL_SMOKE_OUT` to choose
another directory. The script requires Python 3 for generating independent raw
pixel expectations. It checks the C runtime API, cross-bank framebuffer colors,
stride padding, scripted press/release ordering, and identical results for
unthrottled and stepped execution. An optional WAD is copied before use.

For an existing guest binary, capture and stop at the first presentation:

```sh
target/release/rvm game.bin --headless --memory 2000000 \
  --disk fixture.img --seed 1 --clock unthrottled \
  --screenshot artifacts/frame.png --raw-frame artifacts/frame.rgb565 \
  --report artifacts/run.json --max-instructions 100000000 --timeout 30
```

`--headless` disables the window and live terminal/keyboard input. PNGs retain
native guest dimensions. Raw captures contain two little-endian host octets per
RGB565 pixel, without a header; dimensions are recorded in the JSON report.
`--capture-frame N` selects a one-based presentation number. Either screenshot
or raw capture requests stop after that frame. `--expect-frame baseline.rgb565`
requires `--screenshot` and compares exact pixels. On mismatch, a directory made
by replacing the screenshot extension with `.diff` contains `expected.png`,
`actual.png`, and `diff.png`. Baselines must use the same dimensions.

The report records success, stop reason, instruction/tick/frame counts, dimensions,
bank size, memory size, clock policy, seed, input overflow, and SHA-256 hashes of
the binary, disk and replay script. Preserve the report with screenshots when
investigating a regression.

`--max-instructions N` and `--timeout SECONDS` fail if reached. `--max-ticks N`
stops successfully before granting tick N+1, allowing the guest to render tick
N first. A requested screenshot must still be produced. `--exit-code` propagates
the guest's main return value after a normal halt. `--visual` enables the window
for interactive play; `--clock paced --tick-hz 35` paces the game clock.
`--frequency` throttles guest instructions independently.

## Input replay and stepping

`--input-script keys.json` reads a JSON array ordered by positive, nondecreasing
logical tick. Same-tick transitions retain their array order. Keys can be named
or numeric HID usage IDs:

```json
[
  {"tick": 1, "key": "W", "down": true},
  {"tick": 2, "key": "W", "down": false},
  {"tick": 2, "key": "ENTER", "down": true},
  {"tick": 3, "key": "ENTER", "down": false}
]
```

Replay events become visible at the corresponding tick grant. Polling input does
not advance them. Use `--clock unthrottled` for reproducible batch runs.

For external automation use `--clock stepped --control-stdin`. Standard input is
then reserved for JSON lines, not guest text input:

```json
{"op":"key","key":"W","down":true}
{"op":"step","ticks":1}
{"op":"quit"}
```

`step` grants the requested number of ticks before later control lines are
processed; `key` queues a transition for the next grant; `quit` ends the run.
Thus a piped key press, `step` of 2, key release, and `step` of 1 produce a press
at tick 1 and release at tick 3. A replay script is another way to assign events
to particular ticks. A stepped run with no credits remains suspended at
`NEXT_TICK`; EOF with no remaining credits is an error if the guest is awaiting
a grant. An external controller can grant many ticks at once or keep the pipe
open between steps.

## Brainfuck host adapter

The new device ABI is not yet implemented by the Brainfuck host. Future support
must honor the same descriptors, status results, tick yields, key IDs and
presentation boundaries. The first milestone can stop after one frame; it need
not offer playable speed. Device handling must remain outside guest CPU
instruction semantics so that the port's platform adapter can be reused.

## Pinned source checkouts

The source imports are present locally and ignored by the parent repository:

| Source | Checkout | Pinned revision | Intended use |
| --- | --- | --- | --- |
| [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric) | `vendor/doomgeneric` | `e257a09527062bfb986b6e5e0780839b6cbd5b71` | Main port candidate |
| [FrenkelS/Doom8088](https://github.com/FrenkelS/Doom8088) | `vendor/doom8088` | `b3073a2eb2811cd14caba6549694fc3bfcd6dd29` | 16-bit implementation reference |

The tracked [`vendor/doom-sources.json`](../vendor/doom-sources.json) is the source
lock. Recreate missing checkouts with `scripts/fetch-doom-sources.py`; verify
existing copies without network access with `scripts/fetch-doom-sources.py --check`.
The helper fetches exact commits, preserves upstream license files, and refuses
to overwrite a dirty checkout or a checkout at another revision. It never pulls
a moving branch into an existing source tree.

Keep these imports pristine. Adapter files, build rules and ordered
patches live in the tracked [`ports/doom/`](../ports/doom/README.md) directory; create the patched working
source under an ignored build directory. Do not leave the only copy of a port
change inside ignored vendor sources. WAD assets are separate from source
fetching; use the existing local shareware WAD as the first fixture and record
its hash. No additional game data is downloaded by the fetch helper.

## Recommended port scope

Use Doomgeneric as the main base. Doom8088 still assumes 8-bit C bytes, uses DOS
segment/EMS/XMS mechanisms, requires a transformed IWAD, and removes features
such as textured floors/ceilings and diminishing lighting. Those compromises
are not necessary consequences of Ripple's 16-bit instruction set. Borrow its
explicit-width arithmetic and selected memory optimizations only after testing
that they preserve the behavior we want. See its [README](../vendor/doom8088/README.md)
and [allocator](../vendor/doom8088/z_zone.c).

The first playable scope should be shareware E1M1, 320×200 RGB565, single player,
keyboard input, and silent audio stubs. Initially defer save/load, networking,
PWADs, mouse support and melt wipes. These are staged omissions, not a decision
to remove full Doom's rendering or simulation features. Keep the current CPU
ISA and 16-bit C-byte model. The existing cross-bank pointers and software
32-bit arithmetic are the starting point; no VM extender is proposed.

## Findings that determine the order

The following audit established the porting order. Implementation, launch
commands and milestone checks now live in [the port guide](../ports/doom/README.md).
These findings describe the constraints behind that work; the milestone tests
are the source of evidence about what has passed.

- **Widths and serialized data:** Doomgeneric uses `int` for `fixed_t`, sizes,
  offsets, time and some flags. `FRACUNIT` is `1 << 16`; changing one typedef
  does not repair intermediate expressions, `abs`, limits or format strings.
  Our target has no exact-width `uint8_t`. Use a port-local octet representation
  with values constrained to 0..255 and explicit little-endian decoding. Audit
  WAD headers/directories, map records, texture definitions, patches, palettes,
  blockmaps and demo records. Disk record sizes must be constants in octets,
  never target `sizeof(struct)`. Entry points include
  [m_fixed.h](../vendor/doomgeneric/doomgeneric/m_fixed.h),
  [w_wad.c](../vendor/doomgeneric/doomgeneric/w_wad.c), and
  [p_setup.c](../vendor/doomgeneric/doomgeneric/p_setup.c).
- **Global and initialization limits:** The linker currently requires all globals
  together to fit one 64,000-cell GP bank. At 320 pixels, `visplanes[128]` alone
  exceeds that; move large mutable arrays to the bank-spanning heap. Large
  initialized tables also become executable stores: a correctly widened
  `finesine[10240]` needs roughly 81,920 initialization instructions, beyond one
  function's code bank. Measure these before full linking. Prefer explicit heap
  allocation and a build-generated, bounded table-initialization/loading path;
  evaluate any generally useful compiler data-emission change as a separate,
  regression-tested task. Relevant code:
  [r_plane.c](../vendor/doomgeneric/doomgeneric/r_plane.c),
  [tables.c](../vendor/doomgeneric/doomgeneric/tables.c),
  [globals.rs](../rcc-backend/src/globals.rs), and
  [linker.rs](../src/ripple-asm/src/linker.rs).
- **Heap and stack:** Widen zone sizes and arithmetic. Specify the arena in C
  cells and derive host memory allocation separately. Ripple malloc can use
  banks 5..255; enabling that entire range requires at least 256 banks of VM
  memory (16,384,000 cells at bank size 64,000). A smaller test memory allocation
  needs a correspondingly bounded arena. Keep large local arrays off the
  single stack bank and report stack/frame limits with the memory budget.
- **Time and startup:** `doomgeneric_Create()` enters engine startup, which
  already runs tics before returning. `I_GetTime()` must return the VM's logical
  tick directly: tick 1 gives 28 ms, but converting that back yields tic 0.
  Adapt startup and the game loop to one grant per simulation tic, and drain
  the full input batch at that boundary. Wipe loops currently wait internally;
  disable them initially, then restore them as nonblocking tick-driven state.
  Clock reads must remain pure. See
  [d_main.c](../vendor/doomgeneric/doomgeneric/d_main.c),
  [d_loop.c](../vendor/doomgeneric/doomgeneric/d_loop.c),
  [i_timer.c](../vendor/doomgeneric/doomgeneric/i_timer.c), and
  [i_input.c](../vendor/doomgeneric/doomgeneric/i_input.c).
- **Video and host APIs:** Retain indexed-color rendering and convert the palette
  into a typed RGB565 buffer. Existing byte-pointer increments of two/four per
  pixel and hardcoded allocation `width * height * 4` do not match our C model.
  Use checked wide cell counts. Implement a small read-only WAD storage backend
  and the utility functions actually needed by the selected engine build;
  avoid making full filesystem support a prerequisite for the first frame.

## Proposed milestones and acceptance gates

Complete and keep each gate green before expanding to the next. A compiler or
runtime bug discovered along the way gets a minimal regression before engine
work continues. Existing `scripts/test-visual-smoke.sh` remains the platform
regression suite throughout.

| Milestone | Runnable deliverable | Automated acceptance | Manual acceptance |
| --- | --- | --- | --- |
| 1. Port foundation and reference | Tracked patch/build layout, native host reference, and a Ripple diagnostic binary using actual engine primitives | Width/literal audit; FixedMul/FixedDiv/SlopeDiv vectors and table endpoints against host results; module compile inventory; global, function/initializer and stack-size report; representative large table links within limits | Run the diagnostic and inspect its WAD identity, size units, memory budget and feature summary; launch the native reference |
| 2. WAD, zone and asset path | Engine-backed resource diagnostic/viewer using the new storage and decoding layer | WAD directory/lump checksums against an independent host reader; decoded map/texture/patch fields; bounds/truncation rejection; zone allocate/free/cache reload across banks | View the palette, full-size TITLEPIC, a texture and a sprite through the new loaders |
| 3. Real engine title and menu | Doomgeneric boots through D_DoomMain on the Rust VM with the explicit tic driver | Deterministic title/menu snapshots; exact logical ticks 1, 2 and 35; no hidden startup grants or clock polling deadlocks; scripted press/release batches; complete engine link with measured memory use | Navigate the real menu with Escape/arrows/Enter; verify releases and held keys |
| 4. First real world frame | E1M1 loads and renders a frozen, known starting view at a specified game tic, initially with monsters disabled | Map counts/start position match reference; GS_LEVEL reached; real R_RenderPlayerView completes; RGB565 frame and canonical camera/world checkpoint match native reference | Inspect walls, textured floor/ceiling, lighting, weapon, sprites and status bar side by side |
| 5. Deterministic world progression | Short scripted walkthrough driven by logical ticks | Turn, move, collide with a wall, use a door, fire and collect a pickup; compare player/world state and selected frames; identical results under stepped and unthrottled scheduling | Watch the replay, then perform the same actions with keyboard input |
| 6. Playable Rust slice | Keyboard-controlled E1M1 with enemies, combat, death/restart and level exit | Longer replay and repeated map loads; no memory growth; paced/unthrottled state equivalence; measured instruction cost, peak memory and achieved tic/frame rates | Play a complete short session, pause/use menus, die/restart and exit the level; assess responsiveness |
| 7. Broaden and optimize | Additional Episode 1 levels and a supported-feature list | Level/asset corpus; transition and nonblocking wipe tests; performance/memory budgets set from measurements; every optimization preserves replay and frame results | Play several maps and transitions; verify any newly enabled features such as saves or audio separately |

The first world-frame milestone is the first major port achievement. TITLEPIC
and the menu are valuable checkpoints, but their captures must not accidentally
satisfy the level-rendering gate. A normal title/menu boot already initializes
many renderer and asset subsystems, so the resource viewer provides useful
partial deliverables while those are being integrated.

For now, performance is measured rather than promised. Keep simulation at 35
logical tics per game second; if Rust cannot sustain 35 wall-clock tics per
second, profile before choosing optimizations or a lower display resolution.
Rendering may later run less frequently than simulation without changing the
recorded game-tic sequence.

## Test artifacts and reference policy

Every milestone should provide one automatic runner and one documented manual
launcher. Their exact commands are to be added with that milestone; the current
fetch and device smoke commands above already exist.

Store a compact bundle with the upstream revisions, patch identity, compiler
and binary identity, WAD hash, seed, resolution, clock policy, memory budget,
engine phase, map, skill, game tic, and presentation number. Include the PNG,
canonical raw RGB565 pixels, JSON result, and expected/actual/difference images
on mismatch. Frame number alone cannot identify a gameplay milestone because
startup, menus and wipes may all present frames first.

Build the native reference from the same pinned engine and relevant portability
patches, with the same deterministic scheduler, input script and RGB565
conversion. Compare logical state field by field (position, angle, health,
ammo, RNG index and selected world entities), not raw structures or pointer
addresses, since host and target layouts differ. Use an independent host WAD
reader to check serialization separately. Do not accept a new golden image
merely because a test failed; establish why the image changed and inspect it.

## Later Brainfuck gate

BF execution remains outside the Rust port milestones. Once a real level frame
works in Rust, the separate BF feasibility gate must cover both the device host
adapter and the linker's existing multi-code-bank export restriction (see
[linker_main.rs](../src/ripple-asm/src/linker_main.rs)). Only then attempt a
bounded first-level-frame run. Playable BF speed is not an acceptance criterion.
