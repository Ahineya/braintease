# Nano-X on Ripple

Nano-X now runs as a separate server on MMU Riplix/RVM, with the upstream
client library, NanoWM and NXDSKTOP. `nxterm` connects the shared
terminal model to Nano-X and a real guest PTY. RVM still presents one host
window containing the whole guest desktop. Fuse-X provides native top-level
windows and independently built companion apps, retaining macOS window
management rather than running a guest desktop.

Run `scripts/run-riplix.sh --visual`, then `startx` at the guest console.
NanoWM uses Slate decorations and handles focus, stacking, title-bar dragging,
minimize/maximize/close buttons and border resizing. Alt+Tab restores and switches
between applications; Alt+Shift+Tab switches backwards. Border resizing commits
on release. `fdectl list` and its window actions expose the shared
[workspace service](../../docs/fusede/workspace.md). NXDSKTOP's bottom-left Start menu opens
additional applications. Closing the last terminal leaves the desktop available.
`startx --minimal` selects the original text-button `nxlaunch` instead. Closing the host
display ends the graphics session and returns to the original console; use
`shutdown` there to stop Riplix cleanly. `gterm` and `nxdraw` remain available as
standalone tools; close them before `startx`, since GX has one display lease.

## NXDSKTOP and companion apps

The upstream NXDSKTOP 1.1 menu/bar drawing is retained, with platform lifecycle
and process handling in `desktop_adapt.py` and `ripple/nxdsktop-platform.*`.
The Start menu offers About, Help, Calculator, Terminal, Edit file, Processes,
Clock, Eyes, Roaches, Tetris, Image Viewer, Sync disk, Exit to terminal and Shut down.
Mines and the world map remain later ports.

The taskbar reports physical free/total KiB through `/dev/proc` and wall time.
It blocks on Nano-X events, a one-second status timer and registered wake/result
pipes; unchanged status text is not repainted. Separate taskbar/popup windows
leave the root desktop available to Roaches. Panel work-area reservation and
full window task switching belong to later FuseDE work.

`nxmsg`, `nxselect`, `nxcalc` and `nxview` are independently installed companions.
The selector navigates directories and pages through up to 511 entries plus
the parent row. Select a file and click OK to open it in Levee. Closing the
selector or pressing Escape cancels; filenames are passed as arguments rather
than interpolated into a shell command. In Levee, Esc followed by `:wq` and
Enter saves and quits. Calculator supports basic decimal arithmetic and reports
division by zero. These three clients currently use fixed window geometry.

Exit to terminal ends the graphics session and returns to the original shell;
Shut down invokes the guest's orderly filesystem shutdown. Ordinary window
close actions do not stop Riplix. Child reaping, signal wakeups and dialog
results are handled outside signal handlers; desktop cleanup bounds termination
of its remaining child applications.

On Fuse-X, run `nxcalc &`, `nxselect`, or `nxmsg 'Title' 'Message'` after starting
its Nano-X service. They create native macOS windows, with dialog results on
stdout (`OK` or `[]`; a selected file produces its path). NXDSKTOP itself is an
RVM desktop client; native `startx` continues to open Applications and Terminal.
The [FuseDE plan](../../docs/fusede/README.md) tracks the remaining companion
apps, toolkit evaluation and custom desktop.

`fdeshowcase` is the first FuseDE colour/skin client on RVM and Fuse-X. It loads
Slate skins and a small Lucide icon atlas from `/usr/share/fusede/`, demonstrates
masked/tinted drawing, nine-slice scaling and keyboard/pointer control states,
and blocks without repainting while idle. The versioned C16 image extension
retains ownership, quotas, clipping and buffered repaint boundaries; upstream
image codecs remain disabled. See [usage, API and verification](../../docs/fusede/colour-resources.md).

## Tetris

Choose Start → Tetris on RVM, or run `nxtetris &` from a graphical terminal on
either platform. The original NanoTetris pieces, board and drawing run in a
fixed-size window; a game starts immediately.

| Key | Action |
| --- | --- |
| Left / Right | Move |
| Up / F | Rotate clockwise |
| D | Rotate anticlockwise |
| Down | Soft drop |
| Space | Hard drop |
| P | Pause / resume |
| C | Continue |
| N | New game |
| Q / Escape | Quit the game |

The mouse buttons provide the same controls. Pausing freezes gravity; movement
keys leave the game paused. A completed line earns 100 points. The best score
is saved in `$HOME/.nxtetris-score` as lines clear and on new game/exit, using
a temporary file and rename followed by disk synchronization. Scores use
32-bit arithmetic. `nxtetris --help` lists controls; `--seed 0..32767` gives a
repeatable piece sequence for testing.

