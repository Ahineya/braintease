# Shikaku

An original portable C/Nano-X rectangle puzzle game for Riplix and Fuse-X,
with the dark slate and gold presentation used by Klotski and Kolf.

Cover every cell with nonoverlapping rectangles. Each rectangle must contain
exactly one clue, whose value equals the rectangle's area. Every fifth
background grid line is solid and one pixel wide; ordinary cell lines are faint
and dashed. The outer border and rectangle outlines remain stronger.

## Playing

Run `shikaku &` in a graphical session, or select Shikaku in NXDSKTOP's Start
menu. `shikaku 15 --puzzle 3` opens a particular board. There are 12 original,
uniquely solvable puzzles for each of 5×5, 7×7, 10×10, 15×15, 20×20 and 25×25.
New generates a fresh seed-based puzzle of the same size. Generation proves
uniqueness before presenting the board, runs in small slices, and can be cancelled
with Escape without replacing your current board. `shikaku 15 --seed 12345`
recreates a generated puzzle; its seed is shown in the header. The Size picker
remembers the last puzzle or generated seed in each size, and reopening the app
restores the last board. The built-in 72 boards remain available via `--puzzle`.

- Drag from any corner to draw a rectangle; the preview shows width × height
  and area. Releasing replaces whole rectangles intersected by the new one.
- Right-drag, Ctrl-drag or Shift-drag removes intersecting rectangles.
- Incorrect rectangles are allowed as working notes, with red borders.
  Completion checks all clues, areas, overlaps and total board coverage.
- Undo/Redo or U/Y (also Ctrl-Z/Ctrl-Y) reverses edits. Clear asks for
  confirmation and can itself be undone.
- `+`/`-` or the toolbar buttons zoom. Fit/F fits the board to the window.
  Resize the window freely; automatic fit follows resizing until manual zoom.
- Wheel scrolls vertically, Shift-wheel horizontally, Ctrl-wheel zooms.
  Scrollbar tracks can also be clicked/dragged; Page Up/Down scroll vertically.
- Arrows move the keyboard cursor. Space anchors a rectangle, arrows extend
  it, Enter commits, and Escape cancels. Delete erases at the cursor.
- B opens sizes, N or `]` generates a new board, `[` opens the preceding
  built-in board, F1 opens help, Q quits.

Progress is saved separately for every puzzle under `$HOME/.shikaku`;
`SHIKAKU_HOME` selects another directory. Saves use checked textual coordinates,
a puzzle signature, temporary-file replacement and guest filesystem sync.
Undo/redo history is local to the current application session. Invalid saves
are preserved and automatic writes blocked until Clear is confirmed.

## Build and verification

```sh
python3 fuse-x/graphics.py --only shikaku nano-X
make -C ports/fuzix BUILD="$PWD/build/fuzix" shikaku "$PWD/build/fuzix/nano-X.rxe"
python3 ports/shikaku/generate.py --check
```

`generate.py` deterministically generates the original puzzles and proves each
has one solution using a separate Python exact-cover solver. `puzzles.json`
records clues, seeds and the verified tilings; `puzzles.inc` embeds them without
runtime solver cost. Reproduction: `python3 ports/shikaku/generate.py`.
`generator.c` uses a reproducible 32-bit random stream to partition the board,
place clues, enumerate candidate rectangles and prove uniqueness by exact cover.
The current generator mixes small regions with larger ones up to 40 cells, rather
than capping all initial regions at 14 cells. It preferentially refines smaller
ambiguous regions to retain larger clues. On boards of size 15 and above it
requires at least two clues of 12 or more, including one of at least 18; on boards
of size 10 and above no more than one eighth of the clues may be singletons.
Candidate boards that miss these variety checks are retried, with an explicit
failure after the bounded retry limit. These are distribution checks, rather than
a difficulty rating. The earlier generator has been replaced; its seed layouts
are not retained.

If there are two solutions, it splits a rectangle involved in the ambiguity and
checks again. A bounded search also refines over-expensive candidates. Seeds do
not depend on host word size, so Fuse-X and RVM recreate identical puzzles. This
proves solvability and uniqueness; it does not promise a human difficulty rating.

The C rules accept any valid tiling and do not compare the player's board to
an embedded answer. The solution data is used to construct clues and tests.

The controls are inspired by https://www.puzzle-shikaku.com/; no code, artwork
or puzzles from that website are included. New C sources are GPL-2.0-or-later.
`SHIKAKU_TRACE=1` enables diagnostic markers for guest integration checks.

Integration checks: `python3 ports/shikaku/tests/guests.py --target both` uses
private test disks, real GUI input, pixels, resize/zoom/scroll, persistence and
JIT execution. The rules test also exercises generated puzzles and reproducible
seeds on both guest targets. `tests/generation.py` separately checks runtime
C-generator output using the Python solver.
