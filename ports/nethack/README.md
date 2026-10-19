# NetHack 3.6.7 guest port

NetHack's Unix terminal interface and a Nano-X tile frontend, compiled by RCC
for native Fuse-X and for Ripple/RVM. It is a separate optional game; the normal system launchers and
personal disk images are not changed.

## Build

From this directory, with the repository's RCC, assembler, Fuse-X kernel and
Riplix SDK already built:

```sh
make data       # bootstrap symbolic headers, build guest tools, generate C16 data
make fuse-x     # build/fuse-x/nethack.fxe
make fuzix      # build/fuzix/nethack.rxe
make tiles      # target glyph map + original RGB565 tiles
make fuse-x-nx  # build/fuse-x-nx/nethack-nx.fxe
make fuzix-nx   # build/fuzix-nx/nethack-nx.rxe
make stage      # copy native executable and assets into ../../fuse-x-shared
```

The release archive is vendored and SHA256-verified by `bootstrap.py`:
`98cf67df6debf9668a61745aa84c09bcab362e5d33f5b944ec5155d44d2aacb2`.
It came from <https://www.nethack.org/download/3.6.7/nethack-367-src.tgz>.
`upstream/` is the pristine extracted source. All source overlays and generated
headers go into ignored `build/source/`; `build.py` records the port changes.
A host Clang is needed for preprocessing and the initial symbolic-header
bootstrap. Neither executable runs host game code.

`generate.py` boots a new disposable Fuse-X disk, runs target-built `makedefs`,
extracts its target layout fingerprint, rebuilds the level/dungeon file writers
against that fingerprint, and then runs those writers in the guest. This order
is essential: host-generated binary levels have incompatible C layouts.
The generated disk is `build/generator/disk.img`. No normal user disk is read.

## Run and writable files

Run `/mnt/host/nethack.fxe` with `nethack-data/` beside it. The installed layout
can instead use `/bin/nethack` and `/usr/share/nethack/`. `-d` and `NETHACKDIR`
override the data directory. See [REFERENCE.txt](REFERENCE.txt) for controls.

The terminal must be at least 80×24. The game works from the console, mux, or a
desktop terminal; no graphical desktop is needed.
Arrow keys move in the four cardinal directions. Both ANSI CSI and VT
application-cursor sequences are decoded, including when stdio has buffered
the sequence or it arrives in fragments. The active NetHack direction alphabet
is respected (letter, numeric, or telephone keypad). Bare Escape has a bounded
150 ms ambiguity delay. In menus/prompts, cursor keys cancel instead of
accidentally selecting a letter; menu navigation remains NetHack's ordinary
letter/page controls. Unsupported complete CSI/SS3 keys also cancel, while
Meta combinations and incomplete sequences retain their literal bytes.

- Configuration: `$HOME/.nethackrc`, with upstream `NETHACKOPTIONS` support.
- Saves: `$HOME/.nethack/save/`.
- Scores, bones, locks and logs: `$HOME/.nethack/`.
- State directories use mode 0700; newly created state files use 0600.
- Character names are limited to 19 characters so uid/name/level filenames fit
  the guest filesystem's 30-character component limit.

NetHack's permanent death and save-and-exit behavior are preserved. No external
mail or shell integration, external compression program, or setuid installation
is required. Pattern options use the upstream `pmatchregex` wildcard fallback;
the guest libc does not provide the POSIX `<regex.h>` API.

## C16 adaptations

The guest has 16-bit `char`/`int`, 32-bit `long` and logical pointers, and 64-bit
`long long`, on both targets. The port therefore:

- converts preprocessed K&R function definitions to C99 and uses unwidened
  function prototypes;
- casts the level compiler's variadic arguments according to `add_opvars`'s
  format, avoiding int/long argument-slot mismatches;
- explicitly decodes signed packed octets and widens coordinate shifts before
  packing them into 32-bit level operands;
- widens the engine allocator's size parameter to `size_t`;
- uses the guest's real libc declarations and terminal interface;
- uses link-based file locking rather than unsupported fcntl record locking;
- supplies NetHack's integer-only stream scanning and calendar conversion needs;
- links the RVM software floating-point helpers for the few floating operations
  in the engine.

Static binary assets are generated as native C16 cells, then wrapped as
`NHC16v1\n` followed by little-endian uint16 cells. `config/asset.c` decodes that
stream for NetHack's `dlb_*` interface. This preserves high cells when assets
are stored on the host's octet-only shared filesystem. Text help/data files stay
ordinary octet files. Save files use the normal C16 guest filesystem directly;
they are not compatible with ordinary host NetHack saves.

## Tests

