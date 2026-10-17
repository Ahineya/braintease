# Pinned Nano-X audit

The imported Microwindows/Nano-X revision is
`2bf6c0ad1f08af0f0eb6f391d0c1e1d7b7848cab` (2026-10-13).
See [provenance](../../ports/nanox/UPSTREAM.md) for archive verification and
retained source scope. This audit concerns that source, not every historical
Nano-X configuration.

## Findings

| Area | Evidence in pinned source | Ripple work |
| --- | --- | --- |
| Preprocessing | include/mwconfig.h uses `#include INCFILE(MWCONFIG_FILE)` | Fixed in RCC: expand macro-based includes when active, skip inactive includes, preserve literal header names and reject invalid expanded operands; no Nano-X workaround required |
| Fundamental types | include/mwtypes.h derives coordinate limits and image-bit counts with `sizeof(type)*8` | Drawing profile fixes widths, screen sizes, bitmap pitches and dash masks in generated sources; protocol/image types still need auditing |
| RGB565 addressing | drivers/fblin16.c uses unsigned-char addresses, x shifted by one, and octet pitches | The C16 screen adapter uses one cell per RGB565 pixel and cell pitches; file/image octets remain separate future work |
| Nano-X wire protocol | nanox/nxproto.h declares BYTE8 and native request structures, size/alignment rules | Implemented a versioned private C16 local protocol with bounded framing and payload validation; matching target layouts deliberately differ from the upstream octet wire ABI |
| IPC/readiness | nanox/client.c, srvmain.c, srvnet.c use Unix sockets/select | Guest select readiness now exists on RVM/native, including RVM graphics and PTYs; AF_UNIX stream IPC now exists on both targets; the split server now uses checked incremental framing and queued nonblocking output |
| Timer sources and arithmetic | engine/devtimer.c uses gettimeofday and subtracts timeval fields; drivers/osdep.c quantizes GdGetTickCount to 25 ms; srvmain.c has an optional 8088 timeout approximation | Implemented monotonic sampling, normalized C16 carries/expiry and exact conversions in the generated server/platform adapters; kernel deadlines remain unchanged |
| Native windows | nanox/srvfunc.c, srvutil.c own map/move/resize/destroy and hierarchy; PSD->Update reports screen rectangles | Export top-level lifecycle and independent surfaces above PSD, including event-coordinate/focus mapping |
| Terminal | demos/nanox/nxterm.c has GNU case ranges (`MWKEY_F1 ... MWKEY_F12`), legacy PTY/platform conditionals and VT52/ANSI handling | Expand case labels and adapt PTY/session APIs if reused; the first terminal uses a small shared C16 stream model |
| Small GUI clients | demos/nanox/nxeyes.c uses bitmap window masks, math, pointer events and blink timers; nxclock.c uses timed event waits and wall time | Implemented both clients through generated adaptations; bitmap-region payloads are checked and shaped-child clipping is corrected; Clock guards initial negative hand angles |
| Roaches | demos/nanox/nxroach.c uses window tree/visible regions, bitmaps and floating point | Ported with upstream movement, converted C16 masks and transparent bitmap drawing; readbacks and orientation pixmaps are unnecessary; frame pacing, bounded options and safe teardown are port-local |
| Optional dependencies | config, Arch.rules and drivers select many host/image/font backends | Start with RGB565, compiled bitmap fonts, simple input and no optional host libraries |

`ports/nanox/tools/audit.py` compiles representative unmodified engine, driver,
server, terminal and roach files with RCC and saves commands' flags, diagnostics
and compiler hash under `build/nanox-audit`. After the RCC include fix on
2026-10-17, unmodified `devclip.c`, `devrgn.c`, `fblin16.c`, `kbd_null.c` and
`nxroach.c` compile to RCC assembly. Previously five probes stopped at a
macro-based include inside an inactive configuration branch. The server probe
now reaches an unresolved `fd_set` declaration (`rfds`); the terminal probe
still reaches unsupported GNU case-range syntax. The initial roach probe
exposed a missing imported dialog resource header, which is now retained.
These are compile-only results: the units are not linked, and
bitmap octet/cell conversions, drawing/window APIs and runtime behavior still
need adaptation and tests. The blocked probes do **not** establish how much
remaining source compiles after their first blockers are fixed.
The addressing/protocol/window findings come from source inspection, and still
need runtime tests after adaptation. Do not “fix” compilation by making RCC's
char eight bits or importing GNU C extensions into the compiler indiscriminately.

The adapted drawing profile now links and runs as `nxdraw` on JIT RVM. Its
integration test checks clipping/regions, font and bitmap layout, raster
operations, offscreen and self-overlapping blits, display cleanup/reopen and
every pixel of the presented demo against an independent model. This is a
separate generated build, not a claim that the unmodified driver or server
works. See [engine build and test details](../../ports/nanox/README.md).

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

The separate server/client profile now runs on RVM with upstream NanoWM,
text-only `nxlaunch`, and the shared-model `nxterm`. It is a separate executable
per process, using the private C16 socket and checked request allowlist; it is
not NONETWORK/single-address-space mode. The JIT desktop suite exercises actual
client connections and timers, malformed/partial requests, launcher-created
terminals, shell input/files, NanoWM movement/resize and session close/reopen.
The native Fuse-X GX backend, optional image codecs and complete upstream
protocol coverage are not implied by this milestone.
