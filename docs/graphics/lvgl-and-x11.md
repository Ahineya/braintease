# LVGL and X11 graphics assessment

Investigated on 20 October 2026; Retina/font and colour milestones updated 21 October.
New graphical applications should target
Fuse-X first and future byte-addressed Riplix x64. RVM keeps its existing
Nano-X applications and is not an acceptance target for the richer toolkit.

LVGL 9.6.0 now has a Fuse-X C16 port, a calculator, a file selector and a
Graphite component gallery, and a read-only CSV/TSV data viewer. The shared layer supplies compact typography,
semantic control roles and consistent interaction states.
The adaptation uses logical RGB888 channels and A8 masks. It negotiates opaque
full-colour GX surfaces on Fuse-X, with an RGB565 fallback for older backends.
The original RGB565 calculator acceptance matched
all 174800 RGB565 pixels of the same UI built on the host; the selector matched
all 403200 pixels of its reference. The earlier probe
still demonstrates why unadapted RGB565 buffer addressing is incorrect on C16.

Use the LVGL C API for the first application. Keep window creation, events,
clipboard and pixel presentation behind a small adapter. Evaluate X11 as a
separate compatibility project for existing applications, preferably after
ordinary byte-addressed application support. A new toolkit does not require
replacing Nano-X at the same time.

## Initial Fuse-X applications

Build and staging instructions are in [ports/lvgl](../../ports/lvgl/README.md).
The outputs are `fuse-x-shared/lvcalc.fxe`, `fuse-x-shared/lvselect.fxe` and
`fuse-x-shared/lvgallery.fxe`, plus `fuse-x-shared/lvdata.fxe`,
launched through `/mnt/host/`. Each opens its own GX window without starting
a desktop. Stock LVGL buttons, labels and an editable expression field provide
the calculator smoke. The selector adds a scrolling flex container of stock
buttons, a path field, checkbox and keyboard focus/navigation. This follows
LVGL 9.6's recommendation in place of its deprecated `lv_list` widget.
`gx-port.h` isolates native windows, input, timing, clipboard and presentation
from the application UI. The compiler is unchanged. Fuse-X's kernel/runtime
now advertise an optional direct BGR888 upload and full-colour surface discovery.
The new path retains all eight channel bits through native presentation; older
kernels keep working through RGB565 fallback uploads. This format contract is
independent of the CPU/ABI and is intended for future Riplix x64 graphics too.

The current port deliberately validates a small software-rendering profile.
Its guest checks cover arithmetic, buttons, editing, Tab/Shift-Tab/Enter,
keypad input, native clipboard actions, focus-loss cancellation, resizing,
idle presentation, two independent instances and clean close/shutdown.
Its generated adaptations also correct wide coordinate constants, narrow enum
storage for widget parts and flags, integer-promotion assumptions, packed color
operations and a few frontend limitations. These are recorded with source
hashes in `build/lvgl/adaptation.json`. The original release remains separately
preserved and checksum verified.

The host reference and actual guest tests are separate executables. Guest tests
use fresh explicit-manifest disks with networking disabled; they never boot or
modify a normal OS disk. Staging is confined to the host-share executable and
its documentation/licenses. Optional distribution through the existing package
tools is the intended next deployment route for selected GUI apps and Syma.
A shared LVGL guest library can follow when several applications need it.

The separate `lvselect` preserves nxselect's one-line path/`[]` stdout contract
without replacing the system selector. It checks directories-first ordering,
parent/home navigation, hidden files, paging, fractional wheel input, native
clipboard actions, UTF-8 path editing, refresh, resize and clean selection or
cancellation. Its model bounds paths and directory listings; failures preserve
the previous listing. The bundled Noto Sans subset covers 727 codepoints,
including Ukrainian/Cyrillic, at 12/14/16 logical sizes and 24/28/32 backing
sizes on Retina. Identical glyph images share bitmap storage. GX separates logical
window/input coordinates from 1×/2× backing pixels. The apps follow display
density without losing edited text, and a generic optional BGR/A8 bulk blend
operation keeps pixel work out of checked scalar guest loops. Tests compare
actual guest pixels at both densities and simulate monitor-density changes.
Runtime font loading, full Unicode coverage and complex shaping remain later
work. See the port README for source/license pins, buffer budgets and overrides.

