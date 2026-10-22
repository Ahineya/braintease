# Native GX graphics and Graphite desktop

Physical acceptance and the boot/SSH handoff are indexed in
[hardware/README.md](../hardware/README.md). The retained GUI receipt identifies
entry `0003` and its exact candidate. The later desktop trial is entry `0004`;
it boots but has interaction/performance issues. A fresh rebuild needs its own
acceptance.

`boot/gx.c` provides the existing GX transport over bounded native RAM surfaces.
The C16 kernel continues to own `/dev/gfx`, Unix descriptors and window leases;
`gx_call` accepts only numeric arguments and callback-validated C16 cell spans.
Legacy host-create format 0 remains RGB565, matching the existing hosted
backend and older frozen kernels. Handles carry generations, and mutating/data-reading calls require their native
context owner. Kernel-wide readiness polling is the sole handle-only operation.

The accepted initial profile displays one active surface at a time, with no compositor or
window chrome. It supports RGB565/XRGB8888 windows, RGB565/BGR logical-octet
uploads, scaled/indexed uploads, fill/copy, configure, explicit present, raw HID
make/break events and state. Pointer movement, three button edges and signed
16.16 wheel events use the same GX event stream. Alt+F4 sends GX_CLOSE, so an
existing LVGL app exits normally without an SSH kill. Native framebuffer conversion and console handoff
belong to the platform callback. Upload/fill/copy never publish implicitly.
BGR blend matches the existing hosted scalar primitive. Optional retained/A8
raster acceleration returns unsupported, selecting the
LVGL port's existing software fallbacks. It does not introduce another UI model.

## Opt-in native desktop profile

`BOOT_DESKTOP` adds retained client frames, software composition, window chrome,
focus and pointer capture, move/resize/maximize/minimize/cooperative close, a
session registry and manager-owned alpha panels. It uses the **same allocation-free
Rust window policy** as the Fuse-X desktop, linked with the existing no-std math
library. The C adapter owns native RAM and marshals checked C16 spans. No second
scheduler, process representation, LP64 ABI or GPU driver is introduced.

Each client draws into a private back buffer. `GX_PRESENT` commits its damaged
pixels without activating it or exposing another client's unfinished drawing.
The shell owns its regional panels; revoking its process-bound authority hides
those panels and restores compositor fallback controls. An app's close request
remains cooperative, including the editor's unsaved-document confirmation.
The initial native profile supports sixteen surfaces (including the panel),
1920×1200 client storage, 1x density, internal UTF-8 clipboard and the existing
physical keyboard/touchpad route. Native A8/retained-strip acceleration and
hardware wheel/gesture input remain separate work; apps use their software
rendering fallback. Fullscreen video, native audio and GPU acceleration are
not prerequisites for this profile.

Build applications with `--desktop` and a **current** LVGL adaptation. The builder
rejects old RGB-only configurations: alpha-capable panels require the adapted
ARGB8888 software renderer as well as the native GX alpha upload. Preparation
and outputs stay separate from personal disk images:

```sh
python3 tools/x86_64/graphics/build.py --desktop --lvgl-base build/lvgl/x64-base \
  --build build/x86_64-graphite/apps
make -C fuse-x -j4 BUILD="$PWD/build/x86_64-graphite/kernel-ir" \
  RCC="$PWD/target/release/rcc" FUSE_X_HEAP_PROFILE=bounded \
  FUSE_X_GX_MAX_HEIGHT=1200 freestanding-ir
python3 tools/x86_64/graphics/check.py --desktop
```

The app set includes FuseDE, Calculator, Files, Gallery, Data Viewer, Text Editor,
graphical terminal, screenshot and `desktop-probe`. The probe reads the public
session API; `desktop-probe --wait TITLE` waits for a **published** client frame,
not merely an application readiness message. The launcher supports an optional
absolute `FUSEDE_TERMINAL` path alongside `FUSEDE_APPLICATIONS`, so a staged test
set does not need to replace installed applications.

`desktop-build.py` creates an isolated EFI from that kernel IR and a hash-pinned
retained platform build. Supply `--build`, `--kernel-ir`, `--base` and the
independently verified `--base-efi-sha256`. The default `--platform fixture`
copies only font/vector objects and builds virtio support; `--platform hp`
retains the accepted private Wi-Fi/storage platform inputs. Do not mix profiles
in one directory. An HP EFI contains its embedded private network profile and
must not be published with a public fixture. Neither mode installs files or
opens a root disk. Current HP builds require those retained private platform
inputs; the script does not manufacture them in a clean checkout.

`desktop-root.mjs OUTPUT GRAPHITE_APPS UNIX_APPS` creates a new disposable 512 MiB
root from explicitly supplied executables and public examples, without a seed
image. It refuses an existing output. After preparing the fixture boot image
with `usb_image.boot_disk()` and copying QEMU's UEFI code/variable templates,
run `desktop-test.py --fixture DIRECTORY --accel kvm` on a Linux KVM host.
The test checks visible panel pixels, keyboard arithmetic, editor close/focus
recovery, plotting, terminal publication and shutdown page accounting. TCG is
available but the cold app starts can exceed its default test timeouts.
Never substitute the owner's root image for this fixture.

Host tests and a passing disposable VM are separate from HP acceptance. Follow
the [fresh EFI namespace procedure](../hardware/efi-updates.md), then stage the
applications through SFTP into a separate guest directory. Keep accepted EFI
entries and installed packages intact. Launch the staged shell explicitly;
this opt-in profile does not change native desktop startup policy.

