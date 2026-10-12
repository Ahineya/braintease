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

## Source import and first port milestone

Keep imported upstream code separate from the Ripple platform adapter, pin its
source revision, and retain its license. Use doomgeneric as the main integration
candidate and a 16-bit Doom port as a reference for integer widths and memory
organization. Importing a reference does not require adopting its hardware layer.

Before enabling gameplay, establish a reproducible build, WAD octet-reading
helpers, explicit 16/32-bit game types, a memory budget for globals and the zone
allocator, and a deterministic first rendered frame. Existing compiler tests
reduce risk but cannot establish that an uncompiled upstream codebase is fully
supported. Resolve compiler or runtime gaps exposed by the actual source in
small regression-backed changes.