`ui.h`/`ui.c` apply one graphite theme and provide shared controls, rows, text
roles, panels and layouts. The calculator and selector use flex/grid layouts
and those components rather than app-specific colour tables. Their default
windows are 300×360 and 640×480 logical points; minimum sizes remain usable.
The gallery exercises live controls, typography, palette roles and keyboard/
wheel lists. Guest checks cover exact 1×/2× pixels, disabled/hover states,
Unicode clipboard, draft preservation, resizing and density transitions.

The initial scrolling bottleneck was checked scalar pixel work in the guest,
compounded by a small partial draw buffer. Bulk opaque fills, larger bounded
strips and direct BGR upload reduced the same 30-frame test from a median
3.779 s to 0.705 s (three runs each, about 5.4×). Images remain identical.
These are headless software-rendering measurements, excluding compositor and
vsync; the interactive refresh interval remains 33 ms. Commands, buffer sizes
and fallback verification are in the port README.

ARGB layers, transforms, external image decoders, runtime font engines and XML
are outside this first profile. The earlier table/chart/text reference below
remains an investigation result, not validation of those widgets in the guest.

## Targets and current capabilities

| Target | Current state | Consequence for this work |
| --- | --- | --- |
| Fuse-X | Native machine code with C16 characters, integers, logical pointers and guest APIs; GX supplies native top-level windows | Main integration target. Native execution does not make upstream byte-addressed rendering correct. |
| Riplix x64 today | Compatibility applications still use C16 | Hardware instruction selection alone does not remove the representation problem. |
| Future Riplix x64 native applications | The accepted direction is LP64 with eight-bit characters and ordinary pointers | A more conventional LVGL and X11 source-port target, after libc, process and graphics contracts exist. |
| RVM | Existing guest desktop and Nano-X applications | Maintain existing behavior; no new rich-GUI performance gate. |

The x64 direction is specified in [Native x64 Riplix migration](../arch/native-x64.md).
The toolkit integration changes no execution ABI, compiler or x64 graphics
implementation. Its Fuse-X kernel/runtime changes provide optional GX
capabilities. Fuse-X's current window and clipboard behavior is described in the
[desktop contract](../fuse-x/desktop.md).

## Probe results

Reproduction code is in [ports/lvgl](../../ports/lvgl/README.md). Reports and
full logs are under `build/gui-evaluation/`; downloads are pinned by SHA-256.

| Probe | Result | Scope |
| --- | --- | --- |
| Byte-addressed host reference | 216 LVGL C source units linked; text area, six-row table and five-point XY chart rendered under UndefinedBehaviorSanitizer | Actual LVGL rendering with hardcoded data on the development host. It is not a guest GUI or a Syma evaluation. |
| Partial update | Initial image flushed 256000 pixels; changing one table cell flushed 2850 pixels | One 640 by 400 sample with the investigation's styles and buffer configuration. |
| Idle behavior | 100 timer-handler calls without elapsed ticks produced zero extra flushes | Confirms no unconditional redraw in that interval; does not measure a running event loop with cursor timers. |
| Text deletion | Backspace after `e` plus a combining accent left `e` | The tested text area deletes a Unicode code point, not an extended grapheme. |
| C16 source translation | All ten selected, unmodified implementation files translated to native RCC IR | Core object, display, draw buffer, RGB565 blend, bitmap font, chart, table, text area and flex layout. Not a full link or renderer test. |
| Fuse-X representation test | Confirmed sixteen-bit characters and incorrect unadapted second-pixel and next-row addressing | Executed in a fresh, offline guest disk assembled from an explicit manifest. |
| X.Org source audit | Confirmed required library checks, optional features, and the Xephyr-only Kdrive build path | Source and OS-header inspection, not a compiled or running X server. |

