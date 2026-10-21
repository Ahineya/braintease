# SDL software foundation

SDL 1.2.15's software surfaces, events and a bounded PCM audio profile run inside
Fuse-X. This optional, statically linked library supports the original
[LGeneral engine](../lgeneral/README.md), a graphical surface demo and an audio
soundboard. Applications open GX windows directly; Nano-X and `startx` are not
required. The profile does not implement all SDL services.

## Build and try

```sh
python3 ports/sdl/build.py --fetch --target host
python3 ports/sdl/build.py --fetch
python3 ports/sdl/build.py --app demo
python3 ports/sdl/build.py --app audio-demo --build build/sdl-audio
```

Outputs are `build/sdl/sdlcheck-host`, `build/sdl/sdlcheck.fxe`, and
`build/sdl/sdl-demo.fxe`. The host executable is a byte-addressed upstream
rendering reference. The FXEs execute the C16 guest implementation. Build
generation pins the compiler, caches translation units and serializes builds
sharing an output directory. `CC` and `SDKROOT` select the host toolchain.

Explicitly stage the demo into a host-share directory:

```sh
python3 ports/sdl/build.py --app demo --stage fuse-x-shared
```

In Fuse-X:

```sh
/mnt/host/sdl-demo.fxe
```

Click a board cell or use arrows to move the selection. Space changes the unit
color; the wheel selects a color. Resize the window, then close it or press
Escape. `--depth 16`, `--depth 24`, and `--depth 32` exercise SDL's requested
video formats. The default 32-bit request uses an upstream shadow surface over
the driver's 24-bit BGR display surface. The demo blocks on input while idle.
Selection changes repaint only the previous/new cells and status indicators;
tint changes repaint unit cells. Resize/exposure still repaints the full window.
`--trace` reports events; `--frames N` is an explicit timed rendering workload.
`--size W H` chooses initial geometry, `--full-redraw` forces the original full
painting workload, and `--profile` separates rasterization from SDL publication.
`SDL_PROFILE=1` additionally reports GX upload/conversion/presentation timings.

No normal disk image, boot service, x64 driver or application inventory is
modified by building, testing or staging this port.

## Source and platform boundaries

`PROVENANCE.json` pins the original SDL archive and its SHA-256. `adapt.py`
extracts an unmodified tree under `build/sdl/pristine`, generates a separate
target tree and records changed file hashes in `adaptation-c16.json`.

* Upstream code retains surface ownership, clipping, locking, format/palette
  management, conversions, BMP loading, event queues/filtering, keyboard state
  and repeat, mouse state, and video shadow surfaces.
* `c16/pixels.c` handles the byte-addressing assumptions in low-level blits and
  fills. Generated C16 changes widen offsets, file counts, event masks and
  arithmetic, and implement endian stream reads/writes explicitly.
* `platform/gx.c` owns windows, event translation, synchronous uploads,
  presentation, monotonic time and waiting. It knows no game or LVGL objects.
* `platform/audio.c` owns callback lifetime, refill deadlines and mixer locking;
  `platform/pcm_audio.c` supplies the existing Unix PCM lease. `audio_adapt.py`
  and `c16/audio_formats.c` keep serialized samples independent of C packing.
* `mixer/mixer.c` is an optional, original implementation of the SDL_mixer 1.2
  PCM effects API subset. It is reusable by C applications, including LGeneral.
* `platform/support.c` defines the deliberately single-threaded service profile.
  `platform/host.c` and upstream's dummy video driver support reference tests.

SDL surfaces store **one logical octet per `Uint8` carrier**. On C16, each carrier
occupies a 16-bit cell: a 16-bit SDL pixel therefore occupies two cells, and a
24-bit pixel three. `BytesPerPixel` and `pitch` retain logical-octet meanings.
Do not reinterpret those buffers as `Uint16 *` or `Uint32 *`. Applications that
directly cast packed pixels still need representation adaptations. This differs
from GX's one-cell RGB565 upload format, so the backend converts explicitly.
The initial profile bounds ordinary surface widths below 16384 and supports
8/15/16/24/32-bit surfaces; packed 1/4-bit drawing is excluded. BMP's packed
input is expanded by the upstream decoder.

SDL RWops seek/read/write counts use explicit signed 32-bit carriers in the
C16 profile, rather than truncating at the guest's signed 16-bit `int` limit.
This is a source-port ABI choice: applications must use the generated headers.
It does not widen the filesystem or claim compatibility with existing SDL
binaries. File contents and endian values remain serialized octets. Memory
streams clamp seeks before pointer arithmetic, reject overflowing transfers
before changing storage/position, and return zero for nonpositive transfer
sizes/counts.