```sh
python3 test_normalize.py
python3 test_io.py native
python3 test_io.py ripple
python3 test_io.py native --packed
python3 test_io.py ripple --packed
python3 test_io.py native --keys
python3 test_io.py ripple --keys
python3 test_runtime.py native
python3 test_runtime.py native --shared --home /home/tester --keys
python3 test_runtime.py ripple --keys
python3 test_runtime.py native --levels
python3 test_runtime.py ripple --levels
```

The I/O regression round-trips every C16 character value, including `0xffff`,
which cannot be used as an EOF sentinel in binary block transfers. The gameplay
test starts a character, advances a turn, saves, shuts down the guest, reboots,
and restores that character at the same turn. Its private host-share mode also
checks launching beside the asset folder and using a non-default home. The
`--keys` variant answers a gameplay direction prompt with an actual arrow-key
sequence before saving and restoring. The terminal probe separately covers
all arrows in both terminal modes, letter/numeric/phone layouts, prompt input,
stdio lookahead, fragmented keys, bare Escape and Meta-key preservation. The
special-level test uses a private wizard account to enter and save from the
Oracle level. Directory and save-file permissions are checked after shutdown.

Build and runtime artifacts are deliberately kept out of the ordinary app list.

## Nano-X tile frontend

Start the desktop with `startx`, then run `/mnt/host/nethack-nx.fxe &` from a
terminal. Keep the same `nethack-data/` beside the executable, including
`nxtiles`. The installed RVM layout uses `/bin/nethack-nx` and
`/usr/share/nethack`. Normal application lists, launchers and personal disk
images are not changed by this optional build.

The frontend uses the original NetHack game engine and all 1,082 upstream
16×16 tiles. `tiles.py` builds and runs upstream `win/share/tilemap.c` inside
a disposable C16 guest, then extracts its 5,976 glyph mappings and dungeon-wall
substitutions. The textual tile palettes are converted into a little-endian
RGB565 asset with an explicit header; no host-ABI binary structs are used.
The source tile files and license remain in `upstream/win/share/`.

`nx/window.c` implements NetHack's windowport interface: character selection,
map glyphs, message history, two status lines, text/help windows, inventory,
single/multiple selection, quantity prefixes, text input, direction questions,
and extended commands. It keeps configuration and game state under `$HOME`.
The terminal executable remains available separately.

`nx/ui.c` uses the same C/Nano-X client path as Kolf. Tile strips are uploaded
through the existing bounded image protocol into a server pixmap once; map
changes use `GrCopyArea`. Messages/status have independent damage regions.
The game blocks on events while waiting for input. It does not run a frame
loop or redraw the screen continuously.

The window can be resized. At 16 pixels per tile the full 80×21 logical grid
would require 1280×336 pixels before messages/status; on smaller windows a
viewport follows the map cursor/player. Menus and help wrap to the available
width and support scrolling. Text uses the existing 8×16 font. There is no
zoom or animated interpolation in this first frontend.

- Arrows move; Home/End/PageUp/PageDown move diagonally during gameplay.
  Letter and numeric direction layouts remain available.
- `i` opens inventory, `?`/F1 opens help, Ctrl-P opens message history, `#`
  enters an extended command, and `S` saves and exits.
- In menus, use arrows and Enter or an item's letter. Space toggles a focused
  item in multiple-selection menus; Enter accepts and Escape cancels.
  Digits prefix an item count, and `:` searches a menu. Standard selection
  controls (`.` all, `-` none, `@` invert, `,` page) are supported.
- Text/help viewers scroll with arrows/PageUp/PageDown; Space advances a page.
- Left-clicking the map supplies NetHack's usual map-click/travel action.
  Clicking an item chooses/toggles it.
- Closing a live game's window requests the ordinary save confirmation after
  unwinding any open menu/prompt. Declining keeps the game open.

Graphical acceptance uses fresh disks and a private, public-data-only host
share; it never boots or copies a personal image:

```sh
python3 test_graphics.py native
python3 test_graphics.py ripple
```

The native test includes resize, close/save, character selection, graphical
restore and save/restore round trips through the terminal frontend. The RVM test uses its real JIT,
Nano-X server, keyboard events and framebuffer. Captured frames and logs live
under `build/graphics-native/` and `build/graphics-ripple/`.

Fuse-X reaches its first playable window in about 2.3 seconds in the acceptance
fixture. RVM reaches its first command in about 32 seconds with its JIT enabled;
the large guest executable and graphics setup still leave room for performance
work. These are local test measurements, not a target hardware guarantee.

## License

This is a modified NetHack distribution. See [upstream/dat/license](upstream/dat/license)
for the NetHack General Public License and the individual source files for their
copyright notices. The release archive and the port sources are retained here.
