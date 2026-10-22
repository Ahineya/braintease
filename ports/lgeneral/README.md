# LGeneral for Fuse-X

This port builds the original LGeneral 1.4.4 engine against the reusable
[SDL 1.2 software port](../sdl/README.md). The included free K.u.k. General
1.1.1 content provides 22 First World War scenarios and a branching campaign.
No Panzer General retail data is included or required.

The profile is a resizable desktop application with PCM sound effects for Fuse-X. It
retains LGeneral's menus, bitmap artwork, rules, built-in AI, scenario setup,
unit movement/combat, purchasing, saves and campaign progression. Fullscreen
and dynamically loaded AI modules are disabled. Music playback is not included.

## Build and try

From the repository root:

```sh
python3 ports/lgeneral/build.py --target fuse-x --fetch
python3 ports/lgeneral/build.py --target fuse-x --stage fuse-x-shared
```

The second command stages the game, its `sdl-mixerd.fxe` audio helper, data and
license notices. Keep both executables in the share. It does
not modify any disk image. Inside Fuse-X:

```sh
/mnt/host/lgeneral.fxe
```

The executable finds the adjacent `lgeneral/` directory. An explicit override
is also available:

```sh
/mnt/host/lgeneral.fxe --data-dir /mnt/host/lgeneral
```

For a system installation, put the game in `/bin/lgeneral`, the helper in
`/usr/bin/sdl-mixerd` (or `/bin/sdl-mixerd`), and the
contents of `build/lgeneral/data/` in `/usr/share/lgeneral/`. `LGENERAL_DATA`
overrides that default; `--data-dir` takes precedence over the environment.
Settings and saves live under `$HOME/.lgames/`, with `/tmp` as the fallback
when `HOME` is unset. The asset directory remains read-only.

Use the right-hand toolbar to load a scenario or campaign. Choose the
`kukgen` scenario folder or the `kukgen` campaign. Left click selects a unit
and a destination; right click opens its information/actions. The toolbar
provides end turn, strategic map, save/load and settings. The original
confirmation and briefing screens use their displayed buttons. Window sizes
start at 800×600 and support up to 1920×1080, subject to the graphics backend.

LGeneral requests 150% native-window zoom so its original bitmap text is
readable on dense displays. Fuse-X reduces the zoom if needed to fit the
monitor. The game still draws an 800×600 surface initially; mouse coordinates
and resizing use that same logical geometry. Other applications keep their
usual size. Override the zoom for this launch with an integer percentage from
100 to 300:

```sh
SDL_GX_WINDOW_ZOOM=100 /mnt/host/lgeneral.fxe
```

This requires a Fuse-X kernel with `GX_PRESENTATION_ZOOM`; restart Fuse-X after
updating the kernel. Older backends run at their usual size. LGeneral draws its
own bitmap cursors with SDL's separate software cursor disabled, preventing
intermediate cursor updates from exposing the underlay of translucent dialogs.

## Sound effects

GUI, movement and combat effects use the reusable
[SDL_mixer-compatible PCM profile](../sdl/mixer/README.md). The original settings
control sound and volume, saved as `sound_on` and `sound_volume` in
`$HOME/.lgames/lgeneral.conf`. Unsupported or missing individual effects fail
without taking down the interface. An unavailable or busy PCM device leaves the
game playable without sound.

WAVs are decoded and converted when loaded, including the movement effect at
10,989 Hz. The decoder accepts legacy final data chunks whose odd trailing pad
is missing or outside the declared RIFF length; incomplete sample payloads are
still rejected. Audio uses 22,050 Hz stereo with the original 256-frame blocks.
Loading a scenario also loads
and converts its sound resources.

The Fuse-X build uploads effects to a separate producer process, which owns
voices and `/dev/pcm`. It continues refilling audio while the game draws or
waits for I/O. The game finds a staged helper beside its executable, or the
installed helper at the paths above; `SDL_MIXER_HELPER` overrides that pathname.
If the helper cannot start or acquire PCM, the game remains playable silently.
The host reference build retains the local cooperative mixer.