Existing launcher configurations are preserved. To add Tetris to an older
minimal RVM launcher or Fuse-X Applications window, append
`Tetris - /bin/nxtetris` to `/etc/nxlaunch.cnf`; new configurations include it.

## Kolf mini golf

Run `kolf &` in either graphical environment. This native C/Nano-X
reimplementation embeds all eight original KDE Kolf courses (128 holes), with
all eleven obstacle types, moving platforms/gates, bridge layers and warps.
Press C to choose a course. Aim with the mouse towards the target, or use Left/Right (Shift/Ctrl for coarse/fine
steps). Hold/release the mouse, Space or Down for a visible putting stroke;
A toggles the circular three-click mode and Escape cancels a swing. Enter advances
after a hole, R retries, N starts a new round, P pauses and F1 opens help.
U or Ctrl-Z undoes the last shot, restoring scores and moving objects. Water
offers a default drop outside the hazard or a rehit from the last location.
The [port notes](../kolf/README.md) describe compatibility, physics and checks.

## Image viewer

Choose Start → Image Viewer on RVM, or run `nxview &` in either graphical
environment. With no filename it opens `nxselect`; it can also take one or
several files:

```sh
nxview /usr/share/netsurf/images/landscape.jpg
nxview /pictures/first.png /pictures/second.jpg &
```

Open / **O** / **Ctrl+O** opens another local file. **F** fits the image into
the window without enlarging small images; **1** shows actual pixels.
**+ / -** zoom around the visible centre. Drag or use arrows to pan;
the wheel pans vertically. **[ / ]** or PageUp/PageDown browse the filenames
supplied on the command line. **Q**, Escape or the close button quits.
Resizing recomputes fit geometry or clamps the existing pan. The toolbar
exposes the same viewing controls, and the status bar shows dimensions/zoom/path
or an opening/decoding error. Closing/cancelling the file picker and failed
loads leave the previous image available.

This is a new small frontend using `libraries/image`, rather than the upstream
demo's disabled server codec APIs. PNG, baseline JPEG, first-frame GIF and BMP
share NetSurf's bounded decoders and RGB565/alpha upload path. Transparency uses
a checkerboard. Retained images avoid repeat uploads when their resource budget
allows; larger pictures use the shared visible-tile fallback and existing
GX/DMA presentation. No browser process or network connection is required.
The initial limits are 1 MiB input, 262144 pixels, 4096 per dimension and 2 Mi
temporary decoder cells. Progressive JPEG, animated playback, SVG and editing
are outside this version. A picture above a limit reports an error rather than
changing process or server quotas.
Available memory can impose a lower limit, particularly within RVM's current
23-bank data/heap window. Input, decoded RGBA and decoder workspace coexist
during a load; memory exhaustion leaves the previous picture intact.

Normal builds install `/bin/nxview` on both systems. New minimal-launcher and
Fuse-X Applications configurations include Images; existing configurations
are preserved. Add `Images - /bin/nxview` to `/etc/nxlaunch.cnf` if desired.

`python3 ports/nanox/tools/test_viewer.py` checks real hosted and RVM/JIT
processes using private disks. It checks decoded PNG/alpha pixels, all four
formats, zoom/pan, native resize, picker cancellation, failed-load preservation,
idle rendering and clean exit/filesystems. Captures/logs are retained under
`fuse-x/build/nxview-tests/` and `build/fuzix/nxview-tests/`.
`tools/view-model.c` additionally checks fit/zoom/resize coordinate bounds with
a host undefined-behaviour sanitizer.

## Minimal launcher configuration

The launcher reads `/etc/nxlaunch.cnf`. Its initial configuration is installed
without replacing existing user configuration. Each line is `Name - COMMAND
[ARG...]`; this profile uses text buttons and compiled bitmap fonts. Nano-X
apps are independently compiled processes communicating through the private
`/tmp/.nano-X16` Unix stream socket, not linked into the server.

The first upstream GUI apps are installed as `nxeyes` and `nxclock`:

```sh
nxeyes &
nxclock &
```

Eyes follows the pointer and blinks using Nano-X timers. Drag an eye with the
left mouse button to move it; right-click an eye to close it. Its shaped window
leaves the desktop visible around the eyes. Clock displays OS wall time and
updates through timed event waits; use its title-bar close button to quit.
It retains the upstream fixed 200×200 geometry and disables resizing.
Fresh launcher configurations include Eyes and Clock buttons. Existing
`/etc/nxlaunch.cnf` files are preserved; add `Eyes - /bin/nxeyes` and
`Clock - /bin/nxclock` there to add the buttons to an existing disk.

`nxroach` brings the upstream roaches to the RVM desktop:

```sh
nxroach
```

