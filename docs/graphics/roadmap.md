# Graphics milestones

## 1. Foundation and first terminal — implemented

* Pin Nano-X, retain source/licenses, and record C16/RCC and native-window blockers.
* Define window/surface handles, synchronous RGB565 operations and typed events.
* Expand protected process/file/PTY capacity with explicit memory placement.
* Add one exclusive RVM window and a PTY-based graphical terminal, with DMA
  upload/scrolling, ownership checks and exit/exec cleanup.
* Separate guest window close from VM shutdown.

Acceptance: parser tests; real RCC build; JIT guest tests for MMU-translated
uploads, overlap copies, bounds/stale/foreign handles, owner cleanup, high FDs
and all PTYs; frame capture compared with an independent pixel model; shell
commands in a graphical PTY and clean disk restart. Native UI opening, input,
close/reopen and editor interaction require a visual smoke check as well.

Validated on 2026-10-17: the JIT pixel/ownership/FD/PTY and persistence probe,
MMU exhaustion/recovery, job-control/editor, resource/Sokoban and networking
suites passed. Non-MMU kernel builds have since been retired. Visual macOS checks covered typing,
Levee editing and suspend/resume, terminal close, fresh terminal reopening and
clean shutdown. The RVM library suite passes 122 tests, including close-event
forwarding/overflow, modifier recovery and first-frame/window visibility.

Importing Nano-X alone did not enable nxroach; its adapted RVM client is now
implemented in milestone 4 below.

## 2. Readiness and several windows

Descriptor readiness is implemented: `select()` covers files, pipes,
TTYs/PTYs and sockets on both targets, plus RVM graphics events. Gterm uses it;
`SO_ERROR` supports asynchronous TCP completion. See the
[interface and verification](../networking/select.md).

Checked Unix-domain stream IPC and independent service descriptors are now
implemented on both targets, with readiness, bounded buffers and disconnect
cleanup; see [local IPC](../networking/local-ipc.md). Fine monotonic deadlines
for `select()`/`usleep()` and a 1 ms userspace monotonic clock are implemented on
both targets, independently of coarse accounting and IRQ frequency; see
[timers](../../ports/fuzix/TIMERS.md). Choose bounded message
framing in C16 cells explicitly. Use the independent local endpoint pool for
guest drawing messages. Internet
capacity has also grown to sixteen TCP connections and four listeners. Measure
real terminal/editor memory and latency before raising further limits.

Window management is built around Nano-X in the next milestone. On RVM, the
server owns the existing single GX display and composes guest windows into it;
multiple kernel display leases or host windows are not prerequisites. Fuse-X
maps top-levels to independent native windows/surfaces.
Implement titles, close negotiation, size changes, PTY SIGWINCH, recovery after
clients die, and resource exhaustion/reuse. Add terminal scrollback, proper key
repeat timing, clipboard/text input and a deliberate detached GUI policy.
Closing all terminals must leave the kernel able to accept a new terminal.

## 3. Adapt Nano-X deliberately

The initial drawing engine is implemented in `nxdraw`: RGB565, the compiled
8x16 ROM font, dynamic regions, clipping, raster operations and blits. C16
adaptations are generated alongside the pristine import. The JIT probe checks
engine behavior and all 307,200 displayed pixels against an independent model,
including clipped text and multi-rectangle self-overlap. See the
[engine implementation and tests](../../ports/nanox/README.md).

The separate server/client milestone is implemented on RVM: upstream Nano-X
runs over a bounded, versioned C16 Unix stream protocol, with upstream NanoWM
and text-button `nxlaunch`. `startx` supervises the session and waits for server
and window-manager readiness before launching clients. `nxterm` uses the shared
terminal model and a real PTY, including resize/SIGWINCH and shell job control.
RVM keeps one GX desktop display and composes guest windows into it. There is
no temporary custom window manager. See [build, operation and tests](../../ports/nanox/README.md).

Timer sampling and ticks use `CLOCK_MONOTONIC`, normalized C16 timeval arithmetic
and exact millisecond conversion. One select waits for GX, local client input,
bounded queued output and the nearest timer/key-repeat deadline. Presentation
is damage-driven and uses the MMU-aware GX DMA path. Request/event/resource
bounds and client disconnect cleanup keep ordinary broken clients isolated.

The real-kernel JIT test exercises independent clients, timers, partial and
malformed frames, launcher-created terminals, shell-to-disk I/O, NanoWM movement
and corner resize, updated PTY dimensions, display close/reopen and clean disk
shutdown. Full optional image/font/protocol coverage and the Fuse-X native
window/surface lifecycle backend remain further milestones. The latter must
retain native macOS window management rather than composing a second guest
desktop inside a host window.

Terminal preparation is implemented: the shared model has runtime geometry and
a bounded resize API for both screens; `gterm -g 64x20` renders at 512×320 pixels
and sets the matching PTY size. The Nano-X terminal client reuses this model
and the PTY/job-control behavior with a Nano-X renderer/input adapter. GX keeps
a fixed desktop size; Nano-X resizes the guest child windows and notifies their
PTYs independently. Scrollback, clipboard/text input and further terminal polish
remain separate work.

## 4. Small desktop and nxroach

The upstream launcher, graphical terminal and `nxclock`/`nxeyes` clients are
implemented. Eyes exercises shaped windows, pointer tracking and blink timers;
Clock uses timed event waits and OS wall time. Existing terminal applications
such as Levee and top are available through launcher entries. Next add a
graphical resource monitor and further useful clients.
Nxroach is ported from the pinned demo tree, using root/window visibility
queries, region subtraction, transparent bitmap drawing and software floating
point. It draws onto RVM's guest desktop, with server-side clipping protecting
foreground windows. A roach moving
across separate native windows is a host integration feature, not something
an RGB565 screen driver automatically supplies.
The upstream readback/pixmap drawing loop is adapted to draw its 24 converted
C16 masks directly. This removes the optional pixel/image API dependency and
does not require raising the 16-pixmap quota. Animation has a 20 fps ceiling;
fully hidden roaches block on window/exposure events. The JIT suite compares
all original sprite pixels and checks movement, occlusion/reveal and cleanup.

Use this working Nano-X desktop to select retained components and design the custom UI.
Do not commit to a large existing desktop environment. Keep drawing, app IPC
and window-management policy independently replaceable.

The next agreed stage is to port NXDSKTOP and its companion apps as a useful
lightweight desktop, then evaluate Nuklear before building the custom FuseDE
shell and file manager. See the [FuseDE roadmap](../fusede/roadmap.md) for scope
and acceptance criteria. Companion apps also target Fuse-X native windows;
the hosted platform retains macOS window management.

## 5. Performance and polish

Measure time spent rasterizing, MMU translation, DMA, full host snapshots and
compositing. Add dirty-region presentation or shared buffers only when data
justifies the new lifetime/synchronization contract. Expand formats, image
loaders, scalable fonts and acceleration incrementally. BMP decoding must
translate octet files into C16 pixels explicitly; DMA accelerates the resulting
pixel copy, not format parsing.
