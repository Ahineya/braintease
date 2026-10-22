# FuseDE foundation

This is the earlier Nano-X/Slate desktop experiment. New FuseDE development
follows the [Graphite roadmap](../../docs/graphics/graphite-roadmap.md), using
the shared LVGL components and the portable single-window desktop policy.
The code and assets here remain available for the existing RVM desktop and
compatibility tests; they are not the foundation for the new application UI.
Lucide source artwork continues to feed the Graphite icon pipeline.

The original [implementation plan](../../docs/fusede/implementation-plan.md)
records the asset research, current gaps, borderless/window roles, portable dock
and next guest milestones.

This directory provides skin/icon authoring, C nine-slice geometry/rasterization,
a Nano-X image API, an asset-file loader and a real `fdeshowcase` application.
See [guest usage and the resource contract](../../docs/fusede/colour-resources.md).
The [workspace service](../../docs/fusede/workspace.md) adds stable window
enumeration, notifications and supported actions on both platforms. NanoWM uses
Slate frames, border resizing, minimize/maximize/restore and RVM panel
reservations. `fdectl list` inspects the shared service. The desktop shell,
general widget toolkit and dock are subsequent milestones.

## Build and inspect

From the repository root:

```sh
python3 ports/fusede/tools/build_theme.py
python3 ports/fusede/tools/build_icons.py
python3 ports/fusede/tools/test_skin.py
python3 ports/fusede/tools/test_raster.py
python3 ports/fusede/tools/test_showcase.py
```

Open `build/fusede/slate/preview.html` locally. No web server, package download or
Python dependency is required. A host C compiler is needed; `CC` and `SDKROOT`
can override it. On macOS, the default SDK comes from the selected Xcode toolchain.
The HTML study includes control states and a resizable nine-slice sample. Its
browser text/icons are layout illustrations, not guest implementations.
`study.png` is a static counterpart using the existing guest ROM 8×16 font;
its upstream license is emitted as `preview-font-license.txt` beside the image.

The guest showcase checks need the existing RCC/RASM/RLINK/runtime build, the
RVM kernel and `.rxe` clients in `build/fuzix`, and the hosted kernel/`.fxe` clients
in `fuse-x/build`. Bun builds the disposable images. Tests exercise the real
protocol, file loader, ownership/quotas/cleanup, pixel output and session lifetime.
They never install onto a user's root disk. `test_skin.py --rvm --fuse-x` remains
available as the smaller standalone geometry smoke test.

`include/fde_skin.h` defines up to nine source/destination rectangles. A backend
adds origins, applies clipping/masks and paints each patch, preserving sampling
coordinates when clipping. Return `-1` means invalid metadata or a destination
smaller than the fixed border. The function leaves output unchanged on failure.
The first implementation stretches; patterned tiled edges are a future explicit
mode. Content insets, window hit-testing and theme metrics are separate APIs.

## Drawing performance probe

With the RVM kernel, Nano-X client objects and applications already built:

```sh
python3 ports/fusede/tools/profile.py --width 560 --height 400
```

This builds only a measurement client and runs it on a disposable disk with
headless JIT RVM. The desktop binaries and normal user disks are unchanged.
It compares fills, scanline drawing, skin painting, prepared-frame copies,
overlapping copies and moves, plus offscreen and single-pixel baselines.
Each sample waits for server processing and the preceding presentation; it
does not measure physical mouse-to-monitor latency. NanoWM is absent so the
move case isolates the server/driver path. Early samples can compile newly
hot JIT paths; the summary uses the final five of twelve samples.
Results, execution/JIT statistics and transcripts are saved under
`build/fusede/profile`. `--output`, `--samples`, `--width` and `--height` select
separate runs and workload sizes. The private filesystem is checked before
and after the run.

## Graphite application catalog

The guest Graphite shell reads the files in `catalog/`, installed as
`/usr/share/applications/*.ini`. `ports/lvgl/shell-catalog.c` shares the existing
INI reader and keeps a dynamically allocated catalog. Files sort by category,
display name and stable ID. Search matches words against names, categories,
keywords and IDs; Latin ASCII letters match without regard to case, and UTF-8
text is preserved. Availability is checked on each request rather than cached.

Each file describes one application:

```ini
[application]
id=org.riplix.Commander
name=Commander
icon=folder
category=Accessories
keywords=files folders copy move rename
kind=terminal
exec=/bin/mc
fallback=/mnt/host/rfl-fm.fxe
columns=80
rows=28
pinned=true
# Optional exact arguments, contiguous and numbered from one:
arg1=/mnt/host
```