Move a terminal to uncover them: they scurry over the desktop and hide under
windows. Press Ctrl-C in the launching terminal to stop them, or right-click
the bare desktop. The latter also stops a copy started from the launcher.
Options include `-roaches 1..64`, `-speed 0..48` (pixels per frame),
`-rc RGB` (numeric `0xBBGGRR`) and `-seed 0..32767` for repeatable movement.
The defaults retain ten black roaches and the upstream speed of 20.
Fresh launcher configurations include `Roaches - /bin/nxroach`; add that line
to an existing configuration if desired.

The port retains upstream movement, turning and collision logic. All 24 XBM
orientations are converted from LSB-first octets into MSB-first C16 bitmap
cells. Transparent `GrBitmap` drawing preserves the desktop and server-side
window clipping; no background readbacks or orientation pixmaps are needed.
The pixmap ceiling therefore remains 16. Active animation is capped at 20 fps;
when every roach is hidden, the client blocks until a window/exposure event or
stop request; `-speed 0` also freezes turning and waits without repainting.
Signal handlers set an exit flag and post to a nonblocking wake pipe, closing
the race between checking the flag and entering an indefinite wait. Cleanup clears old
sprite rectangles without creating a temporary full-screen window.

[UPSTREAM.md](UPSTREAM.md) records the revision, archive checksum, licenses and
verification commands. Keep `upstream/` pristine; place future source adaptations
in an explicit patch layer and platform adapters alongside it.

Run `scripts/run-riplix.sh --visual`, then `nxdraw` at the original Riplix console.
Escape, Q or closing the window returns to the console. Close `gterm`/the desktop first:
the engine demo and graphical terminal currently share one exclusive display.

`adapt.py` generates sources in the build directory, with checked replacements
for C16 bit widths, memory sizes, bitmap strides, 32-bit dash masks and the ROM
font selection. The ROM font's Latin-1 comments are transcoded to UTF-8.
`ripple/nxr_config.h` selects RGB565, dynamic regions and compiled fonts, disabling
optional image loaders, rotation and international shaping. The desktop enables
monotonic application timers; the standalone engine demo omits them.
`ripple/screen.c` implements raster operations, word-based monochrome conversion
and GX presentation. Framebuffer/pitch units are cells, one per RGB565 pixel.
Drawing happens in guest memory; dirty rectangles upload through GX's MMU-aware
DMA path. Self-blits use a reserved 640-by-16-pixel scratch stripe, traversed
in overlap-safe order, so disjoint clip bands cannot read overwritten pixels.
Moving a large window does not allocate a second window-sized snapshot.
Fully visible copies and nonoverlapping rectangles bypass scratch storage.
Large fills and RGB565 copies use GX_RASTER_FILL/COPY on validated process RAM:
RVM resolves COW pages and uses DMA, while Fuse-X uses page-aware host bulk
operations. Small or unsupported operations retain the software path. Opaque
8-pixel font rows use direct stores instead of per-pixel division and masking.

The standalone demo handles display close/Escape/Q through `select()`. The
server uses the upstream software cursor and Nano-X event delivery, translating
GX HID/pointer/focus events to portable Nano-X symbols. Held keys repeat after
500 ms and then every 50 ms; focus loss clears held keys. `nxterm` forwards
control characters to its PTY, retaining shell job control and editor behavior.
Both graphical terminals initialize their PTY erase character to DEL (127),
matching the Backspace key they send, so raw-mode editors such as Levee and
canonical line input agree with the terminal.
Resize updates the terminal model and PTY dimensions, including SIGWINCH.

`server_adapt.py` generates checked server/client/NanoWM/launcher adaptations.
`apps_adapt.py` adapts the upstream demo clients without changing the import.
Eyes reuses the existing software floating-point and math libraries; Clock
keeps upstream's integer trigonometry, with guards for uninitialized hand angles.
The C16 protocol counts target cells and uses matching target struct layouts,
not upstream's octet ABI. Connections must open with the versioned C16 magic;
requests are capped at 4,096 cells and checked against an explicit allowlist,
fixed structure sizes and variable payload lengths before upstream dispatch.
Each of at most twelve clients has bounded input/output queues. One receive
stages multiple requests and dispatches at most 32 before yielding; complete
staged packets remain ready even if the socket has no new data. Partial reads,
slow readers, malformed packets and disconnects cannot block another client;
queue exhaustion disconnects that client. Queued events and resource counts
are bounded, and reparenting checks prevent cycles or excessive tree depth.
Object allocation at a resource limit returns a zero ID while leaving the
connection and existing objects usable.
`paint_adapt.py` adds exposure-preserving backing buffers to client windows,
including child widgets. Public `GrFlush()` commits their accumulated dirty
rectangles together; automatic transport flushes and synchronous queries do not
publish incomplete repaints. The C16 handshake is versioned with this extension,
so rebuild server and clients together. NanoWM decorations remain direct to
avoid unnecessary container buffers; its direct-mode flag is sent during the
initial connection handshake. Ordinary exposure-driven windows fall back to
direct drawing when automatic backing-store allocation fails; explicitly
requested buffered windows still report failure. Resized backing pixmaps retain their
window owner's lifetime. The fixed system-colour table is cached by clients.
Stretched theme strips sample a single source column or a uniform source row
once per scanline, retaining exact nearest-neighbour sampling and mask semantics.
A focused JIT RVM benchmark of five 28-by-16 theme strips stretched to 560-by-400
pixels took 3,404 ms with the previous sampler and 528 ms with the optimized
sampler on the development host (6.4 times faster), with identical checksums.
This measures theme rasterization rather than complete desktop frame latency.