The software adapter matches SDL's distinct generic/565/888 alpha-rounding
paths. For reordered RGBA pixel-alpha blends it preserves destination alpha,
correcting an upstream overflow into that channel; unused non-channel bits can
also differ. The matrix test reports those differences separately from RGB
correctness. This is a bounded profile, not an SDL conformance claim.

Opaque fills seed one complete pixel, build the first row with nonoverlapping
bulk copies, and copy that row without touching padding or clipped neighbors.
Common opaque matching-RGB 24/32-bit conversions bypass generic channel
unpacking; identity copies use bulk memcpy only where nonaliasing is known.
Matching RGB24 surfaces also blend and apply color keys directly through
logical octets. The surface-alpha formula stays within unsigned 16-bit
arithmetic and retains the generic path's exact rounding and forward alias
behavior. Overlapping or externally aliased surfaces keep their copy semantics.
These paths use ordinary portable operations; they require no new GX request
or native framebuffer pointer.

## GX behavior and current limits

One application owns one SDL display window, matching SDL 1.2's video model.
The driver negotiates full-color XRGB8888 plus direct BGR upload when available.
This is the normal path on current Fuse-X. RGB565 remains a compatibility
format, not Fuse-X's default precision. A requested 16-bit SDL surface is
converted into the full-color GX window; it does not change the host display
format. Older backends use bounded RGB565 conversion strips. Uploads do not publish;
one presentation follows a completed rectangle batch. Buffers are borrowed only
for synchronous requests, and neither native pointers nor DMA addresses escape
into the application. Geometry and all source offsets are checked explicitly.

`SDL_GX_WINDOW_ZOOM` optionally selects native-window presentation zoom as an
integer percentage from `100` to `300`, read when the video driver starts.
For example, `SDL_GX_WINDOW_ZOOM=150 /mnt/host/sdl-demo.fxe` presents a 640×400
game raster in a 960×600-point window before monitor fitting. This changes
only native presentation: SDL dimensions, raster storage, upload rectangles,
input coordinates and backing density retain their original meanings. It is
independent of `GX_BACKING_SCALE` and requires no guest pixel resampling.
The host may lower the effective zoom to fit the current monitor while keeping
the requested percentage for later geometry or monitor changes. Without the
hint, application defaults are unchanged. On older GX drivers, SDL skips the
optional request and retains ordinary 1x presentation. Invalid hint values fail
video initialization before a window is opened. LGeneral selects `150` when no
hint is supplied; `SDL_GX_WINDOW_ZOOM=100` restores its original presentation
size without changing other applications.

The driver translates HID keys and paired committed text, pointer buttons,
fractional wheel motion, focus, exposure, resize and close into SDL events.
Focus loss/overflow releases held input. SDL 1.2's optional Unicode field is
UCS-2; this is not a modern Unicode text composition API. `SDL_WaitEvent` waits
for GX readiness or the next key-repeat deadline rather than polling every
10 ms. `SDL_Delay` uses a monotonic clock and real sleeping.

Threads, asynchronous timer callbacks, OpenGL, hardware surfaces,
fullscreen, YUV overlays and stretch are outside the initial profile. Available
entry points reject unsupported requests; absent entry points remain a link-time
capability boundary. RLE acceleration is bypassed in favor of software blitting.
GX currently exposes its native arrow without shape/visibility/warp/grab
requests. SDL custom/software cursor rendering is disabled; cursor-state APIs
do not yet change that native arrow. Gamma control is unavailable. These are
follow-up capabilities, not features supplied by an SDL software renderer.

## PCM audio

The audio profile implements `SDL_OpenAudio`, pause/status, close, audio locking,
PCM WAV loading, `SDL_MixAudio` and bounded `SDL_AudioCVT` conversion. The driver
uses the existing exclusive `/dev/pcm` lease; no kernel or host device protocol
is added. Opening starts paused. Unpause prefills the queue before starting the
PCM clock; close releases the lease and pending output. Device failures close
the stream and retain an SDL error. Video-only applications do not open audio.

Callback formats are unsigned/signed 8-bit or 16-bit PCM, either endian for
16-bit samples, mono/stereo, at 8–96 kHz. The callback receives logical octets;
the adapter decodes these into a separate signed-16 sample-cell buffer for PCM.
Never cast a C16 callback `Uint8 *` to `Sint16 *`. Partial writes and `EAGAIN`
retain the unsubmitted part of the same callback block. Queues are bounded by
four callback blocks and the device's two-second limit, with a smaller refill
target. Callback sizes are powers of two and their encoded length must fit the
signed 16-bit callback `int` (at most 32767 logical octets).

