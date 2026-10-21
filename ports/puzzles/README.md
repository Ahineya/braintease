# Portable Puzzles for Fuse X

All 40 finished games from Simon Tatham's Portable Puzzle Collection use
their original engines, generators, solvers, and midend through one shared
Nano-X frontend. Each game runs in its own resizable Fuse-X window. A small
collection launcher provides search and mouse/keyboard selection. New uses
the original generator; these are not scraped or fixed board collections.

## Build and run

From the repository root:

```sh
python3 ports/puzzles/build.py
python3 ports/puzzles/stage.py
scripts/run-fuse-x.sh --gui
```

Start the graphical session with `startx` if necessary, then run
`/mnt/host/games/puzzles.fxe`.
Type to filter the collection, use arrows to select and Enter or Play to
open a game. Clicking a selected game also opens it. Ctrl-U clears the search;
Escape clears it or closes the launcher. An asterisk marks games opened by
this launcher; a second launch is prevented until that game closes. Closing
the launcher leaves its games open. Each game restores its own saved progress.

The collection is optional: normal Fuse-X launches do not build it or install
it into `/bin`. Its explicit build uses `build/puzzles-fuse-x`, separate from
the OS artifacts. To rebuild selected programs, use, for example,
`python3 ports/puzzles/build.py --only puzzles net`. `catalog.json` is the
canonical list used by the optional build, launcher, help, and staging tool.
On the next normal image update, copies installed by the former automatic
build are removed only if they still match the old build artifacts. User
replacements, other hard links, and saved games are preserved. The old default
`/bin/puzzles` launcher entry is also removed without changing other entries.

The executables embed their guest libraries and font atlas. In an existing
graphical session, run `/mnt/host/games/puzzles.fxe`. It finds the collection
in the `puzzles/` directory alongside it. Individual shared executables are
available as `/mnt/host/games/puzzles/<game>.fxe`. Other games, such as Doom,
Kolf and Shikaku, remain outside this collection directory. The staging tool
atomically replaces executables so running older copies are not overwritten
in place. `puzzles.fxe --games DIRECTORY` can select another game directory.

The collection includes Black Box, Bridges, Cube, Dominosa, Fifteen, Filling,
Flip, Flood, Galaxies, Guess, Inertia, Keen, Light Up, Loopy, Magnets, Map,
Mines, Mosaic, Net, Netslide, Palisade, Pattern, Pearl, Pegs, Range,
Rectangles, Same Game, Signpost, Singles, Sixteen, Slant, Solo, Tents, Towers,
Tracks, Twiddle, Undead, Unequal, Unruly, and Untangle. Upstream's developer
`nullgame` and `unfinished/` experiments are excluded.

## Shared controls

The toolbar provides New, Type, Undo, Redo, Restart, Solve, Game ID, and Help.
Solve is disabled for games without an upstream solver. F1 provides the
selected game's objective and controls. The complete upstream manual source
is staged as `/mnt/host/games/puzzles/puzzles.but` beside the
shared executables with the licence and game catalog.
Type offers presets and Custom settings. Dialogs support mouse selection,
Apply/Back buttons, Enter to accept, and Escape to cancel. Custom fields use
Up/Down or Tab to select, typing/Backspace to edit, Ctrl-U to clear a text
field, and Left/Right or Space to change choices and checkboxes.
Numeric keypad digits retain upstream's keypad modifier (including Inertia's
diagonal moves), and keypad Enter works in games and dialogs.

| Key | Action |
| --- | --- |
| N | Generate a new puzzle |
| U or Ctrl-Z | Undo |
| Y or Ctrl-Y | Redo |
| F1 | Rules and controls |
| F2 | Presets and custom settings |
| F3 | Inspect or enter a game ID or seed |
| Ctrl-S | Save pending progress again |
| Q or Ctrl-Q | Close the game |

Resize the window to scale the board and its numbers. Game IDs use upstream's
`parameters:description` format; reproducible seeds use `parameters#seed`.
Typing in the Game ID dialog replaces the initially selected ID. Examples:

```sh
/mnt/host/games/puzzles/mines.fxe '9x9n10#123456789'
/mnt/host/games/puzzles/solo.fxe '3x3#123456789'
/mnt/host/games/puzzles/net.fxe '7x7w#987654321'
```

An ordinary launch restores the previous session. `--new` starts fresh using
the game's defaults. An explicit ID or seed also starts that requested game.

## Game controls

**Mines:** left-click opens a cell; right-click flags it. Clicking an open
number opens its remaining neighbours when enough adjacent mines are flagged.
Arrows move the cursor; Enter opens/chords and Space flags. The first click
is safe, and normal generated puzzles are solvable without guessing. Undo
can recover a mistaken mine click. Solve reveals the board and is undoable;
it does not count as a player win. This first port exposes classic square
boards, including the 9x9, 16x16, and 30x16 presets. The newer non-square
tilings are explicitly rejected pending a separate C16 geometry audit.

**Solo:** select a cell and type its number. Right-click selects pencil
marks; typing toggles a candidate. Arrows and Space also control selection
and pencil mode. Backspace or 0 clears an entry. Larger puzzles use letters
after 9. M fills pencil marks. Presets include difficulty levels, X, Jigsaw,
and Killer variants as provided by upstream.

**Net:** left-click rotates anticlockwise, right-click clockwise, and middle
click locks a tile. Shift-left emulates middle click; Ctrl-left emulates
right click. Arrows move the cursor, A/S rotate, and D or Space locks.
Connect every wire to the central power source. Wrapping presets connect
opposite board edges.