`nxterm` compares cells with its rendered shadow and sends only changed text
spans. Scrolling copies existing pixels and moves the shadow, while explicitly
restoring the old cursor and drawing the new one. Calculator caches font/label
measurements, redraws only the display and pressed/released button on input,
and reserves full redraws for exposure. Its original arithmetic is retained.

Presentation remains damage-driven through the MMU-aware GX DMA upload path.
Offscreen pixmaps use RGB565 cell storage; optional image/shm/raw-input APIs
remain outside the enabled protocol profile.
Bitmap-derived regions and bounding window masks are enabled with checked
dimensions/payload lengths and the shared region quota. Shaped-child clipping
uses each child's actual region rather than subtracting its whole rectangle.

`startx` supervises the server, window manager and desktop. Service startup
uses readiness pipes rather than a fixed sleep. Active socket paths are not
replaced; stale Nano-X sockets can be recovered. Closing the desktop releases
the display and session resources so another `startx` can acquire them.

Build and verify without touching the user's disk:

```sh
make -C ports/fuzix BUILD="$PWD/build/fuzix" test-nanox -j4
make -C ports/fuzix BUILD="$PWD/build/fuzix" test-desktop -j4
make -C ports/fuzix BUILD="$PWD/build/fuzix" test-roach -j4
make -C ports/fuzix BUILD="$PWD/build/fuzix" test-nxdsktop -j4
make -C ports/fuzix BUILD="$PWD/build/fuzix" test-tetris -j4
python3 ports/nanox/tools/verify.py
```

The JIT test checks C16 layout, empty/subtracted regions, clipping, high dash
bits, XOR, font metrics, clipped 17-pixel bitmaps, offscreen/self-overlapping
blits, Escape/Q/close event wakeups and display close/reopen. It compares every displayed pixel against an
independent Python model. The captured frame is `build/fuzix/nanox/engine.rgb565`.

The desktop integration test runs independent client/timer, malformed-frame
and resource-exhaustion probes, then drives actual NanoWM/launcher/terminal
processes using RVM's live input API. It checks shaped eyes, pointer tracking,
blink timers, advancing clock hands and both apps' close controls, plus
shell-to-disk I/O, Backspace in
Levee's insert/command modes and shell/canonical input, both terminals' erase
settings, multiple PTYs,
movement/resize, focus after resize, close buttons, reopening a terminal after
closing the last one, session close/reopen and filesystem consistency on an
isolated 192 MiB disk. Logs and
PPM snapshots are retained under `build/fuzix/nanox/`.

The roach integration test compares all 24 orientations against independently
decoded original XBM pixels on a checkerboard, including child-window and edge
clipping. It also runs the real client under NanoWM, covers/reveals a stationary
coloured roach, checks animation, Ctrl-C/right-click cleanup, launcher startup
and desktop-session teardown. It uses its own private disk and stores captures
under `build/fuzix/nanox/roach/`. `test-desktop` runs all desktop suites; `test-roach`
runs only the roach suite.

The Tetris test drives a seeded game through rotation, both walls, hard drops
and a complete 12-cell line. It compares board pixels, checks monotonic gravity,
keyboard/mouse pause, signal wakeup, both launchers, score persistence and a
40,000-point score across desktop sessions. Its disposable disk is checked after
shutdown; logs and a demo capture live under `build/fuzix/nanox/tetris/`.

The repaint regression (`tools/test_desktop.py --only paint`) leaves a parent
and child partly repainted, issues a synchronous query, then lets an independent
client draw. It checks that the previous committed colours remain visible until
an explicit flush publishes both new colours. It retains three full frame buffers
(screen, window and offscreen pixmap), then checks disconnect and idle-server
shutdown. A nonblocking wakeup pipe prevents a stop signal from being lost
between the stop check and an indefinite `select()`. The native graphics suite checks
the same behavior. Shared GX raster probes cover bank crossings, strides,
guards, invalid/overlapping spans and scattered COW pages in a forked child.
