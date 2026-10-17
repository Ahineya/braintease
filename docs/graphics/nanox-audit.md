# Pinned Nano-X audit

The imported Microwindows/Nano-X revision is
`2bf6c0ad1f08af0f0eb6f391d0c1e1d7b7848cab` (2026-10-13).
See [provenance](../../ports/nanox/UPSTREAM.md) for archive verification and
retained source scope. This audit concerns that source, not every historical
Nano-X configuration.

## Findings

| Area | Evidence in pinned source | Ripple work |
| --- | --- | --- |
| Preprocessing | include/mwconfig.h uses `#include INCFILE(MWCONFIG_FILE)` | Replace configuration indirection in an adapter/patch; RCC compile probes currently stop at an invalid include directive |
| Fundamental types | include/mwtypes.h derives coordinate limits and image-bit counts with `sizeof(type)*8` | Use actual integer widths; C16 char/int are both 16 bits |
| RGB565 addressing | drivers/fblin16.c uses unsigned-char addresses, x shifted by one, and octet pitches | Convert to pixel/cell addressing and separate file/transport octets from memory cells |
| Nano-X wire protocol | nanox/nxproto.h declares BYTE8 and native request structures, size/alignment rules | Define a deliberate C16 local protocol or explicit octet codec; native struct layouts are not upstream wire-compatible |
| IPC/readiness | nanox/client.c, srvmain.c, srvnet.c use Unix sockets/select | FUZIX/Ripple currently exposes AF_INET, not the required local sockets/select layer; provide local IPC and readiness before a real split server |
| Native windows | nanox/srvfunc.c, srvutil.c own map/move/resize/destroy and hierarchy; PSD->Update reports screen rectangles | Export top-level lifecycle and independent surfaces above PSD, including event-coordinate/focus mapping |
| Terminal | demos/nanox/nxterm.c has GNU case ranges (`MWKEY_F1 ... MWKEY_F12`), legacy PTY/platform conditionals and VT52/ANSI handling | Expand case labels and adapt PTY/session APIs if reused; the first terminal uses a small shared C16 stream model |
| Roaches | demos/nanox/nxroach.c uses window tree/visible regions, bitmaps and floating point | Port after window/region drawing and server IPC work; source and bitmap assets are present |
| Optional dependencies | config, Arch.rules and drivers select many host/image/font backends | Start with RGB565, compiled bitmap fonts, simple input and no optional host libraries |

`ports/nanox/tools/audit.py` compiles representative unmodified engine, driver,
server, terminal and roach files with RCC and saves commands' flags, diagnostics
and compiler hash under `build/nanox-audit`. Five engine/driver/server probes stop at
the configuration include directive. The terminal probe reaches unsupported GNU
case-range syntax. The initial roach probe exposed a missing imported dialog
resource header. With that upstream header retained, unmodified `nxroach.c`
compiles to RCC assembly. This is a compile-only result: it is not linked, and
bitmap octet/cell conversions, drawing/window APIs and runtime behavior still
need adaptation and tests. The blocked probes do **not** establish how much
remaining source compiles after their first blockers are fixed.
The addressing/protocol/window findings come from source inspection, and still
need runtime tests after adaptation. Do not “fix” compilation by making RCC's
char eight bits or importing GNU C extensions into the compiler indiscriminately.

## Adaptation strategy

Keep the imported tree pristine and maintain an explicit patch/adapter layer.
Name widths in the code and define whether every length is a C16 cell, pixel,
logical character or file octet. Introduce tests around the conversion seams.
Limit the initial engine configuration aggressively, then widen features when
needed. Reuse tested drawing/region/font/application code while keeping the GX
contract independent of Nano-X internals.

The MPL/GPL source notices and per-file licenses remain with the import.
The first terminal's 8×16 atlas is derived mechanically from the pinned compiled
ROM font; its provenance and reproducible verification live with the import.
