# Retained software rendering

The first implementation runs in Fuse-X. Its software rendering core and
command semantics are intended for future Riplix x64 backends as well. This
work does not implement or modify the x64 kernel, hardware drivers, execution
ABI, framebuffer service or build manifests.

## Ownership and portability

Applications refer to renderer-owned targets using opaque handles scoped to
an owned window. A target contains conventional eight-bit A8 samples or opaque
RGBA pixels, independent of C16 memory. Window destruction releases all its
targets; explicit target destruction invalidates that handle. Handles are not
reused within a window. Window resizing leaves targets intact, while subsequent
blits must fit the new window dimensions.

The allocation-free core in
[`libraries/graphics/render.rs`](../../libraries/graphics/render.rs) provides
rectangle fills, A8 colour blending, overlap-safe copies and clipped line/circle
coverage. It depends on `core` and borrowed slices, and compiles under `no_std`.
It knows no windows, guest addresses, process model, allocator, macOS API or
hardware framebuffer. RGBA words have explicit channel semantics; a backend
owns any conversion needed for its physical display format.

Fuse-X's adapter validates the C16 envelope, process/window ownership, complete
guest-memory spans, command records, formats and source rectangles. Its native
backend owns allocation, copies borrowed payloads and translates the decoded
request into the shared core. A future x64 graphics service can reuse that core
and those operations with its own allocation, permission and display adapter.
It does not need to emulate a Mac window or a C16 pixel buffer.

The current `gx_render` structure is specifically a C16 ioctl envelope. Its
pointer-bearing layout must not become a native LP64 wire format. Commands use
fixed numeric 32-bit words, explicit geometry, colours and payload-relative
offsets. A native ABI adapter must decode its own envelope and define byte and
stride units explicitly while preserving the operation semantics. Neither
toolkit pointers nor backend pointers belong in service messages.

## Operations and limits

The optional `GX_RENDER` request is defined in
[`gx.h`](../../ports/graphics/include/gx.h). A version-one request with all
other fields zero probes support. Unsupported backends retain the existing
guest-RAM rendering path; no existing request or capability bit changes meaning.

* Create/destroy allocate or release an A8 or full-colour target.
* Batch executes ordered fills, borrowed or retained A8 blends, and target copies.
* Raster prepares retained A8 line/circle coverage with maximum coverage at joins.
* Upload/readback explicitly transfer A8 or BGR samples while preserving padding.
* Blit copies a colour target into its window's backing surface. Presentation
  remains a separate operation after a complete application refresh.

Each batch is validated completely before any target mutation. Its commands
execute in order. Self-copy has memmove semantics at its position in that
order, including pixels changed by earlier commands. Invalid late commands do
not leave earlier commands applied. Borrowed payloads are consumed synchronously;
the backend never retains guest pointers. Published frames remain immutable.

The initial backend permits 32 targets and 64 MiB of target storage per window,
with physical dimensions up to 3840 by 2160. A batch contains at most 256
commands, an external mask payload at most eight MiB of logical samples, and
staged command source snapshots at most 64 MiB. Raster requests contain at most
8192 primitives. Allocation and bounds failures are explicit errors. C16 payload
lengths and strides count logical octets, each occupying one C16 cell; this
boundary representation is separate from the renderer's packed storage.

## LVGL integration

The primary RGB888 display layer uses a renderer-owned colour strip. LVGL still
owns widget layout, font lookup, shape geometry, clipping and damage generation.
The adapted software blend entry submits clipped solid or masked operations as
bounded batches. Temporary small masks are copied into an owned 128-Ki-cell
arena; larger borrowed masks are submitted synchronously. The native strip is
blitted to the window without writing and then re-uploading a C16 colour buffer.

Plot coverage can remain in one of eight native A8 cache entries and be reused
across strips and repaints. The adapter recognizes registered interior source
addresses and matching strides. Updating a plot raster also reads coverage back
into its ordinary C16 A8 buffer, keeping the CPU copy valid for fallback. This
first implementation removes repeated mask uploads but deliberately retains
that compatibility copy; removing it would require additional image/task
synchronization guarantees. Reallocation and deletion release the association
after queued uses have completed.

Intermediate LVGL layers retain the existing software path. Before an
unsupported primary-layer operation, such as a regular RGB image, queued native
drawing is completed and the current strip is read back into the ordinary
software buffer. The rest of that strip then uses the established CPU path and
ordinary upload. This preserves ordering and pixels drawn before the fallback.
The next strip may use the native target again. `LVGL_SOFTWARE_TARGET=1` disables
the new path, and existing format/raster/upload diagnostic switches continue to
exercise their original fallbacks.

## Acceptance

Pure-core and runtime tests cover alpha rounding, all raster octants, extreme
coordinates, bounds, sequential overlapping copies, retained target lifetime,
allocation limits and published-frame isolation. The direct C16 guest suite
checks complete bank-crossing spans, padding, atomic invalid batches, stale
handles, process ownership, fork/COW, resizing, target teardown and RGB565 blits.
An LVGL scene compares native and software pixels at both densities, including
an unsupported image between masked draws. Full application references and
the drag benchmark exercise the production adapter.

The standalone core also cross-compiles with the installed
`x86_64-unknown-none` target. That check produces only an isolated library under
`build/lvgl/render-core`, without building or changing the x64 kernel. Its
compiler, command and source/artifact hashes are recorded in
`build/lvgl/render-core/no-std-x64.json`. This establishes bare-metal compilation
portability; a functioning x64 graphics service still needs implementation and
runtime acceptance.

Performance reports distinguish CPU preparation, rasterization, refresh and
presentation work. Native compositor cost and scheduler sleeps remain outside
the uncapped rendering benchmark. Future x64 support needs its own graphics
backend and target acceptance; passing Fuse-X tests does not establish it.
