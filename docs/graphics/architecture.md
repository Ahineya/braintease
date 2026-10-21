# Graphics architecture

## Terminal mouse interaction

Mouse input is an opt-in terminal capability shared by Fuse-X and Riplix;
applications keep their keyboard controls. The path is host pointer -> terminal
emulator -> PTY mouse reports -> curses -> application/widgets. No application
reads the desktop pointer behind its terminal's back.

Use xterm SGR reporting (1006), with 1000 for buttons/wheel, 1002 for button
motion, and 1003 for hover. Coordinates on the wire are one-based terminal cells;
C and RFL APIs expose zero-based cells. Wheel events are discrete steps. Keep
button transitions ordered, suppress redundant motion within a cell, and retain
bounded input queues. Shift-drag remains terminal text selection. The terminal
owns the graphical arrow pointer independently of the text insertion caret.

The C interface follows ncurses: mousemask, KEY_MOUSE, getmouse, wenclose and
wmouse_trafo. RFL extends InputEvent with optional MouseEvent data and explicit
mouse subscription; existing key-only callers remain supported. Reusable hit
testing and selection belong in lib/tui widgets. Suspend/exit disable reporting;
resume restores the application's subscription. Unsupported terminals retain
keyboard operation. These escape sequences do not imply complete xterm support.

