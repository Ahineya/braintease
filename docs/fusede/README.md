# FuseDE desktop plan

FuseDE is the planned custom desktop for Riplix on RVM. Its shared application UI,
applications and dock must also compile for Fuse-X. The agreed direction on
October 17, 2026 is to port NXDSKTOP and its
companion applications first, giving us a useful lightweight desktop while we
develop the custom environment. Nano-X remains the graphics and window server.

RVM presents one guest desktop inside one host window. Fuse-X presents
applications in independent native macOS windows under one Fuse-X application
identity. Sharing application code and widgets must preserve these different
window-management models.

| Document | Contents |
| --- | --- |
| [Architecture](architecture.md) | Desktop components, toolkit evaluation, visual design and platform boundaries |
| [Roadmap](roadmap.md) | NXDSKTOP and companion app ports, Nuklear prototype, custom shell and file manager |
| [Implementation plan](implementation-plan.md) | Skin research, original Slate assets, colour rendering, borderless roles and portable applications bar |
| [Colour resources and showcase](colour-resources.md) | Implemented guest image API, asset files, icon pipeline, usage and validation |
| [Graphics foundation](../graphics/README.md) | GX, Nano-X and existing graphics validation |
| [Fuse-X desktop integration](../fuse-x/desktop.md) | Implemented native windows, menus and session lifetime |

## Agreed decisions

- Port NXDSKTOP as a usable initial RVM desktop, retaining it as a lightweight
  alternative when FuseDE becomes available.
- Port its companion applications as independent programs, useful both within
  a guest desktop and in Fuse-X native windows.
- Build the custom desktop incrementally on Nano-X. Evolve NanoWM rather than
  replacing the server or writing another graphics stack.
- Evaluate Nuklear first for shared widgets. Use a representative file-list
  prototype to decide whether its C16 adaptation, performance and memory use
  justify adopting it.
- Aim for dark, bitmap-based chrome inspired by older skeuomorphic macOS:
  illustrated icons, subtle bevels, inset controls and restrained highlights.
- Give the custom RVM desktop a file manager, an application dock, and a top
  menu/status bar with a clock and service icons.
- Support borderless panels and make the dock a separate application with
  horizontal/vertical layout, also buildable as a Fuse-X applications bar.
- Keep native Fuse-X chrome and session controls independent of guest desktop
  policy. Shared widgets and application actions should work in either mode.

## Implementation status

The RVM Nano-X server, NanoWM, launcher, graphical terminal, Eyes, Clock and
Roaches are implemented. Fuse-X has a persistent Nano-X service, native
top-level windows, a launcher and graphical terminals; its native window
integration is described in the linked platform documentation. Root-desktop
effects such as Roaches do not automatically transfer to macOS windows.

NXDSKTOP is now the basic RVM desktop selected by `startx`; `startx --minimal`
retains the original launcher. Message boxes (`nxmsg`), file selection
(`nxselect`) and Calculator (`nxcalc`) are installed on RVM and built/installed
as native Fuse-X companions. NanoTetris (`nxtetris`) also runs on both platforms,
with keyboard/mouse controls and persistent high scores. Edit file opens Levee through a graphical terminal.
The shared-library image viewer (`nxview`) opens PNG, JPEG, GIF and BMP with
fit/actual-size, zoom, panning and resizing on both systems.
The shell uses our resource reporting, asynchronous dialog results and session
shutdown services. See the [runtime guide](../../ports/nanox/README.md).

The [skin foundation](../../ports/fusede/README.md) now includes owned colour-image
transport, clipped nine-slice rendering, guest asset loading and `fdeshowcase`
builds for RVM and Fuse-X. It ships original Slate skins and 20 pinned Lucide
icons rasterized at 16/24/32 pixels. See [usage and validation](colour-resources.md).
NanoWM now uses Slate decorations and provides border resizing, minimize,
maximize/restore and Alt+Tab. The shared workspace service supplies stable client
IDs, enumeration, change notifications and actions on both platforms, plus RVM
panel reservations. See [workspace services](workspace.md) for controls,
capabilities and the native fallback. The remaining companion apps, Nuklear
evaluation, shared widget toolkit and custom shell/dock remain planned.
