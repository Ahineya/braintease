# Graphics on FUZIX/Ripple

The destination is a small, extensible desktop with graphical terminals, native
applications and eventually nxroach. RVM owns a guest desktop; Fuse-X will use
macOS top-level windows under one application identity. Applications should use
the same window/surface/event contract on either backend.

The first implementation is deliberately limited to one RGB565 window and one
PTY terminal. Nano-X is pinned and imported for adaptation, but its server and
applications are not yet built or installed. Importing the source is not a
claim that upstream Nano-X already works on Ripple.

* [Architecture and current contract](architecture.md)
* [Milestones and acceptance criteria](roadmap.md)
* [Nano-X audit and adaptation work](nanox-audit.md)
* [Nano-X provenance](../../ports/nanox/UPSTREAM.md)
* [Fuse-X desktop requirements](../fuse-x/desktop.md)

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

For this milestone there is one graphics lease: a second simultaneous `gterm`
returns busy. Closing its host window requests hangup of its terminal session;
the kernel keeps running and the original console can start another `gterm`.
Use `shutdown` to stop FUZIX and flush its disk. RVM's ordinary close-to-stop
behavior remains the default outside this launcher; `--guest-window-close`
selects guest close events for a visual session.

The terminal is fixed at 640×480 guest pixels / 80×30 cells. Its default host
window is 640×480 logical points, so it fits a laptop desktop; Retina backing
pixels are handled by macOS. It has a bitmap font, 16 colours, a US keyboard mapping, basic key repeat, ANSI cursor/edit/erase
operations, scroll regions and an alternate screen. It does not yet provide
Unicode, clipboard integration, scrollback, mouse reporting or window resizing.
It waits for graphics events and PTY readiness together, without a polling
sleep between keystrokes and shell output. Changed character spans are painted
individually, blank spans use DMA fills, and scrolling uses DMA copies. It uses
the MMU on RVM, the only supported FUZIX/Ripple build.


## Verification

```sh
python3 ports/nanox/tools/verify.py
python3 ports/graphics/tests/run.py
make -C ports/fuzix BUILD="$PWD/build/fuzix" test-graphics
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
