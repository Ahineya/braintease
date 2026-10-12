# Doom on Ripple C

This is the real pinned Doomgeneric engine, adapted to 16-bit C bytes and the
existing Rust VM device interface. It uses the repository's shareware
`c-test/examples/doom/doom1.wad`; it does not download game data. Doom8088 is a
reference checkout, not the engine being built.

The first port profile is single-player, 320×200 RGB565, keyboard controls,
35 Hz logical simulation ticks and silent audio. The CPU instruction set and
Brainfuck implementation are unchanged. Development and smoke tests use only
the Rust VM.

The verified milestone is real engine startup: the title screen, scripted
menu-to-E1M1 startup, and direct E1M1 startup all produce exactly the same RGB565
pixels and frame/tick/game-state logs as the native reference. These checks run
`D_DoomMain` and the real world renderer. They do not substitute the asset viewer.
The first world capture currently takes roughly 25–35 seconds on the development
machine, including startup. Playable speed and sustained gameplay are future
milestones.

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
# Capture one frame and stop:
python3 ports/doom/run.py --scene e1m1 --headless
```

Use Escape/Enter/arrows in menus. In game use W/S to move, A/D to strafe,
Left/Right to turn, Control to fire, Space to use, Shift to run, Tab for the
automap, and number keys for weapons. Close the window to quit.

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
```

Choose a separate `--artifacts DIR` to retain results from each stage.

| Stage | Evidence |
| --- | --- |
| Foundations | Native/Rust arithmetic and complete trig-table hashes; target libc; actual WAD/zone/hash lookup through the final disk block; decoded graphics cache and malformed patches; independent Python WAD/geometry/texture oracle; exact 320×200 TITLEPIC pixels |
| Portability | Native isolated engine tests for map parsing and malformed records, world/rendering arithmetic, and 66,000 consecutive logical tick grants |
| Engine | Native/Rust exact pixels and matching frame/tick/game-state logs for title, scripted menu-to-E1M1 startup, and direct E1M1 startup |

Every Rust run uses bank size 64,000, 256 memory banks, RNG seed 1, and a copied
WAD padded with zeros to a 65,536-octet storage-block boundary. The current VM
storage reader needs complete blocks; the logical WAD length remains unchanged.
Reports record source WAD and padded disk-image identities separately, along
with executable/replay hashes, bounds and termination reason.
Image differences retain expected, actual and difference PNGs. Native reference
captures alone do not demonstrate that the target engine works.

## Source layout and memory model

`vendor/doom-sources.json` pins pristine ignored checkouts. `build.py` verifies
them, applies the tracked `patches/*.patch` in filename order to an ignored
working copy, then builds native and/or Ripple binaries. All port changes belong
in patches or `src/`, never only in `vendor/` or `build/upstream/`.

- `src/` supplies the VM adapter, explicit WAD/patch decoders and private libc.
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

## Remaining scope

Save/load, demo recording, multiplayer, sound, mouse, host configuration files,
PWADs and melt wipes are deferred. Persistent file operations return errors;
they do not pretend to write saves. Demo playback and game simulation require
further sustained replay coverage beyond startup. The initial milestone is a
correct first world frame and repeatable controls, not a claim that every map,
compatibility overflow case or long play session has been validated.

No Brainfuck execution is part of these commands. Its device adapter and
multi-bank code export remain separate future milestones.
