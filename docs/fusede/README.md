# FuseDE desktop plan

FuseDE is the planned custom desktop and shared application UI for FUZIX/Ripple
and Fuse-X. The agreed direction on October 17, 2026 is to port NXDSKTOP and its
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
- Keep native Fuse-X chrome and session controls independent of guest desktop
  policy. Shared widgets and application actions should work in either mode.

## Implementation status

The RVM Nano-X server, NanoWM, launcher, graphical terminal, Eyes, Clock and
Roaches are implemented. Fuse-X has a persistent Nano-X service, native
top-level windows, a launcher and graphical terminals; its native window
integration is described in the linked platform documentation. Root-desktop
effects such as Roaches do not automatically transfer to macOS windows.

NXDSKTOP, its additional companion app ports, the Nuklear prototype and the
custom FuseDE components below are planned. Source availability in the vendor
archive does not mean an application is built or installed.
