# Klotski for Riplix and Fuse-X

GNOME Klotski 3.38.2's 28 original sliding-block puzzles, rules and blue tile
artwork, adapted to portable C and Nano-X. Both systems run the same code.
Start graphics with `startx`, then select **Klotski** from the desktop menu or
run `klotski &`. `klotski 28` opens Sunshine; `klotski --list` lists all puzzles.

| Control | Action |
| --- | --- |
| Left mouse drag | Slide a piece; coalesced motion follows legal unit steps |
| Tab / Shift-Tab | Select the next / previous piece |
| Arrow keys | Slide the selected piece (first piece selected if necessary) |
| C | Puzzle picker; Left/Right changes group, Up/Down selects, Enter plays |
| [ / ] | Previous / next puzzle, preserving each puzzle's progress |
| U / Ctrl-Z | Undo one grid step |
| Y / Ctrl-Y | Redo one grid step |
| R | Restart, with confirmation; best score remains |
| S | Save again, including after a disk error |
| F1 / ? | Help and credits |
| Escape | Close a dialog or clear selection |
| Q / window close | Save and quit |

Move the marked piece onto the green target. Only that piece can pass through
door cells. All original irregular shapes are supported. The move counter
follows GNOME's consecutive-piece grouping: multiple slides of the same piece
count as one move, and returning it to its group origin removes that move.
Undo/redo operates on individual grid steps without changing that scoring rule.

Progress, undo/redo and best scores are saved separately for every puzzle in
`$HOME/.klotski/level-NN`. `current` remembers the last puzzle. Set `KLOTSKI_HOME`
to an existing-parent alternative directory for independent profiles. Moves are
saved after keyboard input or the end of a mouse drag; closing/focus loss also
finishes a drag. Saves use a temporary file, flush and rename, with the guests'
filesystem-wide synchronous flush before and after replacement (host unit
checks use fsync). There is no
cross-instance merge: use separate profiles for concurrent games.

The journal is a versioned text format with a puzzle signature. Loads validate
all bounds and replay every step before replacing the live board. A malformed
save is retained and automatic writes are blocked for that puzzle until the
user explicitly confirms Restart. Save errors are visible and retain progress
in memory. An unreadable `current` selection only falls back to puzzle 1.

Windows resize and scale the board, including 19x19 Ithaca and 17x22 Sunshine.
The minimum content size is 480x340; tiles use even sizes from 2 to 48 pixels.
The original SVG is baked into a small RGB565 atlas, uploaded once and scaled
into one server pixmap when tile size changes. Normal drawing uses bitmap
copies and only repaints changed cells, changed joins, selection and status.
The event loop blocks while idle. No GTK, GLib, Vala, SVG runtime or floating
point is required. History grows from 128 steps to a bounded 16384 steps;
reaching the limit reports an error without changing the board.

## Build and check

```
python3 ports/klotski/assets.py --check
python3 fuse-x/graphics.py --only klotski
make -C ports/fuzix BUILD="$PWD/build/fuzix" klotski -j4
cc -std=c99 -Iports/klotski ports/klotski/tests/engine.c \
  ports/klotski/engine.c ports/klotski/store.c -o /tmp/klotski-check
/tmp/klotski-check
python3 fuse-x/cc.py ports/klotski/tests/engine.c ports/klotski/engine.c \
  ports/klotski/store.c --static -o fuse-x/build/apps/klotskicheck.fxe
make -C ports/fuzix BUILD="$PWD/build/fuzix" \
  "$PWD/build/fuzix/klotskicheck.rxe" "$PWD/build/fuzix/klotskictl.rxe"
python3 ports/klotski/tests/guests.py --target both
```

Normal graphics/userspace builds install `/bin/klotski` and documentation.
The executable embeds all puzzles and artwork, so copying it needs no asset
directory. Builds do not modify running disk images.

The standalone C check covers all boards, irregular pieces, doors and targets,
grouped counting, history growth/branches, undo/redo, malformed saves and a
complete independently generated solution of the original first puzzle.
`tests/solution.json` records that 32-grid-step, 27-grouped-move trace; it is a
test fixture, not a solver feature or a claim of an optimal score. Real guest
tests use private disks, pointer/keyboard input, captured pixels, resizing,
idle-frame checks, a real win and best-score restoration across shutdown/reboot.
RVM acceptance uses the JIT; artifacts are in `build/klotski-tests/`.

`assets.py --raster` regenerates the committed artwork with Python's standard
library. `raster.py` handles only the pinned SVG's rectangle/polygon/translation
and SourceAlpha shadow subset; it is an offline asset tool, not a general SVG
renderer. It uses supersampling and composites onto the dark board background.
The result retains upstream shapes/colours with slight rasterization differences.
Ordinary builds need no regeneration. Sources and raster are hash-checked.

## Provenance

`source.json` pins the official GNOME 3.38.2 release archive and the unmodified
source subset under `upstream/`. Original author: Lars Rydlinge; Vala ports:
Robert Ancell and John Cheetham. The C rules and move counting are adapted from
`puzzle.vala` and `puzzle-view.vala`; puzzle definitions come from
`klotski-window.vala`. Artwork is `data/assets.svg`. GPL-3.0-or-later; see COPYING.
This is an independent Nano-X adaptation, not an official GNOME release.
