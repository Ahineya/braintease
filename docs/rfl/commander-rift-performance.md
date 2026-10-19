# Commander and Rift performance

Commander should retain its panels between keys, and Rift should narrow the
text rows invalidated by an edit. Both also have avoidable work in shared string
and file adapters. Native execution makes Rift cursor movement fast, but does
not eliminate Commander's repeated screen construction or Rift's whole-viewport
editing cost.

The first optimization pass is implemented on both OS targets. Commander and
Rift now use the shared retained panel renderer; Rift consumes accumulated edit
damage and converges multiline syntax state; file loading, saving and copying
reuse buffers. Curses text conversion uses bounded synchronous scratch storage.
The baseline below was recorded on 2026-10-18 before these changes.

## Measurements

Run against already-built applications, without concurrent builds:

```sh
python3 rfl/tools/bench_tui.py --os both
python3 rfl/tools/bench_rift.py --os both --document /tmp/main.rfl
```

`bench_tui.py` creates disposable disks with 200 files in each Commander panel
(both panels initially show the same directory), and a 200-line highlighted RFL
source for Rift. It measures twelve selection changes, twelve scrolling steps,
twelve cursor motions and twelve successive inserted characters. Each sample
waits for changed content and the final cursor position. macOS reports child
CPU time as well as wall time and emitted terminal bytes. Neither program
touches the user's disk image.

The baseline used saved copies of existing app artifacts in
`build/rfl-perf/baseline/{fuzix,fuse-x}` and the installed kernels/VM, including
the preceding native image-cleanup optimization. These are observations of
those artifacts, not a controlled comparison against older compiler revisions.
`--artifacts DIR` reuses a saved set; each OS subdirectory needs `rift` and
`rfl-fm` with the appropriate `.rxe` or `.fxe` extension.

| Operation | RVM 80×24 | RVM 120×40 | Fuse-X 80×24 | Fuse-X 120×40 |
| --- | ---: | ---: | ---: | ---: |
| Commander startup | 12.93 s | 16.13 s | 5.59 s | 3.36 s |
| Commander selection median | 187 ms | 210 ms | 76 ms | 134 ms |
| Commander scrolling median | 278 ms | 143 ms | 75 ms | 146 ms |
| Rift startup with source | 11.91 s | 12.18 s | 0.75 s | 0.67 s |
| Rift cursor motion median | 54 ms | 30 ms | 3.8 ms | 6.2 ms |
| Rift typing median | 151 ms | 398 ms | 28 ms | 60 ms |

Commander selection CPU medians were 182/197 ms on RVM and 62/103 ms on
Fuse-X. Rift typing CPU medians were 145/309 ms and 27/56 ms respectively.
Only 201/241 terminal bytes were emitted per typical Commander selection change;
Rift typing emitted 128/129. There is substantial application work before the
terminal receives those small updates.

The smaller existing three-line Rift fixture measured 99/136 ms typing on RVM
and 5.3/8.5 ms on Fuse-X. Using only that fixture hides the cost of a full
viewport of highlighted text. RVM cold/JIT pauses are significant: full-source
typing samples peaked near 0.9 s. CPU and wall time, warm state and individual
samples should accompany future comparisons; one launch per geometry cannot
establish startup distributions.

These are headless terminal measurements. GUI terminal composition, desktop
repaint and host presentation require a separate measurement.

## Results after optimization

The same benchmark fixtures, installed kernels and VM produced the following
before → after medians, with compilation and integration tests finished before
the performance run. Each input measurement contains twelve samples.

| Operation | RVM 80×24 | RVM 120×40 | Fuse-X 80×24 | Fuse-X 120×40 |
| --- | ---: | ---: | ---: | ---: |
| Commander selection | 187 → 59 ms | 210 → 27 ms | 76 → 4.8 ms | 134 → 7.1 ms |
| Commander scrolling | 278 → 122 ms | 143 → 155 ms | 75 → 10 ms | 146 → 18 ms |
| Rift typing | 151 → 54 ms | 398 → 35 ms | 28 → 4.0 ms | 60 → 5.3 ms |