The host reference uses a 640 by 32 RGB565 draw buffer: 40960 octets. Its
separate full-frame capture array uses 512000 octets solely to retain the
reference image. Neither figure is a complete application memory budget.
An integrated adapter can upload partial buffers into an existing window
surface; measure widget allocations, server backing and presentation copies
as well as the LVGL draw buffer.

The host screenshot is `build/gui-evaluation/host/worksheet.ppm` (a PNG preview
may also be present). It is a rendering reference with static content, without
keyboard/mouse integration, syntax highlighting or calls into Syma.

## LVGL fit and adaptation work

LVGL provides retained objects, layouts, styles, groups, input devices, text
areas, tables, charts and a software renderer. The earlier investigation profile uses libc
allocation, strings and formatting, the simple theme, bitmap fonts, RGB565/A8
software drawing, and selected widgets. It enables no GPU, OS thread backend,
external codec, filesystem driver, or XML runtime.
The calculator port uses RGB888/A8 and a smaller widget set, as described above.

### C16 buffer representation

Current C has `CHAR_BIT == 16`; `sizeof(uint16_t)` is one addressable cell.
LVGL calculates RGB565 sizes as two octets, then advances `uint8_t *` buffers
by those sizes. In the guest representation probe, advancing by two selects
the third sixteen-bit pixel rather than the second. Row strides have the
same mismatch. Relevant upstream code is
`src/draw/lv_draw_buf.c:lv_draw_buf_goto_xy` and
`src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.c:drawbuf_next_row`.

Changing typedefs cannot fix pointer increments, format sizes, packed colors,
buffer allocation, casts or copy counts. A C16 adaptation must define which
lengths count C cells, which count serialized octets, and which count pixels.
Audit draw buffers, glyph masks, color conversion, alignment, image data and
integer promotions together. Test the resulting pixels against the ordinary
host build. Do not add fictitious exact-width eight-bit types to the SDK or
silently change the existing guest ABI.

The first header compilation required an `inttypes.h` that the SDK does not
currently supply. The probe provides its own printf-format macros in a local
include directory. It makes no shared SDK changes. This configuration repair
allows translation but does not solve the buffer mismatch.

### Editor and table models

Use LVGL controls for paths, search boxes, menus, settings and small forms.
Use Rift's text model as the basis of the code-cell editor, with a graphical
renderer and input adapter. The reference's combining-accent test shows why
the generic LVGL text area does not establish Rift-equivalent editing. Syntax
highlighting, undo, grapheme navigation, efficient paste and large documents
remain editor responsibilities.

LVGL's table stores a row-height array and a row-by-column cell-pointer array;
populated cells allocate their own text. Painting visible rows does not make
its data storage a virtual table. Use it for bounded previews initially.
Large CSV datasets should remain in the worksheet's data model, with a
windowed or custom table view. Avoid copying an entire numeric dataset into
thousands of formatted label strings.

The chart API accepts `int32_t` X and Y arrays. The reference confirms that an
XY scatter series uses nonuniform X values. Arbitrary floating-point data,
missing values, logarithmic transforms, scientific labels and export policy
still need an application plotting layer. Enabling floating-point layout
does not change these chart array types. Keep original numeric values and
plot descriptions independent of the widget; use scaled display coordinates
or a dedicated plot renderer without changing the underlying dataset.

### Fonts and desktop behavior

The reference's bundled font is sufficient for its ASCII labels. A usable
worksheet needs fonts covering Ukrainian and mathematical symbols, coherent
font measurement, and a scaling policy. UTF-8 storage alone does not supply
glyph coverage, grapheme editing, shaping, or input-method composition.
Start with an explicitly bounded font set and test those behaviors separately.

Map one LVGL display to each real application window. An LVGL `lv_win` widget
is a control inside a display, not an OS top-level window. Input devices and
focus must be attached to the correct display. Application close requests
must permit save/cancel; destroying a window must not end the OS session.

## Window adapter and Nano-X transition

