# Building FuseDE

FuseDE is the custom desktop for **Riplix on RVM**. Its applications, shared
controls, artwork and dock must also build for Fuse-X. The desktop shell and
window-manager policy belong to RVM; Fuse-X keeps its native window/session
integration. This plan refines the [roadmap](roadmap.md) into the next deliverable
milestones. Remaining NXDSKTOP games and demos can proceed independently.

## Bitmap skins: buy, reuse or create?

The proposed eight border images are the perimeter of a **nine-slice** image:
four corners, four edges, and an optional centre. One image plus four inset
measurements is sufficient; separate files for each piece are unnecessary.
Corners keep their original size, top/bottom edges stretch horizontally,
left/right edges stretch vertically, and the centre fills the remaining area.
Omit centre painting when decorating an application's content. Visual slices,
content padding, and pointer hit areas are separate measurements.

Research checked on October 17, 2026:

| Source | What it supplies | Fit for FuseDE |
| --- | --- | --- |
| [Kenney UI Pack](https://kenney.nl/assets/ui-pack) | 430 UI assets, CC0 | A practical source of reusable buttons/panels and a reference for consistent states. It is artwork, not a guest widget implementation. |
| [Kenney UI Pack — Adventure](https://kenney.nl/assets/ui-pack-adventure) | 130 UI assets, CC0 | Useful alternative visual reference. Its illustrated style would need adaptation to our compact desktop. |
| [Godot NinePatchRect](https://docs.godotengine.org/en/stable/classes/class_ninepatchrect.html) | Documented margin, atlas-region, centre-fill and edge scaling behaviour | A useful behaviour reference; do not port the Godot runtime. |
| [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear) | Skinnable C GUI, MIT or public-domain option; input and rendering supplied by the application | Still our first widget candidate. A renderer and C16/performance evaluation are required. It does not replace Nano-X or the window manager. |
| [Enlightenment Edje](https://www.enlightenment.org/_legacy_embed/edje_main.html) | Image-based themes separated from program logic | A useful architecture reference. Adopting its theme runtime would be a separate large port; that is not required to get the same bitmap-border technique. |

The fit column is our assessment. Slate's core artwork remains original; a small
pinned Lucide icon selection is now imported separately. No new widget toolkit
has been adopted. Keep individual asset provenance/license
records if we reuse a pack later; availability on an asset site is not itself
permission to copy every file there.

**Recommendation:** own the small set of core skins and evaluate existing kits
for later icons. Creating high-quality small frames is quite feasible with
pixel-aligned geometry, controlled lighting and explicit interaction states.
Keep editable parameters or SVG masters, export at intended logical sizes,
inspect after RGB565 conversion, and test narrow/wide/tall windows. AI artwork
can help explore illustrated icons/wallpapers later; the slicing, masks and
state alignment still need deterministic production assets.

## Delivered foundation: Slate

[ports/fusede](../../ports/fusede/README.md) contains:

- A toolkit-independent C nine-slice planner, with at most nine rectangles and
  no allocation. Invalid metadata or a destination smaller than fixed borders
  fails before output changes. Insets and destination sizes use explicit C16
  bounds and widened addition.
- An original parameter-based **Slate** skin: active/inactive window frames,
  normal/hover/pressed/disabled/default buttons, inset field, panel and selection.
  Dark neutral surfaces, satin bevels and muted blue accents follow the existing
  visual direction. Masks have binary coverage; there is no alpha compositor.
- A repeatable host asset compiler producing PNGs, an RGB565 atlas, binary
  coverage mask, metadata, and C arrays. The browser study and software reference
  image demonstrate the artwork; the reference image uses the actual C planner.
- Native geometry/asset checks, a standalone C16 JIT RVM executable test, and
  the same C checks compiled as an independent Fuse-X `.fxe` and executed by its
  guest kernel. This establishes portability of the foundation, not a completed
  GUI library or a graphical application port.

Build the study from the repository root:

```sh
python3 ports/fusede/tools/build_theme.py
python3 ports/fusede/tools/test_skin.py --rvm --fuse-x
```

Open `build/fusede/slate/preview.html`. The checked-in JSON and generator are the
artwork sources; generated files belong under `build/`. Slate currently uses
23,016 atlas octets plus 1,644 mask octets, excluding fonts, icons, application
buffers and server bookkeeping. The compiler caps a source atlas at 128 KiB.
This is an authoring asset format, not a new enabled Nano-X wire format.

## A. Colour assets and a guest skin showcase

This milestone is **implemented**; see [the resource contract and usage](colour-resources.md).
The following design requirements describe that implementation and its remaining
font/performance follow-up. Our enabled Nano-X profile still has
`MW_FEATURE_IMAGES=0`, `MW_FEATURE_AREAS=0`, and no `GrArea` entry in its audited
request allowlist. The separate versioned image-resource request supplies colour
drawing without enabling unaudited upstream codecs and image calls.

Add the smallest versioned path for owned RGB565 resources: create, bounded
chunk upload, masked/copy drawing, nine-slice stretch and destroy. Reuse audited
upstream operations where suitable. Do not put client pointers into messages.
Dimensions/strides count pixels; file and transport lengths explicitly say
whether they count C16 cells or octets. Widen before multiplying and validate
complete rectangles, payload lengths and resource ownership before mutation.
Decode each little-endian asset octet explicitly: C16 `char` is not a host byte.

Upload a theme once per client, reuse it across repaints, and release it on
disconnect. Account against existing pixmap quotas; do not duplicate a complete
640×480 surface for every control. A later immutable shared cache must have
explicit lifetime/accounting. Keep skin parsing and rendering in a C library
usable by NanoWM and independent applications; expose an RFL wrapper when the
API is exercised by a real client.

Deliver `fdeshowcase` on **both** targets, showing all states, fixed corners,
border-only painting, clipping, and interactive resizing. Compare guest pixels
against the reference model; test malformed/oversized/truncated uploads,
disconnect cleanup, quota recovery, overlapping windows and server survival.
Record cold startup, warm input-to-frame latency, uploaded cells, peak RAM and
idle activity on RVM JIT. The application must block at idle and repaint damage
only. Add licensed, pre-rasterized readable fonts and icons as separate resources;
browser preview typography is not evidence that guest fonts exist.

## B. Window roles, borderless panels and a usable workspace

The first [workspace implementation](workspace.md) is delivered: shared window
enumeration, coalesced notifications, supported actions and stable client IDs;
Slate NanoWM frames, border/grip resizing, minimize/maximize/restore and Alt+Tab;
RVM panel reservations/focus/stacking and application survival across a manager
disconnect. Fuse-X exposes its native capability limits and ordinary-window
panel fallback. Popup capture, modal policy, automatic manager restart and
native borderless chrome remain follow-ups.

Add visible resize affordances, generous hit areas independent of the thin
artwork, minimize/maximize/restore, keyboard switching, window enumeration and
work-area reservations. Keep client identity stable across reparenting and
frame changes. Send close requests to clients so editors can offer save/cancel.
Track actual focus and capture; handle client death during drags and dialogs.

Define roles independently of decoration:

| Role | RVM behaviour | Fuse-X application behaviour |
| --- | --- | --- |
| Normal | Managed, ordinarily decorated, participates in switching | Normal native window |
| Dialog | Owned/transient; explicit modal input rules and close policy | Owned native top-level using the same dialog content |
| Panel/dock | Borderless; explicit edge, stacking and reserved work area | Standalone applications-bar window; no guest desktop reservation |
| Popup | Borderless, transient input/capture and dismiss-on-cancel | Popup/transient mapping with a rectangular fallback |

Nano-X defines `GR_WM_PROPS_NODECORATE`, and NanoWM skips decorating windows
with it. The workspace panel role now sets that property and a zero server
border, and supplies reserved work areas, stacking, window lists and focus
policy. Register a panel through that API rather than setting decoration bits
alone.

The inspected Fuse-X adapter currently maps fixed-size/hidden state to GX; it
does not map `NODECORATE` to native borderless chrome. Add an explicit native
capability and translation before promising the same appearance there. Initially
the bar can use an ordinary native window if that capability is unavailable.

Treat **borderless**, **binary window shape**, **alpha transparency** and
**pointer pass-through** as separate capabilities. A dock needs no titlebar,
but does not require blending or blur. RVM has a bounding-region path; the native
window's visual/input shape needs separate host support. Never imply that a
transparent bitmap pixel makes its surrounding native window transparent or
lets clicks reach another app. Start with opaque rectangular panels; add shape
and alpha only with damage, input and resource rules defined.

Normal windows remain reachable after panel changes or resolution changes.
Test borderless windows receive no default frame, panel clicks do not steal
typing focus unnecessarily, keyboard access remains possible, and maximize
respects the usable work area. Test both backends' fallbacks explicitly.

## C. Shared controls and application portability

Run the existing Nuklear file-list evaluation with the skin renderer. Compare
allocation, redraw traffic, latency and keyboard operation on both targets.
Choose it only after that test; retain the focused C-widget fallback described
in [architecture](architecture.md). Prefer a bounded number of client windows,
event-driven updates and cached assets. A polished theme does not supply focus,
text editing, scrolling, popup grabs or accessibility by itself.

Applications use guest files/processes and Nano-X drawing/events. Keep RVM's
root window, physical display, panel reservations and NanoWM internals out of
application code. The platform adapter owns window realization and native
session policy. C16-safe data handling is required for Fuse-X too: its hosted
application ABI is not ordinary host C. Keep the public layer usable from C and
RFL without leaking Nuklear structures into IPC.

Every delivered GUI application needs two build targets (`.rxe` and `.fxe`) and
two acceptance runs. Compile and install independently of either kernel. Check
resize, keyboard/pointer input, multiple simultaneous clients, native close and
cleanup; closing one app must preserve the others. Mark unsupported capabilities
and fallback behaviour explicitly. Cross-compiling the current geometry checks
does not yet validate any of these future GUI behaviours.

## D. Shell and dock as separate applications

Build wallpaper/session top bar and **fdebar**, the dock, as separate clients.
Keep the dock's item model, layout, selection, launch/activate actions and theme
independent of its host window. Use orientation and edge settings rather than
hard-coded bottom-screen coordinates:

```ini
[bar]
orientation=horizontal
edge=bottom
icon_size=24
```

This is the proposed configuration contract; its parser/application is planned.
The vertical variant uses the same model with `orientation=vertical` and a
left/right edge. Layout follows the available main axis, keeps icons upright,
and handles overflow with scrolling and keyboard navigation. Do not rotate the
whole framebuffer or assume a constant display size.

On RVM, register the bar as a borderless panel with NanoWM, reserve its edge,
and obtain running/active/minimized windows from the window-management service.
On Fuse-X, build the **same application** as an independent `.fxe` applications
bar in its own native window, horizontal or vertical. Show Fuse-X guest apps;
launch with explicit argument vectors and activate their existing native windows
through the hosted adapter. It does not need to replace the macOS Dock or manage
unrelated host apps. Hosted mode does not reserve a guest desktop edge. Closing
the bar leaves running applications and the Fuse-X session alive.

Deliver launchers first, then window switching once subscription/activation is
ready. Handle stale IDs, app exit while a menu is open, launching failures and
multiple instances. Test both orientations, overflow, focus handoff, minimize/
restore and independent bar restart on both platforms. Keep NXDSKTOP available
until the custom shell passes equivalent session and recovery tests.

## E. File manager and services

Grow the evaluated file-list client into the graphical file manager. Reuse guest
filesystem operations and existing INI facilities where practical. Add navigation,
selection, open, rename and cancellable file operations with visible failures.
Cross-device moves require copy-and-delete with correct failure handling.
Add file associations, clipboard, drag-and-drop and published application actions
through separate versioned contracts. Native menu translation is a later adapter
step. Neither drag-and-drop nor a global menu follows automatically from skins.

Plan installed artwork under `/usr/share/fusede/themes/slate/`, system selection
under `/etc/fusede/theme.ini`, and user overrides under
`$HOME/.config/fusede/theme.ini`. Reuse the shared INI reader; keep GUI skin
semantics separate from curses theme roles. Support a built-in fallback, validate
inheritance/cycles and apply a replacement theme atomically. System/user
theme selection is planned; the normal image builders install the shipped assets.

The colour-resource milestone now installs its theme and icon data under
`/usr/share/fusede/` through the normal image builders. System/user theme
selection and inheritance remain planned; the showcase uses matching compiled
metadata and loads the corresponding `.fdi` data files.

Keep the first desktop useful at 640×480. Larger resolutions, alpha/shadows,
animations and richer font coverage follow measured memory/rendering work, not
the existence of a higher-resolution mockup.