`SDL_AudioSpec.freq`, conversion rates and conversion lengths use explicit
32-bit fields in the generated headers. SDL libraries and their callers must
use those same headers. The profile preserves the original upstream octet
conversion filters where possible and implements packed sample access
explicitly. Conversion supports exact power-of-two rate ratios; other ratios
fail explicitly. The PCM WAV loader accepts mono/stereo 8/16-bit uncompressed
PCM, checks RIFF/chunk/frame bounds and handles odd ancillary chunk padding.
Compressed WAVs and Ogg/MP3 decoding are outside this profile. The optional
effects mixer below adds load-time resampling without changing `SDL_AudioCVT`.

**Callback scheduling is cooperative.** Callbacks execute on the application
execution thread at SDL event, presentation and delay boundaries. SDL waits
sleep until input or the next refill deadline. `SDL_LockAudio` prevents callback
reentry while the application changes shared mixer state. The existing host
speaker consumes copied samples asynchronously; it never invokes guest code.
Long SDL-free computation and arbitrary blocking libc calls can starve callback
production after queued audio is consumed. This profile does not provide SDL's
usual independent audio thread. A future general thread/continuation facility
must define shared memory, syscall state, locks and exit cleanup before that
guarantee can be added safely. Forked COW processes and signal callbacks are not
substitutes for that facility.

Try the soundboard in a normal Fuse-X session with speaker output enabled:

```sh
python3 ports/sdl/build.py --app audio-demo --build build/sdl-audio --stage fuse-x-shared
# Inside Fuse-X:
/mnt/host/sdl-audio-demo.fxe
```

Click the pads or press 1/2/3 for chime, pluck and zap; Space plays a melody,
S stops voices, M mutes, and Esc/Q exits. Four voices mix through a logical-octet
S16 callback. `--mono` selects mono and `--self-test` plays a short audio-only
test. The byte-addressed host target uses a clocked null sink for reference
tests, without opening the Mac sound device. Fuse-X uses its existing
`RCC_NATIVE_AUDIO` and `RCC_NATIVE_AUDIO_WAV` settings; no new global settings
are needed. See [PCM audio](../../docs/arch/audio.md) for those controls.

## Optional WAV effects mixer

[mixer/](mixer/README.md) provides an original, LGPL-2.1-or-later implementation
of a bounded SDL_mixer 1.2-compatible source API. It is not an upstream
SDL_mixer release or a complete replacement. Applications opt in through
`ports.sdl.build.library(..., mixer=True)`; ordinary SDL builds do not link it.
LGeneral enables this library for WAV sound effects.

The profile loads PCM WAVs and borrowed raw chunks, with eight default effect
channels and checked dynamic channel allocation. It supports reservations,
finite/infinite loops, channel/chunk volume, pause/resume, expiry, fades and
completion notifications. WAVs are converted to the obtained device format at
load time; a mixer-local nearest-neighbor resampler handles arbitrary rate
ratios. The callback then mixes the prepared chunks with no runtime resampling.
Music streams, compressed decoders and effects/postmix hooks remain outside the
subset. Callers must use this port's matching `SDL_mixer.h` and SDL headers.

