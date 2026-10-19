# FuseDE foundation

FuseDE is the custom RVM/FUZIX desktop. Shared application UI and the applications
bar must also compile for Fuse-X. The [implementation plan](../../docs/fusede/implementation-plan.md)
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