For a slow SSH data path, the optional `stage-fetch.c` staging helper reuses
`ports/pkg`'s HTTP downloader and SHA-256 implementation. Build it together
with `ports/pkg/network.c`, `format.c` and `sha256.c` using the same static,
bounded x64 app linker. Bootstrap that small helper through pinned SFTP, then
pass `HTTP_URL DESTINATION EXPECTED_SHA256` through pinned SSH. It verifies a
temporary file before replacing the destination; a wrong digest cannot publish
an executable. Serve only the explicitly staged public payload directory on
the LAN and stop that temporary server after the transfer. This is a development
staging option, not a replacement for the signed package repository.

### Recorded HP bring-up and next checks

The [2026-10-22 trial](../hardware/receipts/2026-10-22/graphite-native-trial.json)
booted FuseDE at 1920×1200 and staged the full app set in `/root/graphite-test`.
The preceding disposable KVM test passed visible panel checks, arithmetic,
editor load/close and focus recovery, plotting, terminal publication and clean
shutdown accounting. Physical feedback was less successful: hover/click state
kept resetting, Calculator took about 15 seconds to appear, and interaction
felt laggy. There is no native 30 FPS or complete input acceptance claim.

Source now makes identical `GXP_CONFIGURE` calls idempotent: they no longer
advance the desktop revision or repaint, which previously caused the shell's
session-watch refresh to trigger itself repeatedly. Source also restores normal
focus when a modal panel is hidden. Host integration tests under fatal UBSan
pass these regressions. **These fixes are not in the running HP EFI `a889571f…`**.
The updated `desktop-probe --idle` and disposable VM idle check have not yet
been built/staged or run in a VM. Verify them before another hardware trial.

Treat that loop as a likely contributor to the hover resets/load, not a proven
explanation for every delay. After stable idle/input checks, profile app loading
and initialization separately from paint, composition and GOP presentation.
Then implement the retained/A8 fast path where measurements justify it. Keep
this work independent of ongoing process mappings and ring-3 changes.

## Clean-checkout LVGL setup

Run from the repository root. Install the [host toolchain prerequisites](../README.md#freestanding-kernel-boot):
Rust/Cargo, Python 3.12+, a working host C compiler/SDK, Clang with the x86_64
freestanding target and LLVM LLD (the Rust toolchain's bundled LLD is supported).
QEMU/UEFI firmware is needed for later boot tests, not these application builds.

Build the compiler, native object emitter and hosted runtime used by the
preparation build:

```sh
cargo build --release -p rcc-driver -p rcc-native -p rcc-native-runtime
```

Prepare the pinned LVGL adaptation and populate its object cache by building
only the hosted calculator, with a separate SDK cache:

```sh
RCC_NATIVE_MEMORY=checked python3 ports/lvgl/build.py --fetch --app lvcalc \
  --build build/lvgl/x64-base --sdk-build build/lvgl/x64-base-sdk
```

`--fetch` downloads the pinned LVGL 9.6.0 archive when absent and verifies its
SHA256 from [the provenance manifest](../../../ports/lvgl/PROVENANCE.json).
For offline preparation, supply that exact archive as
`build/gui-evaluation/lvgl-v9.6.0.tar.gz` and omit `--fetch`; checksum verification
still applies. Cargo dependencies must also be available locally for an offline
tool build. Fonts/icons are baked repository assets; no asset rasterizer or
guest font library is required.

This creates `build/lvgl/x64-base/src/lvgl.h`, `config/lv_conf.h`, `objects/`
and adaptation/license records. The SDK is generated from repository sources;
an existing Fuse-X build or personal root image is not a prerequisite.
The hosted `build/lvgl/x64-base/lvcalc.fxe` is a preparation artifact. Build the
freestanding RXE1 applications separately, selecting that adaptation explicitly:

```sh
RCC_NATIVE_MEMORY=checked python3 tools/x86_64/graphics/build.py \
  --lvgl-base build/lvgl/x64-base
python3 tools/x86_64/graphics/check.py
```

The isolated RXE1 outputs are `build/x86_64-graphics/apps/lvcalc.fxe` and
`screenshot.fxe`. The calculator requires the native runtime's 32 MiB image limit.
The x64 builder reads the selected adaptation/cache and compiles missing IR in
its own build directory. Its optional seed from `build/x86_64-tui-wave/sdk` is
not required. `check.py` runs focused host GX/screenshot checks and a native
compile; it does not boot the result or establish physical acceptance. These
commands write build outputs without installing applications, opening disk
images or connecting to the HP.

## Reusing an existing adaptation

For an existing workspace, select its prepared directory with `--lvgl-base`.
The historical default is `build/lvgl/system-dialogs/apps`; it is not provided
by a clean checkout. Once the adaptation exists, repeat the x64 build/check
commands above. Refresh the preparation build after changing the LVGL port;
a fresh binary needs its own QEMU and physical acceptance before EFI publication.

## Screenshots

On the native system:

```
screenshot /tmp/screen.ppm
screenshot --scale 4 /tmp/preview.ppm
```

Retrieve that file with SFTP. It is a standard binary P6 RGB PPM once exported.
The optional scale is an integer from 1 to 16; it keeps every Nth pixel/row for
a smaller development preview. Default capture keeps the full resolution.
One RGB octet occupies one C16 char cell on the guest filesystem; SFTP exports
its low eight bits. The native display service supplies RGB888 logical octets,
independent of GOP RGB/BGR channel order and framebuffer row padding.

The private development syscall is `call(0x7847, operation, row, data, capacity)`:
operation 0 returns width, 1 height, and 2 writes one complete RGB row and returns
its sample count. Errors return -1. The native runtime intercepts this request;
it does not add a shared C16 kernel syscall. Ordinary rendering never reads the
framebuffer. Explicit screenshots may read it and capture rows successively, so
moving content may change during a capture. They are diagnostic snapshots, not
atomic frame recordings.