The mixer inherits the cooperative callback scheduling described above. Long
work and blocking calls outside SDL service boundaries can still drain the
queue; linking the mixer does not create an audio thread. The
[threading investigation](../../docs/arch/audio.md#sdl-threading-investigation)
also considers an explicit mixer helper process as an audio-specific future
option.

Build and test the reusable mixer independently of LGeneral:

```sh
python3 ports/sdl/build.py --app mixer --build build/sdl-mixer
python3 ports/sdl/tests/mixer.py
```

The runner builds byte- and wide-carrier host fixtures and an actual C16
fixture, then tests the production PCM backend on disposable guest images.
`--host-only` runs just the host fixtures; `--deterministic-only` includes C16
callback checks while skipping production PCM captures.

## Deferred game targets

Audio is useful infrastructure, but an SDL 1.2 rendering path alone does not
establish that a game can build or run. The current RCC frontend compiles C99;
the two SDL strategy candidates audited here require C++. LinCity-NG and
Wesnoth are deferred at the user's request while C++ support and other
priorities are addressed.

| Candidate | Verified upstream requirements | Main additional port work |
| --- | --- | --- |
| Classic LinCity 1.12 | C with SVGALIB, X11 or Win32 front ends; not SDL | A new GX/SDL graphics and input adapter; this is the candidate that fits the current compiler language |
| LinCity-NG 2.0.0 | C++, SDL 1.2, mixer/Ogg, image/PNG, ttf, gfx, PhysicsFS, zlib and libxml2 | C++ compiler/runtime, dependency ports, synchronous replacement or support for its sound-loading thread, explicit software-only build without its unconditional OpenGL checks |
| Wesnoth 1.0.2 | C++, SDL 1.2, image/PNG, mixer/Vorbis, SDL_net and FreeType; bundled modified SDL_ttf | C++ compiler/runtime, fonts/codecs, synchronization and network worker support, a checked software-window profile |

Primary sources: [classic LinCity README](https://lincity.sourceforge.net/readme.html),
[NG 2.0.0 README](https://github.com/lincity-ng/lincity-ng/blob/lincity-ng-2.0.0/README),
[NG build checks](https://github.com/lincity-ng/lincity-ng/blob/lincity-ng-2.0.0/configure.ac),
[NG sound loader](https://github.com/lincity-ng/lincity-ng/blob/lincity-ng-2.0.0/src/lincity-ng/Sound.cpp),
[Wesnoth 1.0.2 INSTALL](https://github.com/wesnoth/wesnoth/blob/1.0.2/INSTALL),
[Wesnoth build checks](https://github.com/wesnoth/wesnoth/blob/1.0.2/configure.ac),
[Wesnoth thread wrapper](https://github.com/wesnoth/wesnoth/blob/1.0.2/src/thread.cpp).
NG 2.0.0 is pinned by the audited commit
`521600339d0e2df710a924f291444e7ed0dcb8dd`. No build or gameplay acceptance for
either strategy game is claimed.

The reusable WAV mixer and LGeneral's sound integration do not resolve the
remaining codec, threading or C++ dependencies of those games. SDL_image can
reuse the existing image-decoding library; SDL_ttf needs a font engine adapter.
Fuse-X is the first target, with portable service boundaries retained for
future Riplix x64 adapters.

## Riplix x64

Follow the [x64 master plan](../../docs/x64-master-plan.md),
[native application plan](../../docs/arch/native-x64.md), and
[GX contract](../../docs/graphics/architecture.md#gx-version-1).

| Build | Rendering and service boundary |
| --- | --- |
| Fuse-X C16, implemented | Isolated logical-octet adaptations; hosted GX, matching C16 SDK. |
| Freestanding x64 C16, planned | Reuse the C16 renderer; link the bounded freestanding SDK and test against the HP GX/input backend when available. Hosted FXEs are not HP executables. |
| Future x64 LP64, planned | Ordinary upstream byte-addressed renderer, native libc and explicit native GX marshalling. Current C16 structs/pointers are not native message layouts. |

Keep separate SDK/cache/package identities. Graphics format negotiation is
independent of ABI; full-color surfaces remain preferred where supported.
Software graphics do not require a GPU, threads or SMP. Responsive multi-app
behavior still depends on bounded operations and the OS execution/preemption
contracts. Do not retain borrowed C16 views across yields. PCM and SDL audio
need their own hardware and callback/thread acceptance.

The [LGeneral port](../lgeneral/README.md) now runs its original interface and
the free K.u.k. content on Fuse-X. Its separate game tests cover all 22 scenarios,
movement, combat, AI turns, campaign progression and portable save/reload.
SDL surface tests alone do not establish game correctness or HP graphics support.

## Verification

```sh
python3 ports/sdl/tests/test_pixel_matrix.py
python3 ports/sdl/tests/test_gx_backend.py
python3 ports/sdl/tests/test_audio_formats.py --guest
python3 ports/sdl/tests/test_audio_backend.py
python3 ports/sdl/build.py --app audio --build build/sdl-audio
python3 ports/sdl/tests/audio.py
python3 ports/sdl/build.py --app mixer --build build/sdl-mixer
python3 ports/sdl/tests/mixer.py
python3 ports/sdl/tests/test_demo_damage.py
python3 ports/sdl/tests/run.py
python3 ports/sdl/build.py --app bulk --target host
python3 ports/sdl/build.py --app bulk
python3 ports/sdl/tests/test_pixel_bulk.py
python3 ports/sdl/tests/test_rgb24.py --guest
```

Audio verification includes 432 conversions, 30 mixing cases, WAV parsing and
malformed/overflow checks compared with scalar expectations, byte-addressed
upstream and actual C16 execution. The lifecycle fixture tests the production
core against partial writes, `EAGAIN`, callback reentry, deferred close and
device failures. Twelve private guest cases cover exact mono/stereo output in
all six PCM formats, 44.1/48 kHz operation, shared callback state, pause/lock,
lease cleanup and audio progress during `SDL_WaitEvent`. Captures use the
existing muted WAV sink, so they establish sample correctness rather than a
physical speaker latency guarantee. Sound Lab has separate real-window input,
idle playback, mute/stop and clean-exit checks. No ordinary disk image is used.

The focused mixer suite passed 3,481 checks on each host byte/wide-carrier
profile and on actual C16, covering exact samples, dynamic channels, WAV
conversion/resampling, loops, gain/clipping, fades, expiry and reentrant
completion cleanup. Separate captures through production SDL audio and
`/dev/pcm` matched the expected overlap, finite-loop, volume and pause samples;
control/lifecycle checks also passed. These muted captures establish sample
correctness, not a speaker latency guarantee. LGeneral's separate GUI/audio
checks also passed: original GUI clicks matched their stereo samples, scenario
sounds loaded, mute persisted, and close/reopen cleaned up correctly.

The host matrix covers 1012 supported format/alpha/key combinations against
upstream blitters. The actual guest suite compares 82 surface cases exactly
with the host reference, including clipping, pitch padding, palettes, color
keys, alpha, overlap and BMP memory decoding beyond 32 KiB. It saves/reopens
a 49,206-byte BMP through real file I/O, checks 16/32/64-bit endian streams,
and rejects overflowing requests without changing stream/storage state. It also checks
event queues/filter semantics, monotonic sleeping, unsupported services, real
GX windows at requested 16/24/32-bit depths, pointer/keyboard/wheel input,
resize, idle publication and clean close/reopen.

The isolated GX backend test injects allocation/configuration/clock failures
and interrupted waits, checks direct/full-color and RGB565 fallback uploads,
and verifies input translation and cleanup. Test results/logs/frame captures
live under `build/sdl/`. Guest tests use disposable offline images and perform
filesystem checks on shutdown. Host tests alone do not establish guest or
physical hardware behavior.
The RGB24 fixture compares 234 complete storage results with upstream SDL,
the host adapter and the actual C16 guest, including all channel orders, key
values, alpha extremes, padded/clipped surfaces, wide rows and forward aliases.
The bulk-operation fixture additionally checks 36 cases of wide rows, clipped
fills, padding, conversion and overlapping/external storage on both the host
reference and actual C16 guest. The demo damage harness checks 507 full/partial
pixel comparisons and 441 no-op updates against the pinned host renderer.

## Interaction performance

The initial demo painted and converted its entire window for every selection.
Measured on 21 October 2026, the same default 32-bit demo and unchanged Fuse-X
kernel produced these median input-to-completed-frame times (three actions per
geometry):

| Window | Original | Bulk pixel paths and damaged painting |
| --- | ---: | ---: |
| 320 x 240 | 54.4 ms | 3.5 ms |
| 640 x 400 | 193.6 ms | 6.1 ms |
| 1024 x 800 | 703.2 ms | 8.2 ms |
| 1024 x 1024 | 769.8 ms | 8.3 ms |

Every initial and post-selection RGB snapshot matches the original. These are
headless guest measurements including input transport and frame dumps; they do
not include visible compositor latency or establish frame-rate guarantees.
Forced full redraws also improved: 1024 x 800 dropped to 84.3 ms, isolating the
library gain from the demo's damage policy. Full-frame 32-to-24-bit conversion
remains more expensive than small updates. Full color stays enabled throughout.

Reproduce comparisons against a saved old executable:

```sh
python3 ports/sdl/tests/bench.py --demo OLD.fxe --output build/sdl-performance/before
python3 ports/sdl/tests/bench.py --output build/sdl-performance/after \
  --compare build/sdl-performance/before/result.json
python3 ports/sdl/tests/bench.py --output build/sdl-performance/full \
  --full-redraw --profile --gx-profile
```

The benchmark uses disposable offline images, records app/kernel/machine
identities and checks exact frame pixels when `--compare` is supplied. Receipts
for the measurements above are in `build/sdl-performance/`.

## Licensing

SDL and adaptations derived from it retain LGPL-2.1-or-later notices. The
original `COPYING` is retained under `build/sdl/licenses/` and accompanies
explicit staging. Keep the pinned source, adaptation scripts and relinkable
application inputs available when distributing statically linked applications.
Applications and game assets retain their own licenses. SDL 1.2 is a legacy
compatibility target; it does not replace GX, LVGL, or a future modern SDL port.