At 120×40, Commander selection CPU time fell from 197 to 26 ms on RVM and
103 to 7.1 ms on Fuse-X. Rift typing CPU time fell from 309 to 33 ms and
56 to 5.3 ms respectively. Typical emitted bytes remained identical to the
baseline: the gains come from avoiding work before presentation.

Commander startup changed from 12.93/16.13 to 8.56/9.41 s on RVM and
5.59/3.36 to 0.61/0.73 s on Fuse-X (80×24 / 120×40). Rift startup changed from
11.91/12.18 to 6.87/11.81 s and 0.75/0.67 to 0.60/0.51 s respectively.
Startup remains sensitive to cold code and host scheduling; these are individual
launches. RVM scrolling still repaints the active panel and encounters JIT
pauses; its larger-terminal median increased slightly in this run, so that path
does not establish a consistent improvement. Native scrolling improved at both
sizes.

Raw optimized observations are in `build/rfl-perf/optimized/tui.jsonl`.
The full Commander/OS and Rift integration suites passed on native Fuse-X and
RVM JIT. They cover file operations, binary copies, partial reads and EOF,
buffer copy-on-write, syntax/selection colors, text objects, undo/redo/dot,
save conflicts, themes, resize, dialogs, child interruption and terminal
restoration. The shared widget test also checks unchanged paints, selection-only
paints, listing invalidation and independent shared snapshots. The portable C
engine passed 1,200 randomized batches against fresh full renders across C/RFL,
Python and Pascal, with multiple edits before a frame, undo/redo and newline
changes, plus right-overflow checks with tabs.

## Commander baseline and changes

[`main.rfl`](../../rfl/apps/commander/main.rfl) previously marked the screen dirty after every
key, including unhandled keys and movement clamped at an endpoint.
[`ui.rfl`](../../rfl/apps/commander/ui.rfl) then cleared the complete virtual
screen and rebuilt both panels, status, selection path and function-key labels.
The shared [`tui_files.rfl`](../../rfl/lib/tui_files.rfl) fills every panel row,
rebuilds borders and headings, formats every visible name/size, and redraws the
scrollbar. Selection movement itself does not read the directory again.

`PanelPaint` and `draw_cached` now retain panel paint state in the shared file widget. For selection-only
movement, they repaint the old/new rows, affected scrollbar cells, count and selected
path. Commander retains the inactive panel, headings and function-key strip. A changed
viewport repaints only the affected panel. Theme, geometry, listing,
focus and return from overlays/tools invalidate the relevant caches.
Listing generations and explicit invalidation identify changes. Inputs with no
visible effect skip drawing.

Startup and reload have separate opportunities. Both panels previously loaded and sorted the
same directory independently. Matching paths now share a copy-on-write listing
snapshot while retaining independent selection/viewport state. Known file
operations refresh affected directories, including panels inside moved/deleted
trees. Explicit reload and return from arbitrary external tools refresh listings
to discover external changes.

Directory reads perform `lstat` per entry, build a full path and temporary C
string, then filter and Shell-sort a second vector. The comparator now borrows
entries and still compares names one indexed octet at a time. A batched directory/
metadata adapter remains a candidate for a separately profiled follow-up. Preserve directory-first ordering, inaccessible
entries, symlink handling and transactional refresh on errors.

## Rift

Rift already retains chrome and blank rows, paints text directly through the C
engine without a temporary RFL cell vector, skips lexing during ordinary cursor
motion, binary-searches keyword tables and transfers vi state to avoid copying
the dot-repeat recording. Keep these paths.

### Reuse outgoing lexical state

[`text_render`](../../libraries/text/syntax.inc) previously discarded the outgoing state
returned when painting a row. Before the following row, it lexed that previous
row again to establish its incoming state. A freshly invalidated 35-row viewport
therefore took 69 lexer calls.

