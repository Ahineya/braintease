# Graphics on FUZIX/Ripple

The destination is a small, extensible desktop with graphical terminals, native
applications and eventually nxroach. RVM owns a guest desktop; Fuse-X will use
macOS top-level windows under one application identity. Applications should use
the same window/surface/event contract on either backend.

The current implementation provides one RGB565 display, a PTY terminal, and
the first Nano-X drawing-engine application (`nxdraw`). The Nano-X server and
client library are the next milestone.

* [Architecture and current contract](architecture.md)
* [Milestones and acceptance criteria](roadmap.md)
* [Nano-X audit and adaptation work](nanox-audit.md)
* [Nano-X provenance](../../ports/nanox/UPSTREAM.md)
* [Fuse-X desktop requirements](../fuse-x/desktop.md)
* [FuseDE desktop and application plan](../fusede/README.md)

## First terminal

```sh
scripts/run-fuzix.sh --visual
```

At the FUZIX console:

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
Use `shutdown` to stop FUZIX and flush its disk. RVM's ordinary close-to-stop
behavior remains the default outside this launcher; `--guest-window-close`
selects guest close events for a visual session.

The terminal defaults to 640×480 guest pixels / 80×30 cells. Its default host
window is 640×480 logical points, so it fits a laptop desktop; Retina backing
pixels are handled by macOS. It has a bitmap font, 16 colours, a US keyboard mapping, basic key repeat, ANSI cursor/edit/erase
operations, scroll regions and an alternate screen. It does not yet provide
Unicode, clipboard integration, scrollback, mouse reporting or window resizing.
The shared terminal model supports resizing both screens while preserving
their top-left contents, clamping saved cursors, clearing new cells and
resetting scroll margins. Connecting this to interactive window resize events
and PTY size notifications belongs to the Nano-X terminal integration.
It waits for graphics events and PTY readiness together, without a polling
sleep between keystrokes and shell output. Changed character spans are painted
individually, blank spans use DMA fills, and scrolling uses DMA copies. It uses
the MMU on RVM, the only supported FUZIX/Ripple build.


## Nano-X drawing engine

With the visual launcher running, enter `nxdraw` at the original FUZIX console.
Close `gterm` first so the demo can acquire the display. It draws text, colours,
lines, subtracted clipping regions, monochrome bitmaps and overlapping RGB565
blits using the actual Nano-X engine. Escape, Q or the window close button
releases the display and returns to the console. `nxdraw --check` runs the
engine checks and exits automatically.

This is the engine milestone, with no separate Nano-X server or window manager
yet. RVM retains one host window for the eventual guest desktop; Fuse-X's native
top-level windows remain future work. See the [port details](../../ports/nanox/README.md).
The first RVM desktop will use Nano-X's NanoWM and upstream launcher; a custom
desktop is later customization, not an intermediate implementation.

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
profile. Non-MMU FUZIX builds are retired. Native window behavior
also needs the visual close/reopen and keyboard checks described in the roadmap.