This uses the existing process scheduler and introduces no guest threads or
x64 kernel changes. Long synchronous kernel/host operations can still delay
both processes. See the
[threading investigation](../../docs/arch/audio.md#sdl-threading-investigation).

## Port structure

- `PROVENANCE.json` pins both upstream archives and their SHA-256 hashes.
- `prepare.py` generates a separate source tree and platform profile.
- `abi.py` gives game integers their original 32-bit semantics while keeping
  SDL/libc declarations at the SDK's ABI. It also translates formatted output
  arguments, including optional AI diagnostics.
- `engine_adapt.py` fixes compact-resource lengths, parser errors, scratch
  buffer lifetimes and other representation assumptions.
- `graphics_adapt.py` accesses SDL logical octets explicitly and handles
  bitmap fonts, sprite mirroring and reduced-size unit artwork.
- `terrain_adapt.py` shares identical weather artwork and fog images within
  each terrain type, with explicit ownership and cleanup.
- `save_adapt.py` uses the portable LGC1 save format, independent of pointer
  size, C structure packing and character-cell width.
- `assets.py` merges free runtime content and deterministically normalizes
  all 90 BMPs to uncompressed BGR24 without changing their RGB pixels.

The save format deliberately rejects upstream native-layout saves, whose
pointer placeholders differ between 32-bit and 64-bit hosts. It validates
lengths and references before loading. A successful write is committed by
renaming a completed temporary file, preserving an existing save on failure.

English bitmap fonts are used in this profile. This does not add general
Unicode text rendering to LGeneral. The upstream K.u.k. package includes some
scenario-specific data warnings; these are distinct from loading failures.

## Verification

```sh
python3 ports/lgeneral/test_assets.py \
  --engine build/lgeneral/pristine/engine/lgeneral-1.4.4 \
  --data build/lgeneral/pristine/data/lgeneral-data-1.1.1
python3 ports/lgeneral/test_graphics_adapt.py \
  --source build/lgeneral/pristine/engine/lgeneral-1.4.4
python3 ports/lgeneral/test_cursor_adapt.py \
  --source build/lgeneral/pristine/engine/lgeneral-1.4.4 \
  --sdl-source build/lgeneral/sdl/pristine/SDL-1.2.15
python3 ports/lgeneral/tests/test_saves.py
python3 ports/lgeneral/build.py --engine-test --build build/lgeneral-engine
python3 ports/lgeneral/tests/run.py --all-scenarios
python3 ports/lgeneral/tests/gui.py
python3 ports/lgeneral/tests/audio.py
python3 ports/lgeneral/tests/shared.py
python3 ports/lgeneral/tests/presentation.py
```

The engine test runs real scenario parsing, movement and combat through the
action queue, flag capture, save/reload, an AI turn, and campaign advancement.
`--all-scenarios` also loads every distributed scenario with its map, nations,
unit library and graphics. The GUI test exercises the original selectors,
mouse input, scrolling, resizing and clean exit. Both guest runners use private
offline disk images and check the filesystem after shutdown.

The audio test compares the original GUI click with an independent, exact PCM
and volume oracle in both channels, loads scenario effects including the
10,989 Hz resource, checks persisted mute, and closes and reopens the game.
It captures the muted PCM sink; physical speaker latency is not measured.
The shared-launch check runs the staged game from `/mnt/host` without an
installed helper, proving adjacent helper/data discovery and exact click PCM.

The milestone verification loaded all 22 complete scenarios in Fuse-X and
passed 40 engine checks, including byte-identical save/load/overwrite and
unchanged AI rankings. The GUI checks compare complete screenshots, including
resized layouts. Weather sharing and the SDL RGB24 path reduced the measured
Bosnia load from about 24.5 seconds to 5.4 seconds on this host; startup was
about 1.5 seconds. These are observations, not timing requirements.

A host reference build uses the same game adaptations and upstream SDL dummy
video driver:

```sh
python3 ports/lgeneral/build.py --target host --engine-test --build build/lgeneral-host
mkdir -p build/lgeneral-host/home
HOME="$PWD/build/lgeneral-host/home" \
LGENERAL_DATA="$PWD/build/lgeneral-host/data" SDL_VIDEODRIVER=dummy \
LGENERAL_TEST_ALL_SCENARIOS=1 build/lgeneral-host/lgeneral-test
```

## Portability and licenses

The game and software SDL layer do not depend on Cocoa or Fuse-X window
objects. GX sits behind the SDL driver boundary. A future Riplix x64 backend
can reuse this engine and content when its graphics/input service is ready;
it will require its own SDK build and verification. No bare-metal x64 game
executable is claimed by this port.

The engine is GPL-2.0-or-later; utility notices are retained. The free K.u.k.
content is CC-BY-SA-3.0 with its authors and notices in `data/licenses/`.
SDL and the original repository mixer implementation are LGPL-2.1-or-later;
the mixer is a bounded compatible API, not an upstream SDL_mixer release.
Normalized bitmap files are identified in the asset
manifest with their source and output hashes. The build receipts record the
executable hash and adaptations; this directory contains the corresponding
port sources. Distributing a binary also requires distributing the applicable
notices and corresponding source under those licenses.