## Progress and rendering

Each game saves its board, elapsed time where applicable, and upstream undo
history in `$HOME/.puzzles/<game>.save`. Changes are saved after a short idle
period and again on close. Saves flush a temporary file before replacing the
previous save, then flush the directory update through the guest's synchronous
filesystem flush. Failed or corrupt saves produce explicit errors; an
unreadable save is preserved until the user deliberately starts a new game.

The frontend retains the game's incremental drawing in a server pixmap and
copies only accumulated damage to its window. Exposures restore those pixels;
they do not rely on games redrawing unchanged cells. Polygons use upstream's
scanline fallback; circles use horizontal spans. Text uses the existing ROM
font as one small, tintable FuseDE mask atlas, with server-side scaling.
Text batches use at most nine glyph rectangles, matching the FuseDE image
protocol even for longer Pattern clue strings.
Blitters retain pixmap snapshots with clipping. Timers run only while upstream
requests animation/accounting or an autosave is pending; idle games do not
repaint continuously. Generation remains synchronous, so large or unusually
hard puzzles can take longer to generate.

Clip regions remain allocated while bound to the drawing GC: Nano-X retains
the region ID, so destroying it immediately would let wires and other shapes
draw outside their tile and make incremental redraws depend on drawing order.

## Source and C16 adaptation

`source.json` pins official upstream commit
`616da16f25da428ccda805536209095bd062676e` (20260923.616da16) and hashes every
vendored file. `upstream/` contains unmodified sources and the MIT licence.
`adapt.py` verifies the hashes and writes adapted copies under the build
directory. The shared frontend and support code are MIT licensed; the reused
Nano-X ROM font retains the licence in `ports/graphics/fonts/LICENSE`, and
the linked Nano-X/FuseDE components retain their own licences.

The C16 adaptations widen candidate masks and packed render-state shifts,
31-bit random ordering keys, polygon fixed-point arithmetic, Mines coordinate
products, and Solo's generation step budget. A 16x16 Solo board needs a budget
of 256 squared, which would otherwise wrap to zero on C16. SHA-1 and random
seed data remain octet values even though a guest C char occupies 16 bits.
The compiler ABI is unchanged. Guest double math uses the existing accelerated
libm; the port supplies the float sqrt entry also requested by Mines.

RCC currently misparses an expression whose local variable hides an outer
typedef. The failing, correct-behaviour regression is
`c-test/tests/bugs/test_puzzles_typedef_shadow_expression.c`. Generated sources
rename the conflicting grid/vector typedefs until that parser defect is fixed.
Other source adaptations address parser/IR defects recorded by the red tests
`test_puzzles_pointer_typedef_tag`, `test_puzzles_sizeof_postfix`,
`test_puzzles_unreachable_after_return`, and `test_nested_struct_tag_completion`.
They rename a conflicting structure tag, parenthesize a sizeof operand,
terminate an unreachable IR block, and hoist Pearl's nested struct definition.
Upstream source files remain unmodified and hash verified.

This milestone targets Fuse-X. RVM build integration and printing are not
provided. The frontend uses guest POSIX and Nano-X APIs so future RVM work can
reuse it without replacing the games.

## Verification

```sh
python3 ports/puzzles/tests/engines.py
python3 ports/puzzles/tests/netdraw.py
python3 ports/puzzles/tests/gui.py
python3 ports/puzzles/tests/collection.py
python3 ports/puzzles/tests/collection_gui.py
python3 ports/puzzles/tests/install.py
```

Engine tests compare PRNG sequences and seeded board hashes against unmodified
upstream built by the host C compiler, then exercise solve/reveal, undo/redo,
saved history, and atomic rejection of a truncated save in real Fuse-X.
Net drawing tests compare the full drawing-command stream and retained tile
flags with unmodified upstream across rotations, animation, undo animation,
locks, wrapping, barriers, completion flashes, cursor display, and three tile
sizes. An unchanged board must issue no tile drawing commands. The packed
render state must remain 32 bits through comparison, drawing, and caching;
narrowing it to a C16 int loses high flags and causes spurious redraws.
GUI tests use actual window input and captured pixels to exercise moves,
resizing, menus, presets, custom validation, save/reopen, solving, new boards,
idle rendering, and three simultaneous game clients on a disposable root.
Net also checks that four rotations in either direction restore every board
pixel, that locking visibly changes a tile, and that unlocking restores it.
The collection probe compares every game's seeded generation, initial drawing,
and actual midend input drawing against unmodified upstream built by the host
compiler, then checks saved state/history. The collection GUI suite opens all
games through the launcher, captures their pixels, resizes them, exercises
input/help/presets, and reopens their saved sessions on a disposable root.
It also verifies the sibling `puzzles/*.fxe` layout and visible handling of a
missing executable. Installer tests use a temporary image with all 40 cached
game binaries present and verify that normal installation excludes them and
their documentation while installing core apps and preserving filesystem health.
The collection reference links the guest's qsort implementation: C leaves
equal-key ordering unspecified, and Undead's generator uses the resulting
order of equally long paths. Its seeds can therefore produce different valid
boards with another platform's libc. No engine changes are made to hide this
difference.

Project and full upstream rules:
[Simon Tatham's Portable Puzzle Collection](https://www.chiark.greenend.org.uk/~sgtatham/puzzles/).