Keep document and computation state independent of the toolkit's window
backend. C16 and future LP64 builds can share source-level interfaces while
using different ABI adapters. Do not serialize native C structures, LVGL
pointers or toolkit objects into window-service messages.

| Adapter responsibility | Existing Fuse-X mechanism | Required integration behavior |
| --- | --- | --- |
| Create, resize and destroy a top-level | GX window/surface ioctls | Validate geometry, handle allocation failure, acknowledge resize and preserve save/cancel on close. |
| Upload and commit pixels | `GX_UPLOAD` and `GX_PRESENT` | Translate pixel/stride units explicitly; upload damaged rectangles and commit at a complete LVGL refresh boundary. Measure any full-frame copies farther down the host path. |
| Keyboard and text | Ordered GX key, committed-text and action events | Separate shortcuts from Unicode input; clear held keys and pointer state on focus loss or overflow. |
| Clipboard | GX UTF-8 clipboard and Copy/Cut/Paste/Select All actions | Route commands into the editor; batch paste rather than synthesize individual key presses. |
| Event waiting | Existing graphics/select mechanisms plus LVGL's next timer deadline | Block while idle; wake for input, resize, exposure and genuine timers. No permanent short-interval repaint loop. |
| Fonts and scaling | Toolkit font callbacks and platform geometry | Keep logical coordinates, rendering scale and pointer coordinates consistent. |

A direct GX adapter would allow an LVGL client to use Fuse-X native windows
without running a Nano-X server. A Nano-X adapter remains useful for transition
and comparison. Neither is implemented by these probes. Preserve the existing
Nano-X clients and their lifecycle contracts while new applications use a
different path. Future Riplix x64 needs its own window service/backend; the
current migration plan is not evidence that it already has one.

Syma evaluation should not occupy the GUI event loop for an unbounded period.
The existing session engine has process-global storage and requires serialized
calls. A separate guest evaluator process is a candidate for responsive
execution and cancellation; arbitrary calls from multiple GUI threads are not
supported by the current API.

## X11 compatibility path

The official X.Org 21.1.25 source checks for protocol headers, Xtrans, Pixman,
libXfont2 and libxkbfile. It also needs keyboard data/tools and platform support
for the selected build. GPU/GLX, DRI, MIT-SHM, udev, systemd and network-listener
features have separate configuration choices; a first software server should
disable unneeded features rather than import a complete Linux desktop stack.

The pinned `hw/kdrive/meson.build` includes `src` and `ephyr`. Xephyr depends
on XCB and an existing X server. This release does not provide a standalone
framebuffer Xfbdev target that can simply be pointed at GX. Earlier references
to a small TinyX/Kdrive route need this qualification.

Investigate a custom software display backend using the X server's existing
framebuffer machinery, with Xvfb as a headless reference. A first visible
version could present one X desktop inside a GX window. Independent native
windows would require additional rootless integration; they do not follow
automatically from displaying the X framebuffer.

The C16 representation is a substantial obstacle here too. For example,
`include/misc.h` calculates protocol lengths through shifts by one and two
and assumptions about `sizeof` in octets. Reusing ordinary byte-addressed
source becomes more practical with the planned native application ABI.

Current Unix-domain streams and `select` are useful existing foundations.
The public `sys/mman.h` is empty and the socket header does not declare
`sendmsg`/`recvmsg` or descriptor transfer. A first core X11 path can avoid
MIT-SHM and DRI; shared-buffer extensions and a standard Wayland port would
need additional services. Wayland's standard transport explicitly passes
file descriptors and its surface model depends on buffer lifetimes, so it
is a larger platform contract than a replacement drawing library.

X11 is a route to source compatibility, not Linux binary compatibility.
Start acceptance with a small ordinary Xlib client exercising windows,
events, text and drawing. Then choose an existing application and audit its
exact extension and toolkit dependencies. A server alone does not supply
GTK, FLTK, C++, scalable text or all their operating-system requirements.

## XML layouts

