# FuseDE architecture

FuseDE builds on the existing Nano-X server, client protocol, raster engine
and platform graphics adapters. Desktop policy, reusable widgets and individual
applications remain separate so the initial NXDSKTOP environment and the
custom shell can share applications.

## NXDSKTOP as the initial desktop

The retained [NXDSKTOP source](../../ports/nanox/upstream/src/demos/nanox/nxdsktop.c)
identifies itself as version 1.1 and targets slow 8086/8088 systems. It provides
a Start menu, clock, memory display, application launching and helpers for file
selection, image viewing and editing. Its interface uses Nano-X drawing calls
directly; it is not a general widget library or a full file manager.

Adapt that implementation into a practical RVM desktop before building the
custom shell. Retain its simple appearance initially. Replace ELKS-specific
memory queries and session commands with our platform services. Launch the
ported editor through our existing graphical terminal and Levee. The desktop
and its apps must have independent lifetimes: closing a dialog or application
must not stop the desktop or kernel.

Keep the existing Nano-X import pristine and generate adaptations alongside
the current port. Record any additional source or asset imports through the
[provenance workflow](../../ports/nanox/UPSTREAM.md). The
[NXDSKTOP release](https://github.com/toncho11/microwindows/releases/tag/version_1.1)
is the upstream project reference; our recorded vendor revision remains the
reproducible source baseline unless deliberately updated.

## Component boundaries

| Component | Responsibility |
| --- | --- |
| Nano-X server and engine | Drawing, clipping, resources, client isolation and input delivery |
| RVM window manager | Decorations, focus, stacking, move/resize, minimize/maximize and usable work area |
| Desktop shell | Wallpaper, dock, top bar, clock, status indicators and application launching |
| Shared GUI library | Layout, controls, focus, keyboard navigation, scrolling, dialogs and theme |
| Applications | File manager, editor, image viewer, calculator, games and preferences |
| Fuse-X host integration | Native top-level windows, macOS chrome, Window/Dock menus and session controls |

Evolve NanoWM for the RVM role. The shell needs a bounded interface for
enumerating windows, observing titles and focus, activating/restoring windows
and reserving panel space. The dock must distinguish launching an app from
activating an existing window. Minimized windows need a reliable restore path.
These capabilities are planned additions, not assumptions about today's
launcher.

## Shared widgets and Nuklear evaluation

The first toolkit candidate is [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear),
a C immediate-mode UI library. Our full Microwindows archive already contains
a Nuklear demo and a Nano-X rendering adapter. They are source material for a
port, not an enabled widget library. The current `NUKLEARUI` configuration
selects decoration styling only.

Evaluate one resizable application with a toolbar, editable path field,
scrolling file list, popup menu and confirmation dialog. Exercise pointer and
keyboard focus, selection, text editing and resizing. Measure idle activity,
input-to-frame latency, scrolling, allocation peaks and protocol traffic on
RVM JIT and native Fuse-X.

Nuklear uses floating-point layout; its retained Nano-X renderer clears and
redraws the application surface during rendering. Adapt fixed-width types,
C16 character/octet boundaries, allocation sizes and optional modules
explicitly. Block on input, exposure or a real timer while idle. Immediate-mode
UI does not require a continuous redraw loop. Measure redraw costs before
adding command caching or partial painting.

TinyWidgets is a fallback reference in the same archive. It has common controls,
but its inspected implementation uses individual Nano-X windows and graphics
contexts for widgets. Account for that resource model before choosing it.
If Nuklear's adaptation or runtime cost is unsuitable, implement a focused C
widget library against the prototype's requirements. Toolkit selection remains
open until that evaluation; the desktop direction does not depend on its outcome.

The reusable layer must cover buttons, labels, editable text, lists,
scrollbars, menus, dialogs and layout, including disabled/selected/focused
states and keyboard operation. Keep it usable by independent client processes.
Avoid exposing toolkit-specific structures in desktop service protocols.

## Theme and graphics assets

Use a shared dark palette with charcoal frames, inset fields, subtle bevels,
illustrated icons and glossy accents for active controls. Bitmap artwork should
support resizing through nine-slice rendering: fixed corners, scalable or tiled
edges, and a fillable center. Define normal, pressed, hovered, disabled and
focused states consistently across shell, decorations and applications.

Add bounded color-image upload and drawing to the enabled Nano-X protocol.
Decode file octets explicitly into C16 pixel storage. Cache RGB565 assets and
use masks or color keys where appropriate. The existing monochrome bitmap
path is useful groundwork, but it is not a general color image API. Preserve
resource quotas, ownership and disconnect cleanup for every added operation.

DMA accelerates movement of prepared pixels into RVM scanout. It does not
decode images or supply alpha composition. Start with opaque/precomposed
artwork and masks; add blending or desktop shadows when their rendering and
damage costs are understood. Add readable proportional fonts early, with
matching text measurement. International text and clipboard behavior need
explicit follow-up contracts.

Set memory budgets for asset caches, backing surfaces and window counts.
An RGB565 640×480 surface occupies 600 KiB of physical octets; 1024×768 needs
1.5 MiB. RVM's current 640×480 display ceiling requires scanout allocation,
device, server and resource work before larger desktop sizes can be promised.
Host window scaling alone does not create more guest workspace.

## Menus and application actions

Begin the RVM top bar with desktop/session menus, a clock and service status.
A true global application menu needs an application to publish its menu tree,
enabled/checked state, shortcuts and action identifiers. Route actions to the
focused application through a bounded, versioned interface, removing stale
registrations when clients exit. Legacy Nano-X apps remain usable without
publishing a menu.

Design these as application actions rather than screen coordinates or toolkit
callbacks. Fuse-X can later present the same actions through native macOS
menus. Clipboard, file associations and drag-and-drop are separate service
contracts; drawing matching controls does not implement those behaviors.

## Platform behavior

RVM runs NXDSKTOP or the custom shell with a guest window manager inside its
single host display. The shell reserves room for the top bar and dock. Closing
that graphics session follows the established RVM teardown behavior without
implicitly shutting down FUZIX.

Fuse-X keeps its existing native window model. Companion apps should launch
directly as native top-levels, without a guest NanoWM or an obligatory nested
desktop. NXDSKTOP's root background, system commands and child-process shutdown
policy must not be copied into hosted mode unchanged. If a hosted NXDSKTOP
launcher is useful, adapt it as a normal application window with platform-aware
commands; companion app availability does not depend on that launcher.

Keep shared application drawing, menus and document logic independent of guest
decorations. Continue to use the existing native session lifetime, close,
resize and terminal-detachment rules in the
[Fuse-X desktop contract](../fuse-x/desktop.md).
