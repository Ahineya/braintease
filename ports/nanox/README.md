# Nano-X on Ripple

Nano-X now runs as a separate server on MMU FUZIX/RVM, with the upstream
client library, NanoWM and text-button `nxlaunch`. `nxterm` connects the shared
terminal model to Nano-X and a real guest PTY. RVM still presents one host
window containing the whole guest desktop. The Fuse-X native-window backend
is a later integration; these desktop executables are not installed there.

Run `scripts/run-fuzix.sh --visual`, then `startx` at the guest console.
NanoWM decorates the terminal and handles focus, stacking, title-bar dragging,
close buttons and corner resizing. The launcher opens additional applications.
Closing the last terminal leaves the launcher available. Closing the host
display ends the graphics session and returns to the original console; use
`shutdown` there to stop FUZIX cleanly. `gterm` and `nxdraw` remain available as
standalone tools; close them before `startx`, since GX has one display lease.

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

Run `scripts/run-fuzix.sh --visual`, then `nxdraw` at the original FUZIX console.
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
DMA path. Self-blits snapshot their source so disjoint clip bands cannot read
already-overwritten pixels. This initial implementation prioritizes correctness;
it does not yet accelerate local rasterization with DMA.

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
Each of at most twelve clients has bounded input/output queues. Partial reads,
slow readers, malformed packets and disconnects cannot block another client;
queue exhaustion disconnects that client. Queued events and resource counts
are bounded, and reparenting checks prevent cycles or excessive tree depth.
Object allocation at a resource limit returns a zero ID while leaving the
connection and existing objects usable.
Presentation remains damage-driven through the MMU-aware GX DMA upload path.
Offscreen pixmaps use RGB565 cell storage; optional image/shm/raw-input APIs
remain outside the enabled protocol profile.
Bitmap-derived regions and bounding window masks are enabled with checked
dimensions/payload lengths and the shared region quota. Shaped-child clipping
uses each child's actual region rather than subtracting its whole rectangle.

`startx` supervises the server, window manager and launcher. Service startup
uses readiness pipes rather than a fixed sleep. Active socket paths are not
replaced; stale Nano-X sockets can be recovered. Closing the desktop releases
the display and session resources so another `startx` can acquire them.

Build and verify without touching the user's disk:

```sh
make -C ports/fuzix BUILD="$PWD/build/fuzix" test-nanox -j4
make -C ports/fuzix BUILD="$PWD/build/fuzix" test-desktop -j4
make -C ports/fuzix BUILD="$PWD/build/fuzix" test-roach -j4
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
isolated 96 MiB disk. Logs and
PPM snapshots are retained under `build/fuzix/nanox/`.

The roach integration test compares all 24 orientations against independently
decoded original XBM pixels on a checkerboard, including child-window and edge
clipping. It also runs the real client under NanoWM, covers/reveals a stationary
coloured roach, checks animation, Ctrl-C/right-click cleanup, launcher startup
and desktop-session teardown. It uses its own private disk and stores captures
under `build/fuzix/nanox/roach/`. `test-desktop` runs both suites; `test-roach`
runs only the roach suite.
