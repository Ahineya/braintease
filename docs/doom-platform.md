# Doom platform boundary

The Rust VM is the development and playable-game target. The first Brainfuck
milestone is one correctly rendered Doom frame. Full Doom runs on the Brainfuck
backend remain outside the validation loop; its native device host is validated
with small assembly and C fixtures.

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

The Doom port uses the clock reading for its I_GetTime implementation and
RGB565 presentation from I_FinishUpdate. Tick acquisition belongs in the surrounding
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
another directory. The script uses Node.js for generating independent raw
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

The [`rbf` host](../rbf/README.md) implements ABI 1 over the BF CPU's strided RAM,
using explicit LOAD/STORE hooks. It reuses the Rust VM's descriptor handling,
logical clock, input replay, RGB565 display and capture code. All guest CPU work
continues to execute in Brainfuck. Small bounded tests cover both display modes,
bank crossings and logical tick/input behavior. The remaining single-code-bank
export restriction must be lifted in the BF CPU before attempting full Doom.

## Current Doom port

The complete adapted DoomGeneric source, shareware WAD, Make build, launcher
and tests live in [ports/doom](../ports/doom/README.md). Engine sources are
tracked directly under [engine/](../ports/doom/engine/); the upstream revision
and original file hashes are in [upstream.json](../ports/doom/upstream.json).
There is no external source checkout or patch-application stage.

The port implements the engine's I_* platform boundary and preserves the
original gameplay and software renderer. It handles 16-bit C bytes with explicit
WAD octet decoding, software 32-bit arithmetic, banked heap storage and bounded
canonical-table initializers. Its architecture, supported features, controls,
automated checks and manual launch commands are documented in the port guide.

## Test artifacts and reference policy

The port provides an automated native/VM comparison runner and a manual
launcher. Keep those checks passing when adding features or optimizing.

Store a compact bundle with the upstream revision, source identity, compiler
and binary identity, WAD hash, seed, resolution, clock policy, memory budget,
engine phase, map, skill, game tic, and presentation number. Include the PNG,
canonical raw RGB565 pixels, JSON result, and expected/actual/difference images
on mismatch. Frame number alone cannot identify a gameplay milestone because
startup, menus and wipes may all present frames first.

Build the native reference from the same tracked adapted engine sources, with the same deterministic scheduler, input script and RGB565
conversion. Compare logical state field by field (position, angle, health,
ammo, RNG index and selected world entities), not raw structures or pointer
addresses, since host and target layouts differ. Use an independent host WAD
reader to check serialization separately. Do not accept a new golden image
merely because a test failed; establish why the image changed and inspect it.

## Later Brainfuck gate

Full Doom BF execution remains outside the Rust port milestones. The device
host now has its own small checks; the next BF feasibility gate must address
the linker's existing multi-code-bank export restriction (see
[linker_main.rs](../src/ripple-asm/src/linker_main.rs)). Only then attempt a
bounded first-level-frame run. Playable BF speed is not an acceptance criterion.
