# Graphics architecture

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
gterm: terminal model           Nano-X clients → server
     │ RGB565 upload/copy/fill/present   │
     └────────────────┬─────────────────┘
                GX window/surface/event interface
                         │
        ┌────────────────┴────────────────┐
 RVM: kernel /dev/gfx              Fuse-X hosted backend
 physical scanout + DEVICE         native top-level windows
 one guest desktop              one macOS app identity
```

Keep terminal parsing and painting independent of the window manager. Nano-X
is a source of graphics algorithms and a small application API, rather than a
requirement to put an entire virtual desktop inside every Mac window. There is
no X11 protocol or X server in this first milestone.

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
| CREATE | Checked dimensions/format, title metadata, opaque window and surface handles |
| UPLOAD | Copy a caller-owned RGB565 rectangle synchronously, with a pixel stride |
| FILL | Fill a surface rectangle with one RGB565 colour |
| COPY | Copy a rectangle on the same surface with memmove overlap semantics |
| RASTER_FILL / RASTER_COPY | Bulk fill/copy in caller RAM, with validated strides and complete ranges; COPY requires disjoint spans |
| PRESENT | Publish a completed surface snapshot; no retained userspace pointer |
| EVENT | Nonblocking per-window event; type zero means no event |
| STATE | Recover held keys, buttons, focus and pointer state after overflow |
| WAIT | Ripple PTY-terminal extension: atomically wait for window input, PTY output/EOF or PTY write space |
| DESTROY | Release the window and its surface |
| TITLE | Reserved request; usable only when advertised by the backend |
| CLIPBOARD_READ / CLIPBOARD_WRITE | Optional owned-window plain UTF-8 text exchange, up to 32,768 octet cells; advertised by GX_CAP_CLIPBOARD |

Handles are opaque and generation-checked. Applications must not assume the
window and its surface always share a handle, even though this backend does so.
Unknown/unsupported operations fail. Capability flags are the source of truth:
the initial backend advertises RGB565, copy, fill and input, one window, and a
640×480 maximum. It does not advertise multiple windows, resizing or title
updates. CREATE accepts a title as metadata; the initial RVM window still uses
its host title. A future backend should implement the same requests and add
capabilities, rather than change application pointer or pixel conventions.

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