The implemented change stores the painted row's outgoing state when extending
the valid prefix. This reduces those 69 calls to 35. Eighteen comparisons across
C/RFL, Python and Pascal fixtures produced identical cells, including selections,
multiline comments/strings, newline insertion/removal and repeated edits. The
count reduction alone is not an end-to-end speedup claim.

### Invalidate fewer text rows

[`ui.rfl`](../../rfl/apps/rift/ui.rfl) previously repainted every visible file row whenever
the buffer generation or selection range changed. `text_damage` now accumulates
edit bounds and propagates cached lexical state until the unchanged suffix
converges. Ordinary edits repaint only the affected rows. Newline changes
conservatively repaint the suffix. Undo/redo and all mutations since the previous
frame contribute to damage. Selection changes repaint rows touched by either
the old or new selection.

Long-line lexing still traverses the complete logical line, but overflow now
comes from that pass rather than a separate `visual(endline)` traversal. The UI
also reuses its cursor row/visual column within a frame. Clipping the
lexer alone is insufficient when later rows depend on its outgoing state.

For very many short lines, editing near the beginning shifts all later line
offsets in [`replace_raw`](../../libraries/text/text.c). Consider a chunked line
index or lazy offset deltas if a dedicated large-file benchmark justifies the
complexity. Preserve the existing gap buffer and undo semantics initially.

## Shared adapters and compiler

[`std/curses.rfl`](../../rfl/std/curses.rfl) previously created a temporary byte vector for
every `text()` call, copying clipped characters through a `push()` loop. Commander
invokes it for names, sizes, scrollbars, headings and footers on every repaint.
Conversion now uses 512-octet C scratch storage in synchronous bounded chunks,
preserving clipping and printable-ASCII replacement without per-label vector
allocation. No callback or suspension can occur while a chunk is borrowed.
`Canvas.glyphs` batches repeated line glyphs into one C adapter call.

[`read_bytes`](../../rfl/std/posix.rfl) fills a fresh vector through dynamic
`push()` calls up to the requested limit, reads into it, and trims through
`pop()`. An EOF read requesting 4096 bytes still initializes and then removes
4096 elements. Rift loading, save-conflict hashing and saving, plus Commander copying, now
use `read_into`/`write_prefix` with reusable 4096-octet initialized buffers.
The allocating API remains for callers that need an owned result.
The compiler already folds some fresh constant fill loops into bulk fills;
the dynamic `limit` in `read_bytes` does not match that optimization.

Rift's existing save conflict check intentionally hashes the target; keep that
protection. Improve buffering rather than replacing it with timestamp checks.

The app artifacts are substantial: approximately 16.5/33.0 MB for Commander/Rift
on RVM and 6.25/12.36 MB on Fuse-X. Investigate generated frame and managed-value
overhead, borrowing and small synchronous wrapper inlining with function-level
profiles. By-value managed fields normally retain their storage; they do not
necessarily deep-copy it. Do not infer copy cost solely from source syntax.

Curses already compares complete layers in batches, diffs changed rows and
emits only changed cells. Keep this behavior for overlapping/aliased windows;
the primary app optimization is to avoid reconstructing unchanged virtual cells
before curses performs that comparison.

## Completed scope and remaining work

The retained panels/chrome, no-op input handling, outgoing syntax cache, bounded
syntax propagation, reusable hot-path I/O, scratch text conversion, borrowed sort
comparator, shared duplicate listings and targeted refreshes are implemented.
Generated-wrapper inlining, batched directory metadata and a different large-file
line index remain profiling candidates rather than prerequisites for this pass.

Compare the same saved executables, kernel and VM before/after each stage. Check
selection, scrolling, focus, themes, resize, popup restoration and external file
changes on both OS targets. Rift checks must cover multiline syntax convergence,
selection, newline edits, undo/redo and save conflicts. Keep CPU, output bytes and
cold versus warm samples alongside wall time.