mux owns its tab bar, splitters and floating desktop window frames/task bar. It decodes outer mouse reports, selects the
topmost target pane, translates coordinates and re-encodes according to that pane's
terminal modes. Drags retain their initial owner until release/cancellation.
Each pane retains independent reporting modes. See [desktop tabs](../terminal-multiplexer.md#floating-desktop-tabs) for window controls and composition. Resize, detach and reconnect
cancel capture; terminal selection remains available through Shift. Terminal
output from a pane must not directly alter the outer terminal's mouse modes.

Implementation sequence:

1. Shared terminal modes/report encoder, gterm and Nano-X terminal integration;
   C curses input/lifecycle support and RFL event bindings.
2. A mouse/widget probe, Commander click selection and wheel navigation;
   mux click focus, tab selection, splitter dragging and pane-local forwarding.
3. Reusable list/input/dialog interaction and adoption in Rift, Music,
   Telegram, Fuse Chat and Commander dialogs.
4. Reusable hover/capture widgets and richer interactions. Optional pointer-shape
   requests may use OSC 22 with capability discovery; plain terminals retain
   their default pointer.

The initial milestone uses the existing host-native arrow; movement never
requires redrawing the terminal. Pixel-granular reporting and pointer-shape
negotiation are follow-ups. On future bare metal, the display/input backend
supplies the pointer and the same terminal protocol remains applicable.

Images are explicitly out of scope for this milestone. A later design should
evaluate Kitty graphics/Sixel, reuse libraries/image, negotiate capabilities,
bound decoded memory and define clipping, scrolling, alternate-screen cleanup
and mux pane ownership before enabling image transport.

The RVM text-console host reader also re-encodes decoded host mouse events as
SGR input for the guest. Without that adapter, its keyboard event parser consumes
the mouse packets before curses sees them. Console shutdown/signal/panic cleanup
disables host mouse reporting. The graphical gterm path uses GX input directly.

References: [xterm mouse protocols](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html),
[ncurses mouse API](https://invisible-island.net/ncurses/man/curs_mouse.3x.html),
[pointer shapes](https://sw.kovidgoyal.net/kitty/pointer-shapes/).

### First implementation and verification

The first three stages are implemented with cell-coordinate reporting. Shift-drag
is the sole selection override; Command-drag is not assigned. The existing
native arrow remains independent of the terminal's insertion cursor. Custom
pointer shapes and images remain future work.
The C compatibility subset exposes raw transitions, not synthesized double or
triple clicks. Vertical wheel pulses map to buttons 4/5; smooth/pixel scrolling
and horizontal wheel handling in curses are future extensions.

RFL applications call `screen.mouse(MOUSE_DRAG)`, selecting **one** of
`MOUSE_BUTTONS`, `MOUSE_DRAG`, `MOUSE_HOVER` or `MOUSE_OFF` (not a bitwise mask).
`Screen.event()` / `Window.event()` return `kind == 2` and `Some(MouseEvent)`
for mouse events; `event.key()` returns `NO_KEY` for them. Mouse coordinates are
screen-relative even when read through a Window. `Rect.contains` provides hit
testing, and `FilePanel.mouse(area, event)` shares geometry with its painter.

Application controls now include:

- Rift: click a text cell to place the caret, drag for a vi visual selection,
  wheel to scroll three visual rows while keeping the caret visible, and click tabs, menu headings or footer
  shortcuts. Insert mode survives a caret click; dragging enters Visual mode.
  Hit testing uses the text engine's wrapped, tab-expanded Unicode layout.
- Music: click a file/playlist row to select it; click the selected row again
  to open/play it. Wheel navigates the pane under the pointer. Click transport
  buttons or Shuffle; click/drag the progress line to seek. Wheel over the
  time/volume row changes volume. A drag survives idle audio polling ticks.
- Telegram: click to open a chat, position the search/message caret, or use
  footer actions. Wheel over chats navigates selection; wheel over history
  scrolls three lines and requests older history at the top. Sending remains
  an explicit Enter/Send action.
- Fuse Chat: click conversations, position the multiline composer caret, choose
  `@file` completions and activate footer shortcuts. Wheel scrolls history by
  three rows, navigates the conversation chooser or moves completion selection.
  Input placement uses the existing Unicode wrapping engine and adapts to
  narrow mux panes. A completion click never sends the draft.
- Shared dialogs: click choices, position text-field carets, scroll text/menus,
  and click accept/cancel/close hints. The file picker supports list selection,
  wheel navigation and clicking the selected row again to open it. Popup mouse
  coordinates are translated from screen space before widget hit testing.
- Commander: its theme/About dialogs reuse the shared widgets; confirmation
  hints are clickable and the file viewer accepts wheel scrolling.

`Selection.mouse(area, event, count)` consumes list hits without indexing empty
rows. `LineEdit.mouse(area, event)` places a caret on a grapheme boundary,
including wide characters and horizontally scrolled fields. Neither widget
presents frames or chooses an application's activation policy. Shift-drag
continues to belong exclusively to terminal text selection in all these apps.
The applications subscribe to button or drag reporting; continuous hover
repainting is not enabled for these controls.

Build the interactive hover/button probe:

```sh
target/release/rfl build rfl/examples/mouse.rfl --os fuse-x -o build/tui-mouse/mouse-demo.fxe
target/release/rfl build rfl/examples/mouse.rfl --os macos -o build/tui-mouse/mouse-demo
```

Rebuild curses consumers together with their SDK libraries: independent native
packages validate shared-library ABI fingerprints. Old packages paired with a
new library are rejected. The regular Fuse-X build/install workflow handles
the full collection; the commands above only build the probe, without updating
a disk image. Shut down a guest before updating its image.

Verification entry points:

```sh
python3 ports/graphics/tests/run.py
python3 ports/fuzix/tools/test_mux_mouse.py
python3 ports/fuzix/tools/test_curses.py --native
python3 ports/fuzix/tools/test_curses.py
python3 fuse-x/test_terminal_mouse.py
python3 fuse-x/test_clipboard.py --terminals-only
python3 rfl/tools/test_mouse.py
python3 rfl/tools/test_apps_mouse.py --target both
python3 ports/telegram/tests/test_ui.py
python3 ports/chat/tests/test_interaction.py --ui-only
cargo test -p rfl --test tui_widgets
python3 ports/fuzix/tools/test_mux.py --system fuse-x
```

The RFL probe/Commander test expects the freshly built standalone probe and
`fuse-x/build/apps/rfl-fm.fxe`. Graphical transport tests inject opt-in headless
host events into both real terminal frontends, including Shift-drag clipboard
selection. Tests use private disposable disks; no running user image is updated.

## Ownership and boundaries

Riplix owns processes, PTYs, permissions and window ownership. A graphics service
owns surfaces and presentation. The host backend supplies display/input and,
for Fuse-X, native window lifecycle. A terminal owns a PTY master; its child
session owns the slave and runs the shell or foreground command. A GUI library
must not bypass terminal job control or use one global input stream for every
window.

```text
sh / vi / top                  graphical applications
     │ PTY stream                       │ drawing
     ▼                                  ▼
gterm: terminal model           LVGL / GX apps; Nano-X clients → server
     │ upload/copy/fill/present         │
     └────────────────┬─────────────────┘
                GX window/surface/event interface
                         │
        ┌────────────────┴────────────────┐
 RVM: kernel /dev/gfx              Fuse-X hosted backend
 physical scanout + DEVICE         native top-level windows
 one guest desktop              or optional nested desktop
                                one macOS app identity
```

Keep terminal parsing and painting independent of the window manager. Nano-X
is a source of graphics algorithms and a small application API, rather than a
requirement to put an entire virtual desktop inside every Mac window. There is
no X11 protocol or X server in this first milestone.

Fuse-X's optional `--desktop` mode composes independent GX clients inside one
host window. The portable window policy and opaque compositor live in
`libraries/graphics/desktop.rs`; the host adapter supplies macOS presentation
and input. Applications retain their own processes, PTYs and surfaces. This is
a test desktop foundation; a Graphite shell and application launcher remain
separate work. See [Fuse-X desktop](../fuse-x/desktop.md).

## GX version 1

The common C header is [gx.h](../../ports/graphics/include/gx.h). This is a
C16 **guest ABI**, not a host C structure layout or an octet network protocol.
All dimensions, strides and buffer lengths count pixels/C cells as specified;
one RGB565 pixel occupies one 16-bit cell. Pointers use the existing RCC ABI.
A native backend must decode/validate guest data; it must not cast guest
structures to host structs or retain translated pointers after an operation.

| Operation | Contract |
| --- | --- |
| INFO | Version, formats, capability flags and maximum dimensions/window count |
| FORMATS | Optional format bit mask and preferred surface format; unsupported backends retain RGB565 |
| CREATE | Checked dimensions/format, title metadata, opaque window and surface handles |
| UPLOAD | Copy a caller-owned RGB565 rectangle synchronously, with a pixel stride |
| UPLOAD_BGR888 | Optional synchronous B,G,R logical-octet source upload, with a source-cell stride; preserves all channel bits on XRGB8888 and quantizes on RGB565; advertised by GX_CAP_BGR888_UPLOAD |
| BACKING_SCALE | Opt into 1×/2× physical surfaces while window geometry and input stay logical; zero follows display density, advertised by GX_CAP_BACKING_SCALE |
| PRESENTATION_ZOOM | Optional per-window native presentation enlargement, 100–300 percent; the backend may reduce it to fit the monitor and returns the effective percentage; guest geometry, backing pixels and input coordinates remain unchanged; advertised by GX_CAP_PRESENTATION_ZOOM |
| FILL | Fill a surface rectangle with one RGB565 colour |
| FILL_RGB888 | Optional opaque fill with a 0x00RRGGBB colour; retains eight-bit channels on XRGB8888 |
| COPY | Copy a rectangle on the same surface with memmove overlap semantics |
| RASTER_FILL / RASTER_COPY | Bulk fill/copy in caller RAM, with validated strides and complete ranges; COPY requires disjoint spans |
| RASTER_BGR888 | Optional BGR caller-RAM fill/blend with an A8 mask and opacity; complete spans and colours are checked before writes, overlapping mask/destination spans are rejected |
| RASTER_A8 | Optional antialiased line/circle coverage in caller RAM, combining coverage with maximum; version 1 with all other fields zero probes support |
| RENDER | Optional renderer-owned A8/colour targets with validated ordered drawing batches, explicit upload/readback and target-to-window blits; a zero-fields version-one request probes support |
| PRESENT | Publish a completed surface snapshot; no retained userspace pointer |
| EVENT | Nonblocking per-window event; type zero means no event |
| STATE | Recover held keys, buttons, focus and pointer state after overflow |
| WAIT | Ripple PTY-terminal extension: atomically wait for window input, PTY output/EOF or PTY write space |
| DESTROY | Release the window and its surface |
| TITLE | Reserved request; usable only when advertised by the backend |
| CLIPBOARD_READ / CLIPBOARD_WRITE | Optional owned-window plain UTF-8 text exchange, up to 32,768 octet cells; advertised by GX_CAP_CLIPBOARD |

Handles are opaque and generation-checked. Applications must not assume the
window and its surface always share a handle, even though this backend does so.
Unknown/unsupported operations fail. Use capability flags or an operation's
explicitly documented probe to discover support:
the initial backend advertises RGB565, copy, fill and input, one window, and a
640×480 maximum. It does not advertise multiple windows, resizing or title
updates. CREATE accepts a title as metadata; the initial RVM window still uses
its host title. A future backend should implement the same requests and add
capabilities, rather than change application pointer or pixel conventions.

Surface precision is a backend capability, independent of the CPU or C data
model. `GX_INFO.format` retains its legacy RGB565 value. New clients query
`GX_FORMATS`, whose `formats` is a 32-bit `GX_FORMAT_*` bit mask and `preferred`
is a format identifier accepted by CREATE. An unsupported query means RGB565.
Fuse-X advertises both RGB565 and opaque `GX_XRGB8888`, preferring the latter;
RVM/RBF retain their existing RGB565 devices and requests. No VM driver or
scanout buffer is widened by the hosted implementation.

The full-colour ABI specifies eight bits per R/G/B channel, not a host memory
layout. Existing UPLOAD/scaled/indexed sources and FILL colours remain RGB565,
including on a full-colour surface; the backend expands them once. BGR888
upload and FILL_RGB888 provide full precision, COPY preserves it, and PRESENT
publishes an immutable snapshot. The hosted backend stores opaque RGBA32
pixels directly for presentation and does not round-trip them through RGB565.
Resize preserves overlapping pixels in their original precision; backing-scale
changes allocate a fresh surface for repaint. There is no alpha-compositing
contract in this extension.

Presentation zoom is independent of backing density. Fuse-X normally maps one
guest logical pixel to one native window point. `GX_PRESENTATION_ZOOM` enlarges
that mapping for bitmap applications such as LGeneral without reallocating or
resampling the guest surface. Native input and resize events map back through
the same scale. The requested zoom is retained when fitting to a smaller
monitor, so moving the window to a larger display can restore it. Applications
that do not request this capability keep the existing presentation path.

Future Riplix x64 graphics should implement this format discovery and choose
its preferred full-colour scanout/storage independently. LP64 buffer adapters
will still need their own pointer/stride contract: the current C16 BGR888 source
uses three logical octet cells per pixel, not a castable host byte buffer.

Events carry the target window, sequence, type and typed payload. Keyboard codes
are USB HID usages, pointer coordinates are content pixels, wheel values are
signed 16.16 lines. Focus/capture flags describe actual state. Close is a request
to the owner, not machine shutdown. Resize is reserved for a later capable
backend. Overflow tells the client to recover state and discard assumptions
about intermediate transitions. The current terminal clears held modifiers and
repeat on loss of focus or overflow. Text composition/IME needs a future
versioned event extension; HID translation alone is not a full text-input API.

## Current RVM implementation

`/dev/gfx` is a checked ioctl device, exclusive to the creating process. The
process table slot **and** PID identify its owner. An inherited descriptor does
not grant a forked child access to its parent's handles. Explicit DESTROY,
owner exit and successful exec release the lease. Closing a transport descriptor
alone does not destroy a window; this avoids an inherited child's close
releasing the parent's window. Clients must use DESTROY or exit. Revisit this
transport policy when introducing independent server/client connections.

The framebuffer uses reserved physical banks 122–126 (320,000 cells), outside
the COW user pool. A maximum frame needs 307,200 cells / 614,400 physical octets.
Uploads validate the complete userspace rectangle first, then resolve readable
MMU pages and DMA into physical scanout storage. This does not expose physical
addresses to applications and never hands DMA a virtual userspace address.
The syscall does not switch tasks while copying. Kernel code alone controls
the shared DEVICE mailbox, with IRQ protection for each transaction.

FILL and COPY use synchronous bulk memory commands. COPY chooses vertical row
order and snapshots each row, preserving overlap in either direction. A scalar
fallback preserves the contract if bulk commands are unavailable. PRESENT uses
RGB565_EXT and publishes a host-owned immutable snapshot; the guest may reuse
upload storage after the ioctl returns. There is no asynchronous fence or
zero-copy promise. The existing host presenter still copies a complete frame;
dirty rows reduce guest painting/upload work, not host publication cost.

`gterm` compares dirty rows against a shadow of the painted cells. It uploads
only changed character spans, fills runs of spaces, and moves the shadow along
with COPY/FILL scrolling. Old and new cursor cells are damaged explicitly.
`gterm` uses the standard guest `select()` interface to wait on its graphics
descriptor and PTY master. Graphics read readiness means a queued event,
overflow notice or close request; `GX_EVENT` retrieves it without changing the
wait API. PTY writes are watched only while keyboard replies are pending.
Finite waits drive repeat/child checks using monotonic time. Queue transitions
wake the process immediately; timeout duration is not an input delay.
See [descriptor readiness](../networking/select.md) for the C16 ABI, native
coverage, timer resolution and lifetime rules.

`GX_WAIT` remains as a compatibility ioctl combining one window and one PTY.
It is no longer gterm's wait mechanism. Unix-domain local IPC descriptors can join
the same select loop; multiple windows still need explicit event ownership and
subscription semantics.

RVM opens a 640×480 framebuffer at 640×480 logical host points. Very small
framebuffers retain integer enlargement to stay readable. GPU redraws occur on
frame changes or host expose/resize requests, rather than continually rendering
an unchanged terminal.

The MMU loader resolves validated writable pages before DMA, including COW
resolution, and uses physical page spans for exec data, user copies and clears.
Fresh exec pages are already zeroed. No user virtual address is submitted as a
physical device address. JIT regions retain physical instruction provenance:
code writes invalidate overlapping regions in all roots; instruction-map flushes
revalidate mapped regions and retain unaffected supervisor and user code. The
virtual executable bound is cached until a mapping flush. User memory has a
checked direct-mapped translation cache; native fast paths require matching
page/epoch, permissions and both virtual and physical bounds. Misses and COW
writes retain the reference fault path.

The bounded terminal model and event/output batches
prevent an unbounded escape sequence or output stream from consuming the
kernel. Keyboard input is sent to the PTY, so signals and foreground groups
continue to be handled by the tty driver. Child sessions receive the selected
terminal geometry (80×30 by default, with matching pixel dimensions) and
TERM=ansi. Terminal close sends hangup/continue to the child's
process group and bounds teardown before forced termination; it does not yet
provide a save-work dialogue or a GUI detachment policy.

RVM's optional guest-close policy hides a closed host window, posts a typed
close event, and keeps the VM alive. A later display lease can reopen it.
Close events survive input overflow/state recovery. Ordinary applications
without that option retain the existing close-to-stop behavior. Platform UI acceptance testing includes opening, keyboard input and
close/reopen; headless capture alone cannot prove native window behavior.
Mac synthetic input can omit separate modifier transitions. The presenter uses
modifier flags to supply a left-key alias when no physical side is known, and
clears both sides on release. GTERM_TRACE_INPUT=1 logs GX events to the original
console for diagnosis; normal sessions stay quiet.

## Resource budget

The protected profile expands capacities without increasing the existing
32,768,000 physical-octet RAM reservation or the 16,000 KiB COW process pool.

| Resource | Before | Protected graphics profile |
| --- | ---: | ---: |
| Process slots | 16 | 64 |
| Descriptors per process | 16 | 32 |
| System open-file objects | 64 | 192 |
| Cached inodes | 64 | 128 |
| PTY pairs | 4 | 16 |
| User data/heap virtual banks | 16 | 23 |
| Per-process user stack | 64,000 cells | 64,000 cells |
| TCP connections | 4 | 16 |

The close-on-exec mask grows to 32 bits where UFTSIZE exceeds 16. All descriptor
mask shifts use wide arithmetic. Kernel stacks occupy banks 32–95; packed MMU tables
96–117; reference counts 118; saved task data 119; FAT chain 120–121; scanout 122–126. User physical
pages remain 128–255. Compile-time checks guard stack/table overlap and saved
task-data size. Moving saved task data out of GP bank 3 leaves space for the
larger process, inode, file and input tables. Riplix now supports only
MMU builds on RVM; the former non-MMU/RBF configuration is retired.

These are finite capacities, not a promise that 64 maximum-size programs fit.
Executable allocation is still variable-size within the configured code pool.
`free`, `ps` and `top` report the process pool and per-task mappings; reserved
scanout belongs to kernel-reserved RAM and is not charged as user heap.

## Buffered Nano-X repaints and bulk raster operations

Client windows and child widgets draw into RGB565 backing buffers. Explicit
`GrFlush()` commits each client's dirty rectangles together. Query replies can
flush transport bytes without publishing intermediate painting; another client
remains free to present. This prevents visible button-by-button or line-by-line
redraws without delaying input behind an entire unbounded request stream.
The server handles at most 32 staged requests per receive pass. NanoWM's own
container/decorations remain direct rather than allocating duplicate surfaces.

GX_CAP_RASTER advertises synchronous operations on the caller's RAM, independent
of surface handles. Width, height and strides count RGB565 cells. The full source
and destination extents must be valid user memory; overlapping COPY extents are
rejected before mutation. RVM resolves writable COW pages and submits physical
DMA spans. Fuse-X performs page-aware bulk operations on its guest memory.
Neither backend retains guest pointers or presents automatically. The driver
uses these operations for large fills and copies, with scalar fallbacks. General
raster modes retain their software implementation.

Fuse-X additionally implements optional `GX_RASTER_A8` for bounded generic
geometry. It copies and validates up to 8192 line/circle records before writing
the destination, clips extreme signed coordinates before rasterization, and
preserves strided padding. Source/destination spans must be disjoint. A8 samples
use the low eight bits of each C16 cell; `GX_A8_CLEAR` clears only active pixels,
otherwise new coverage combines with existing samples using maximum. The host
keeps no caller pointer and performs no window presentation. This operation
has an explicit all-zero-fields version-1 probe, leaving the existing 16-bit
capability flags unchanged. Older backends reject it and applications retain
their portable rasterizer. LVGL plots use this path to prepare one coverage
image per changed viewport, then blend that image through `GX_RASTER_BGR888`.

The optional [retained rendering contract](retained-rendering.md) removes the
colour-buffer round trip for supported LVGL primary-layer drawing. Targets use
packed native pixels behind window-owned handles; the portable software core
has no C16, macOS or hardware dependency. The C16 ioctl envelope remains a
separate adapter from future native ABI interfaces. Existing raster/upload
operations and software fallbacks remain available. The first implementation
is hosted Fuse-X; it does not modify or implement the ongoing x64 kernel or
framebuffer work.

Heap growth can use virtual banks 8–30 (23 banks), leaving stack bank 31 and the
C ABI unchanged. Initial executable data remains capped at 16 banks. This gives
the server room for retained window buffers without increasing physical RAM;
allocations still compete for the shared physical pool. MMU acceptance checks
exercise the last heap bank, fork/COW isolation and shrinking that range.

## Fuse-X and Nano-X seam

Fuse-X needs a window registry with owner, generation, dimensions, title,
surface, event queue and host NSWindow reference. macOS owns decoration,
placement, minimization and focus; Riplix owns process and content lifetimes.
One Dock identity, native Window menu and New Terminal remain host policy.
No new MMIO pins are necessary for this guest API. The native hosted backend
is a separate implementation of GX, not an emulation requirement for DMA.

For Nano-X, classify root, top-level, child, popup and offscreen windows. A
native top-level must become an independent GX window/surface; children retain
clipping/coordinates inside that content. Preserve lifecycle, stacking, grabs,
focus, close and resize semantics at that layer. The RVM backend can compose
those surfaces into one guest desktop. A Nano-X PSD update callback alone
receives framebuffer damage and lacks the lifecycle information needed here.
Shared-window adaptation therefore belongs above that screen-driver callback.

`nxeyes` also uses a nonrectangular top-level bounding region. The future native
adapter must carry that mask into native window/input handling, or explicitly
offer a rectangular fallback. RGB565 pixels alone cannot describe transparent
corners or input passing through them; the current GX v1 header has no surface
alpha or window-shape operation.

The initial Nano-X engine is exercised by `nxdraw`, which owns the existing
single RVM GX lease. Its C16 screen driver rasterizes into a 640×480 userspace
RGB565 buffer and uploads the bounding dirty rectangle at presentation. GX
resolves the MMU mapping and uses DMA for the upload. Mono bitmap strides count
16-bit cells; surface sizes use 32-bit cell counts. Same-surface blits currently
snapshot the source in userspace for correct clipping and overlap across region
bands. A later optimization can replace this copy with bounded scratch spans
or validated GX bulk operations. The separate Nano-X server and event/window
adapters remain the next milestone; the engine demo installs no window manager.


## Hosted text clipboard

Fuse-X advertises the optional clipboard requests and emits `GX_ACTION` edit
commands (copy, paste, cut, select-all) in input order. Terminal Ctrl-C remains
a real tty input; Command-C is a selection command. The AppKit main thread owns
pasteboard access; the runtime exchanges bounded owned UTF-8 strings with the
kernel's ownership-checked GX service. Nano-X request 251 bridges client-owned
windows using chunked snapshots and atomic write commits. The RVM Nano-X
server keeps a local clipboard without adding host clipboard MMIO.

See [Fuse-X desktop integration](../fuse-x/desktop.md#text-clipboard) for the
ABI, app bindings, limits and acceptance checks.