LVGL 9.6.0's MIT source archive contains no `lv_xml_*` engine or `LV_USE_XML`
configuration option. XML examples on its website include the separate
LVGL Pro workflow. According to the project's
[XML engine announcement](https://lvgl.io/blog/announcement-lv_xml-removal-from-v9-5),
the loader moved out of the core in 9.5 and current on-device runtime XML
requires a separate license. Exported C depends only on the MIT LVGL library
and requires no guest XML parser.

The older 9.4.0 implementation includes a bundled Expat parser in
`src/others/xml/lv_xml.c`, rather than libxml2. Its configuration makes XML
optional. Upstream states that those older MIT implementations remain MIT;
using or adapting that implementation to the newer API would be a separate
compatibility and licensing review, not an implicit feature of this port.

Begin with C layouts. The official editor's C export is a possible later
authoring workflow under its applicable tooling terms. Runtime-editable XML
can be evaluated independently if it becomes a requirement. There is no need
to introduce a guest XML dependency to obtain LVGL layouts, styles or widgets.

## Recommended implementation gates

The calculator completes the first baseline for gates 1 and 2 below, with
numeric ASCII input and no document persistence. Keep those tests as regressions
when extending the profile. The file selector adds scrolling lists, paths,
selection and keyboard focus. Integer Retina density and bounded Ukrainian
font coverage are now tested. Full Unicode editing/shaping, runtime fonts,
save workflows and plotting remain later gates. The Data milestone now validates a
shared virtual table, menus and modal dialogs in the guest, including 100,000
rows, two-axis scrolling, sorting and persistent viewer preferences. It uses
a bounded visible-text cache with a directly painted LVGL viewport, rather than
allocating cell widgets or relaying out labels while scrolling. The gallery exposes the same widgets
on its Data page (F5). See the port README for limits and acceptance reports.

1. **Correct C16 rendering:** select a small LVGL profile and adapt buffer,
   color and font representations in a generated port layer. Match fills,
   masks, text, clipping and partial-update pixels against the host reference;
   reject unsupported formats explicitly. Complete this before treating
   successful source translation as a usable toolkit.
2. **One real Fuse-X application:** implement the window adapter, then verify
   resize, mouse capture, keyboard focus, Unicode text, clipboard, close/save
   and idle behavior. Keep the code reusable by a future native x64 backend.
3. **A shared widget example:** a code-cell editor using Rift's model, a bounded
   table and a plot object. Measure peak allocations, input latency, scrolling
   and bytes/pixels presented. Run this on Fuse-X; no RVM acceptance gate.
4. **A graphical Syma worksheet:** connect real evaluation and typed results,
   preserving the existing TUI and document semantics. Add responsive execution
   and explicit failure/cancellation behavior before large workloads.
5. **X11 as a separate native-platform project:** settle the native libc/window
   contract, build dependencies, run a core Xlib client, then port a named
   application. Keep this out of the critical path for the first LVGL app.

If the C16 rendering adaptation becomes disproportionate, pause that port
before changing shared compiler/runtime semantics and revisit the native ABI
milestone with its owners. The host reference remains useful for toolkit
development, but running host C outside the guest is not a delivered Fuse-X app.

## Sources and reproduction

* [Probe sources and commands](../../ports/lvgl/README.md)
* [Pinned archive identities](../../ports/lvgl/PROVENANCE.json)
* [LVGL 9.6.0 release](https://github.com/lvgl/lvgl/releases/tag/v9.6.0)
* [LVGL upstream repository](https://github.com/lvgl/lvgl)
* [XML engine changes](https://lvgl.io/blog/announcement-lv_xml-removal-from-v9-5)
* [Older XML implementation](https://github.com/lvgl/lvgl/blob/v9.4.0/src/others/xml/lv_xml.c)
* [Official X.Org source archives](https://www.x.org/releases/individual/xserver/)
* [Wayland protocol and surface lifecycle](https://wayland.freedesktop.org/docs/book/Protocol.html)

No normal disk image was opened or modified. Guest execution uses fresh,
explicit-manifest offline disks. The calculator, selector, gallery and data viewer
executables and their notices are staged into the shared folder; normal images
remain untouched.