`id`, `name`, `icon`, `category`, `kind` and `exec` are required. `kind` is `gui`
or `terminal`; only terminal entries accept `columns` and `rows`. `keywords`,
`fallback`, arguments and `pinned` are optional. IDs use 1–63 ASCII letters,
digits, dots, hyphens or underscores. Metadata is validated as UTF-8; names and
categories fit 63 cells, icons 127. Each input file has a 1 Mi-cell budget and
4096-cell line budget. These are input budgets, not a fixed application count.
Unknown keys, duplicate keys/IDs, invalid paths and gaps in argument numbering
reject the whole load without changing the live catalog.

Executables must have absolute paths. The installed path is preferred when it
is a regular executable file; a `fallback` names one explicit alternative for
host-share testing. The terminal's own installed/fallback paths are configured
separately by the shell. Missing applications and missing terminal support stay
out of the available application list. File modes matter. Configuration never
becomes a command string: the launcher forks and executes the exact argument
vector without shell evaluation. INI matching outer quotes are removed; there
are no shell escapes or environment substitutions.

GUI IDs match the applications' existing GX registrations. Terminal entries
run through `gterm --app-id ID --app-name NAME --app-icon ICON -- PROGRAM ...`.
Gterm registers the application's identity and labels its initial window with
the display name. The application and its shell/helpers do not become extra
dock entries. Multiple launches share the same app ID but retain separate
instance and window identities. Ordinary `gterm` still identifies as Terminal.
The current catalog includes the five Graphite GUI apps, Terminal, Commander,
Rift, Music, Telegram, Chat, Multiplexer and JavaScript. Legacy Nano-X apps such
as NetSurf and `nxview` remain earlier porting experiments; their catalog
integration is not a current goal, and browser choice will be revisited later.

The Applications menu also offers **Run…**. `shell_catalog_run` parses a
program name/path and optional quoted arguments, preserving catalog identity
and GUI/TUI policy when matched. Other commands use explicit paths or `PATH`;
terminal mode wraps them in gterm with a derived launch identity. Launches use
the same asynchronous exec/error/reaping path as catalog entries. Quotes and
backslash escapes group arguments; shell expansion, pipes and redirection are
not implicit. The dialog retains its command and shows errors until corrected
or canceled.

Catalog `pinned` values supply defaults. User preferences override them by ID:

```ini
[pins]
org.riplix.Calculator=false
org.riplix.Commander=true
```

The checked-in `pins.ini` is an empty override template. Unknown valid IDs
survive load/save, so a temporarily unavailable app retains its preference.
Saving uses an exclusive mode-0600 sibling temporary file, closes it, then
atomically renames it over the selected user configuration. Host builds also
call `fsync`; guest crash durability awaits a linkable libc `fsync`. No system catalog
or unrelated user preferences are modified. Child exec errors and exits are
reported asynchronously; polling never blocks or reaps unrelated children.

Run the parser, pin, dynamic-availability and actual fork/exec tests with:

```sh
python3 ports/lvgl/test_shell_catalog.py
```

## Sources and generated assets

`themes/slate.json` and `tools/build_theme.py` author the original pixels. The
generator emits PNGs, `atlas.rgb565`, `atlas.mask`, `theme.fdi`, `theme.json` and `theme.h`.
Files count octets; C arrays count C16 cells. RGB565 and mask words serialize as
little-endian 16-bit integers; mask rows are padded to 16 bits, most-significant
bit first within each word. Mask bit one paints; zero preserves the destination.
The software preview applies that mask and uses the actual C planner.

The atlas has a one-pixel transparent gutter around every asset. Metadata stores
`[x,y,width,height]` regions and `[left,top,right,bottom]` source insets. It is
versioned authoring metadata, not a stable transport ABI. The C header supplies
equivalent pixels/mask arrays and descriptors for built-in fallback rendering.
`include/fde_nano.h` loads and paints owned server images; `image-file.inc`
decodes the bounded `.fdi` file format on both C16 targets.

The [icon pipeline](icons/README.md) retains a small, pinned Lucide SVG selection
with ISC/Feather MIT notices and pre-rasterized masks; normal builds are offline.
The static layout study reads the repository's existing licensed ROM font. New C code uses
GPL-2.0-or-later, matching the Nano-X port's local platform code. Original Slate
artwork and generated pixel assets are dedicated under
[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/); see
[theme notes](themes/README.md). The original browser study's icons remain layout
placeholders; the guest showcase uses the real imported masks.
