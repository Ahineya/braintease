# Graphics on Riplix

The destination is a small, extensible desktop with graphical terminals and
native applications. RVM owns a guest desktop; Fuse-X uses
macOS top-level windows under one application identity. Applications use
the same window/surface/event contract on either backend.

The current implementation includes the Nano-X server/client library,
NanoWM, graphical terminals, Eyes, Clock and Roaches. NXDSKTOP supplies the
basic RVM desktop, with message boxes, file selection, a calculator and Tetris.
The [FuseDE plan](../fusede/README.md) describes the custom desktop direction.

* [Architecture and current contract](architecture.md)
* [Milestones and acceptance criteria](roadmap.md)
* [Nano-X audit and adaptation work](nanox-audit.md)
* [Nano-X provenance](../../ports/nanox/UPSTREAM.md)
* [Fuse-X desktop requirements](../fuse-x/desktop.md)
* [FuseDE desktop and application plan](../fusede/README.md)

## Nano-X desktop

Run `scripts/run-riplix.sh --visual`, then `startx` at the guest console.
Use the bottom-left Start menu to launch applications, select a file for Levee,
sync the disk, leave the desktop or shut down. Left-drag title bars to move
windows; left-drag the extreme bottom-right corner to resize supported clients.
`startx --minimal` retains the original text-button launcher. See the
[runtime guide](../../ports/nanox/README.md) for current apps and limitations.

On Fuse-X, `scripts/run-fuse-x.sh --gui` provides independent native windows.
Its `startx` keeps the native launcher/session model.

Client repaints now use retained buffers: a completed `GrFlush()` publishes the
window and its child widgets together, so synchronous colour/size queries do
not reveal half-drawn controls. Terminal updates send changed text spans and
copy pixels when scrolling. Calculator caches measurements and repaints only
the affected controls. Large RGB565 fills/copies use validated bulk operations
on both backends. See the [architecture](architecture.md#buffered-nano-x-repaints-and-bulk-raster-operations)
for the commit boundary, DMA/COW rules and memory budget.

## First terminal

```sh
scripts/run-riplix.sh --visual
```

At the Riplix console:

```sh
gterm
```

The new window runs an independent shell on a PTY. Try `ls`, `vi /notes`,
`top`, or `sokoban`. Shell job control remains PTY-based: Ctrl-C interrupts the
foreground job, Ctrl-Z stops it, and `jobs`, `bg`, and `fg` manage it. `exit`
closes the terminal and returns to the original console. `gterm COMMAND ARG...`
runs a particular command instead of a shell.

Choose a smaller terminal in character cells, for example:

```sh
gterm -g 64x20
gterm --geometry 64x20 -- vi /notes
```

The first example opens a 512×320-pixel terminal. The PTY reports the same
rows, columns and pixel dimensions to its child applications. Geometry is
bounded to 1–80 columns and 1–30 rows; invalid sizes are rejected before
acquiring the display. `--` ends terminal options.

For this milestone there is one graphics lease: a second simultaneous `gterm`
returns busy. Closing its host window requests hangup of its terminal session;
the kernel keeps running and the original console can start another `gterm`.
Use `shutdown` to stop Riplix and flush its disk. RVM's ordinary close-to-stop
behavior remains the default outside this launcher; `--guest-window-close`
selects guest close events for a visual session.

The terminal defaults to 640×480 guest pixels / 80×30 cells. Its default host
window is 640×480 logical points, so it fits a laptop desktop; Retina backing
pixels are handled by macOS. It has a bitmap font, 16 colours, a US keyboard mapping, basic key repeat, ANSI cursor/edit/erase
operations, scroll regions, an alternate screen and shared Unicode bitmap glyphs.
On Fuse-X, both terminals support mouse selection, Command-C/V/A and
Ctrl-Shift-C/V using the [shared text clipboard](../fuse-x/desktop.md#text-clipboard).
Scrollback and application mouse reporting remain follow-ups.
The shared terminal model supports resizing both screens while preserving
their top-left contents, clamping saved cursors, clearing new cells and
resetting scroll margins. The Nano-X terminal connects this to interactive
window resize events and PTY size notifications; standalone `gterm` retains
its fixed guest geometry.
It waits for graphics events and PTY readiness together, without a polling
sleep between keystrokes and shell output. Changed character spans are painted
individually, blank spans use DMA fills, and scrolling uses DMA copies. It uses
the MMU on RVM, the only supported Riplix build.


## Nano-X drawing engine

With the visual launcher running, enter `nxdraw` at the original Riplix console.
Close `gterm` first so the demo can acquire the display. It draws text, colours,
lines, subtracted clipping regions, monochrome bitmaps and overlapping RGB565
blits using the actual Nano-X engine. Escape, Q or the window close button
releases the display and returns to the console. `nxdraw --check` runs the
engine checks and exits automatically.

This standalone tool exercises the engine without a separate server or window
manager. The desktop uses the implemented Nano-X server, NanoWM and NXDSKTOP;
RVM retains one host window for the guest desktop. Fuse-X maps top-levels to
native Mac windows. See the [port details](../../ports/nanox/README.md).
The custom FuseDE shell follows the initial upstream desktop and widget work.

## Verification

```sh
python3 ports/nanox/tools/verify.py
python3 ports/graphics/tests/run.py
make -C ports/fuzix BUILD="$PWD/build/fuzix" test-graphics
make -C ports/fuzix BUILD="$PWD/build/fuzix" test-nanox
```

The graphics target uses private disks and JIT. An additional real-terminal
pixel/latency probe checks typed text, cursor movement, deletion, scrolling,
repeat cancellation and clean close without opening a host window:

```sh
FUZIX_TEST_BUILD="$PWD/build/fuzix" cargo test --release -p rvm \
  --test fuzix_graphics -- --ignored --nocapture
```

The probe prints startup and key-to-frame timings; these are observations, not
machine-dependent pass/fail limits. The normal kernel integration,
resource-pressure, job-control and networking suites cover the larger resource
profile. Non-MMU Riplix builds are retired. Native window behavior
also needs the visual close/reopen and keyboard checks described in the roadmap.
