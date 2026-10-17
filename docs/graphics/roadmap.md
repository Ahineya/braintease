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

Nano-X being imported does not complete its adaptation or enable nxroach.

## 2. Readiness and several windows

Extend the terminal milestone's combined graphics/PTY wait to checked local IPC,
independent service descriptors and finer deadlines. Choose bounded message
framing in C16 cells explicitly. Reserve Unix-domain/local endpoints for guest services;
do not spend the small internet TCP pool on local drawing messages. Measure
real terminal/editor memory and latency before raising further limits.

Add multiple independently owned surfaces and windows. RVM gets composition,
focus, z-order and damage tracking; Fuse-X maps top-levels to native windows.
Implement titles, close negotiation, size changes, PTY SIGWINCH, recovery after
clients die, and resource exhaustion/reuse. Add terminal scrollback, proper key
repeat timing, clipboard/text input and a deliberate detached GUI policy.
Closing all terminals must leave the kernel able to accept a new terminal.

## 3. Adapt Nano-X deliberately

Resolve the preprocessing/type/layout issues in the audit using local adapters
and narrow source patches, preserving RCC's C ABI. Start with RGB565 only,
compiled fonts, no X11 wrapper and no optional image/font dependencies. Prove
regions, clipping, text and blits before enabling more drawing features.

First link a bounded single-client test to expose engine/runtime issues. Then
bring up a separate server over local IPC with readiness and disconnect
cleanup. Do not adopt a single shared address space as the long-term solution.
Translate Nano-X top-level lifecycle into GX, retain child clipping and native
window policy, and validate with two independent applications on each backend.

## 4. Small desktop and nxroach

Add a launcher, graphical terminal, clock/resource monitor and small editor.
Port nxroach from the pinned demo tree. It uses root/window visibility queries,
region subtraction, bitmap operations and floating point; first define what
“desktop background” means on RVM versus native macOS windows. A roach moving
across separate native windows is a host integration feature, not something
an RGB565 screen driver automatically supplies.

Use this stage to select retained Nano-X components and design the custom UI.
Do not commit to a large existing desktop environment. Keep drawing, app IPC
and window-management policy independently replaceable.

## 5. Performance and polish

Measure time spent rasterizing, MMU translation, DMA, full host snapshots and
compositing. Add dirty-region presentation or shared buffers only when data
justifies the new lifetime/synchronization contract. Expand formats, image
loaders, scalable fonts and acceleration incrementally. BMP decoding must
translate octet files into C16 pixels explicitly; DMA accelerates the resulting
pixel copy, not format parsing.
