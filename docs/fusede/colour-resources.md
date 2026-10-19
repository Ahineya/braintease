# FuseDE colour resources and first guest showcase

The first guest rendering milestone is implemented for **RVM and Fuse-X**.
`fdeshowcase` displays Slate nine-slice skins and the pinned Lucide icon atlas in
an independent Nano-X application. It supports button hover/press/disabled/focus
states, keyboard selection and resize repainting. It is a renderer/control
showcase, not yet the reusable widget toolkit or desktop shell.

## Build and run

From the repository root, using the existing built toolchains and kernels:

```sh
make -C ports/fuzix BUILD="$PWD/build/fuzix" -j4 userspace
python3 fuse-x/graphics.py
```

Rebuild/install the guest applications with the usual launcher before using a
normal root disk. Inside the RVM graphical session, run `fdeshowcase` in a
terminal. On Fuse-X, start the graphics service with `startx`, then run
`fdeshowcase`; it opens an independent native top-level. Tab moves button focus,
Enter/Space activates, Up/Down changes list selection, and Escape/Q closes it.
The file list is illustrative; it does not perform filesystem operations.

The normal build/install paths include the application and these resources:

| Guest path | Purpose |
| --- | --- |
| `/usr/share/fusede/themes/slate/theme.fdi` | RGB565 theme atlas and binary mask |
| `/usr/share/fusede/themes/slate/theme.json` | Named skin rectangles, insets and palette metadata |
| `/usr/share/fusede/icons/lucide/icons.fdi` | Tint-only icon masks at 16/24/32 pixels |
| `/usr/share/fusede/icons/lucide/icons.json` | Named icon rectangles and pinned revision |
| `/usr/share/fusede/icons/lucide/LICENSE` | Lucide ISC and Feather-derived MIT notices |

The showcase loads both `.fdi` files by default; `--built-in` uses its compiled
fallback atlases for comparison. It uses compiled matching metadata. General
theme discovery, user overrides and JSON/INI-driven theme switching remain later
work; replacing a data file alone does not replace its compiled layout metadata.

## Application library

[fde_nano.h](../../ports/fusede/include/fde_nano.h) exposes creation, upload,
load-from-memory/file, clipped drawing, destruction and nine-slice painting.
Link the shared Nano-X client and skin object as the showcase build does. This
is a C interface suitable for an RFL wrapper once shared controls settle.

Each image belongs to one Nano-X connection. Dimensions count pixels; one
RGB565 pixel is one C16 cell. Masks use MSB-first 16-bit words and padded rows.
An image can hold opaque RGB565, RGB565 plus a mask, or only a tintable mask.
Upload the complete image once; drawing reuses server memory. Uploads are
sequential and become immutable when complete. Drawing before completion fails.
Explicit destroy and connection loss reclaim both finished and partial images.

One draw contains up to nine source/destination rectangles, allowing one
nine-slice frame to be validated together. The whole batch validates before
painting. Nearest-neighbour sampling preserves its phase after destination
clipping. GC clipping regions, window clipping and buffered `GrFlush()` repaint
boundaries continue to apply. Image drawing supports copy mode; source masks
preserve the destination, while tint uses the caller's GC foreground. It does
not permanently change that foreground.

## Bounded C16 transport

The existing private `.nano-X16` protocol gains request **253**, image version
**1**. The existing connection ABI handshake remains required. The request
contains only scalar fields and an inline payload: no client pointers, host
struct casts or filenames. The server does not open arbitrary paths or decode
PNG/SVG/TTF data. Optional upstream image codecs and `GrArea` remain disabled.

The operations are create, sequential upload, draw batch and destroy. All return
a typed status reply. Bad framing, payload lengths, versions and operation codes
disconnect that client. Semantic failures return a status and leave resources
and other clients usable. The client helpers handle the existing typed event/
reply stream. Invalid dimensions, stale/foreign IDs, unsupported flags, incomplete
uploads and foreign drawable/GC ownership are rejected.

| Bound | Current value |
| --- | --- |
| Maximum source width/height | 512 pixels each |
| Storage per image, including mask | 65,536 C16 cells / 128 KiB |
| Combined image storage | 131,072 C16 cells / 256 KiB |
| Images per client | 4 |
| Global public image + pixmap object quota | 16; window backing buffers excluded |
| Upload payload per request | 2,048 C16 cells |
| Draw rectangles per request | 9 |
| Destination rectangle | Up to 1024×768; clipped to actual drawable |
| Total requested draw work per batch | At most 1024×768 pixels |

Image IDs increase monotonically during a server lifetime and are never recycled;
exhausting that namespace fails creation. Resources cannot be carried across
connections or server restarts. Rendering emits horizontal colour spans through
the existing clipped engine, avoiding an IPC request per pixel. The selected
skin plus icons occupy 31,572 source-data octets before bookkeeping; destination
backing surfaces are additional costs. Immutable cross-client sharing is not yet
implemented.

## File format

`FDI1` is deliberately small. It contains the four ASCII octets `FDI1`, followed
by little-endian 16-bit width, height, flags and reserved-zero words (12 header
octets total). Flags are 0 for RGB565, 1 for RGB565-plus-mask, and 2 for mask-only.
The tightly packed RGB565 words come first when present, followed by mask words
when present. Mask row length is `(width+15)/16`. There is no stride/pointer or
compression field. The exact payload length follows from dimensions and flags.

The client loader decodes each octet explicitly, bounds creation before reading
the payload, uploads in 256-word chunks and rejects truncation/trailing data.
Failures reclaim partial resources. Loading this format does not require the
guest to interpret SVGs or rasterize a font. Builders and source records are
under [ports/fusede](../../ports/fusede/README.md).

## Verification

```sh
python3 ports/fusede/tools/test_skin.py
python3 ports/fusede/tools/test_raster.py
python3 ports/fusede/tools/test_showcase.py
```

The integration suite creates disposable disks and boots the real kernels. It
checks framing rejection, partial/duplicate uploads, image/pixmap quota recovery,
foreign/stale resources, foreign drawable/GC access, abrupt-exit cleanup, malformed
asset files, mask tint, GC state restoration and continued legacy client use.
An independent oracle compares every pixel of a 128×96 drawing probe on each
platform, including clipped scaling and a user GC clip region.

The RVM test exercises selection/focus through real keyboard events, observes
that an idle showcase stops presenting frames, and returns cleanly to the shell.
Fuse-X checks independent native resizing/close, server reuse and orderly shutdown.
Both disposable filesystems are checked after shutdown. Captures/logs are written
under `build/fusede/tests/`. These headless checks do not replace a visual host
input/window smoke test or a formal cold-start/input-latency benchmark.

The [workspace milestone](workspace.md) now provides window services, RVM panels
and Slate NanoWM decorations. The measured shared-widget evaluation and separate
dock/shell follow it.
