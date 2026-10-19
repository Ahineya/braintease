# NetSurf: C16 port

The port builds NetSurf 3.11's actual HTML/CSS browser engine for both
Riplix/RVM and native Fuse-X. `netsurf` now has a Nano-X graphical frontend:
HTTP/HTTPS, local HTML/text files and `data:` pages, CSS colours and tables, links,
editable text fields, history, address entry, viewport scrolling and resizing.
It uses the same guest renderer on both systems.

This is a **bounded graphical web browser** with verified guest TLS, external
stylesheets, redirects, GET forms and bounded PNG/GIF/JPEG/BMP images. JavaScript, downloads,
POST/uploads, multiple windows/tabs and downloadable web fonts are not enabled.
`netsurfcheck` and `netsurfcorecheck`
are separate headless acceptance programs. See
[the browser architecture and sequence](../../docs/networking/gui-browser.md).

## Source provenance

`AUDIT.json` pins the official full source archive and its SHA-256. The 2,136
files in `upstream/` are unmodified sources from that archive, with
licenses, build inputs and generators. Upstream test datasets, examples and
documentation are omitted. The six imported support libraries have MIT
licenses. The browser core is GPL-2.0; its embedded Samba talloc implementation
retains its LGPL-2.0-or-later notice. Component licenses and source-file notices
are retained in the import.

`upstream-manifest.json` records every imported file's hash. `adapt.py` verifies
all hashes before generating working sources under the build directory.
Changes use checked replacements, and `source/adaptations.json` records the
changed source files. Shared source preparation is locked; outputs are replaced
atomically so native and RVM compilation can use the same working tree.

To reproduce the import into an empty `upstream/` directory:

```sh
python3 ports/netsurf/vendor.py /path/to/netsurf-all-3.11.tar.gz
```

The importer refuses a different checksum or an existing destination. To add
a component from the same archive, first extend `AUDIT.json`, then pass
`--extend`. This verifies all existing pinned files and their archive hashes,
rejects overwrites and omitted components, and only adds new files. Normal
builds use the checked-in import and require no network access.

## Build and test

Prerequisites are the release RCC/assembler/linker, Python 3, Perl, gperf, a
host C compiler for the upstream property generator, and Node.js for private
test disk creation. On this macOS checkout use the working SDK when linking:

```sh
export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
python3 ports/netsurf/app.py --target native -o build/netsurf/netsurfcheck.fxe
python3 ports/netsurf/app.py --target ripple -o build/netsurf/netsurfcheck.rxe
python3 ports/netsurf/tests/host.py
python3 ports/netsurf/tests/guests.py
```

To build and test the browser-core milestone:

```sh
python3 ports/netsurf/tests/core_host.py
python3 ports/netsurf/app.py --core --source ports/netsurf/tests/core.c --target native -o build/netsurf/netsurfcorecheck.fxe
python3 ports/netsurf/app.py --core --source ports/netsurf/tests/core.c --target ripple -o build/netsurf/netsurfcorecheck.rxe
python3 ports/netsurf/tests/guests.py --core
```

`--core` adds 127 browser/utility/backing-store translation units (plus the shared Unicode
engine on RVM) and links the guest's
floating-point SDK. Its private SDK directories have a `-float` suffix so the
dependency-only SDK profile remains independent. `core.py` can compile the
core to IR alone with `--target native` or `--target ripple`.

The core acceptance frontend supplies a bounded callback queue, UTF-8-aware
fixed-cell font metrics and the upstream default/quirks/internal stylesheets.
It loads an HTML/CSS data URL, checks title/text/32-bit colour output and
wrapping after a viewport resize, then loads a 10 KiB local file with a table
through multiple 4 KiB fetch buffers. Separate checks cover numeric conversion,
base64 high octets/empty input, plain message tables and charset conversion.
Images and history thumbnails return unavailable; no replacement layout engine
or host web renderer is involved.

The conventional host passes **107 core checks, zero failures**. Native Fuse-X
and real Riplix/RVM JIT each pass **108 checks, zero failures**, including their
extra heap-measurement check. Both programs return zero and both disposable
filesystems pass consistency checks after shutdown. All three produce the
same layout: the last text baseline moves from y=149 to y=212 when narrowing
the viewport from 480 to 180 pixels.

In the earlier HTML-only acceptance run, Fuse-X reserved 217,772 additional heap cells and
RVM reserved 217,785: about 425 KiB of physical storage each. This is program
break growth, including allocator slack and the talloc stress cases; it is
not peak live page memory. RVM's acceptance package has about 5.23 million
instructions and about 313,500 global cells, occupying about 40 MiB on disk. Code
storage, global storage and heap growth are different measurements. The
acceptance frontend has no viewport framebuffer allocation.

Core adaptations include standard C99 logging macros; explicit 32-bit plot
shifts; bounded framesets (16 rows/columns maximum); a 1 Mi-cell minimum cache
target with bounded persistent backing storage; monotonic timing; and plain resource tables
that reject gzip and overlength records explicitly. ParserUtils replaces iconv
for supported charsets, with a 65,536-octet conversion ceiling and explicit
truncation checks. Resource-path lookup checks readability and length; it does
not claim to canonicalise paths. Option-value alternatives use separate storage
to avoid the recorded nested-union initializer defect. The same separation is
used by page-info entries, exercised by creating, measuring and redrawing the
local-document information view. Talloc's magic/flag word is explicitly 32-bit;
its ownership, references, recursive free and bank-spanning resize are tested.

NetSurf's `strndup` fallback uses the private name `ns_strndup`. The guest SDK's
historical declaration takes a 16-bit `int` length, conflicting with the
fallback's 32-bit `size_t`. On RVM this mismatched call ABI corrupted copied
URLs and HTTP header names, causing stylesheet rejection and repeated error-page
loads. The private declaration and implementation consistently use `size_t`;
substring and zero-length checks cover that bridge. Resource/file ETags and log
timestamps use supported 32-bit `long` formatting instead of the SDK's absent
`long long` printf conversion. This does not add general 64-bit printf support.

The `about:nscolours.css` generator also needs an explicit unsigned `long`
colour argument and `%06lx`: upstream's `%06x` consumes only one C16 cell,
leaving half the colour in front of the following string pointer. This caused
native pointer faults ending in `00e0` when an unreachable or rejected Wiby
destination opened NetSurf's own error page. The checked adaptation preserves
the original upstream sources and the existing ABI. Network acceptance fetches
the stylesheet directly, renders actual fetch/certificate error pages, and
alternates between errors and successful loads in one browser. The native GUI
check also verifies its rendered background and a clean application close.
The fix passes the full 18-case native network suite and four focused RVM JIT
checks (stylesheet, rendered fetch error, rendered certificate error and verified
HTTPS), plus the native graphical regression. Each guest filesystem is clean
after shutdown. Native runtime faults still terminate the hosted session;
per-application fault handling is separate platform work.

The application driver builds a private userspace SDK by default. Pass `--sdk`
to reuse another SDK; RVM's `--skip-sdk` skips rebuilding both the browser and
worker SDKs, which must already be populated. Libraries
are cached separately per backend with source/header/compiler fingerprints.
Each failed compilation retains its diagnostic log next to the IR file.

The dependency host runner (`tests/host.py`) also accepts
`--sanitize --build build/netsurf-host-asan`
for AddressSanitizer and `--timeout` for execution limits. In this sandbox,
even a trivial AddressSanitizer control stalled before `main`, so no sanitizer
pass is claimed for the 2026-10-18 validation.

The guest runner builds the checks and a minimal shell/echo set in the private
SDKs, then creates disposable root images, boots the real kernels, verifies
the result and exit status, shuts down, and checks filesystem consistency.
It requires the existing Riplix kernel/BIOS/init in `build/fuzix/` and the
Fuse-X kernel in `fuse-x/build/`; override with `--fuzix-build` and
`--native-build`. `--os fuzix` or `--os fuse-x` selects one system.
`--skip-build` reuses the test executables, with optional `--fuzix-shell` /
`--native-shell` paths. No persistent user disk is opened.

## Graphical browser

Build both targets (the GUI option enables the core, floating SDK and guest
HTTP worker; it also builds `htget.fxe` / `htget.rxe` beside the browser):

```sh
python3 ports/netsurf/app.py --gui --target native -o build/netsurf/netsurf.fxe
python3 ports/netsurf/app.py --gui --target ripple -o build/netsurf/netsurf.rxe
python3 ports/netsurf/tests/gui.py
```

The graphical test requires the existing Nano-X servers, shells, echo and kill
in `build/fuzix/` and `fuse-x/build/`, plus native `nxhostcheck`. Override their
locations with `--fuzix-build`, `--native-build`, `--fuzix-shell` and
`--native-shell`. Use a freshly built Nano-X server: older copies predate
Unicode text events and will fail that input check. To refresh the usual RVM
server independently, run:

```sh
make -C ports/fuzix BUILD="$PWD/build/fuzix" "$PWD/build/fuzix/nano-X.rxe"
python3 fuse-x/graphics.py --only nano-X
```

The runner creates disposable disks, invokes the actual installer,
boots the real kernels, checks rendered pixels and shuts down before checking
filesystem consistency. Native checks exercise external window resize/close;
the opt-in Rust RVM test injects real HID events for scrolling, local-link
navigation, history, an editable HTML field, address entry and quitting. It
also checks that an idle browser does not keep presenting frames. Captures and
logs go to `build/netsurf/gui-tests/`.

Use an image updated through the normal system build so it already contains
the current Nano-X graphics stack. The browser installer adds the browser and
examples. An optional final argument installs an explicitly supplied current
Nano-X server; the kernel is not updated.
To try it on that **shut-down** image, make an installed copy:

```sh
node ports/netsurf/install.mjs native fuse-x/build/ROOTFS.IMG /tmp/netsurf-native.img
node ports/netsurf/install.mjs ripple build/fuzix/fuzix.img /tmp/netsurf-rvm.img
```

Replace the input paths with your actual root image / RVM disk image. The
installer validates the filesystem, installs `/bin/netsurf`, `/bin/htget`,
the default trust bundle at `/etc/ssl/cert.pem`, the image gallery and examples in
`/usr/share/netsurf/`, and writes a new output image. An existing trust file is
preserved. It refuses an existing
output, wrong executable format, dirty filesystem or insufficient space;
it does not resize the source image. The RVM package currently occupies about
42 MiB of executable octets, and the independent HTTP/TLS worker another
16 MiB. Each executable octet consumes a C16 filesystem cell, so allow over
116 MiB of physical root space for these two files, plus filesystem overhead.
The browser is also in the normal Riplix and Fuse-X application inventories.
Normal system builds maintain `/bin/netsurf`, its showcase/gallery and the
ordinary `/bin/htget` worker. Shared-folder experimental executables are not
needed. Incremental builds reuse the shared NetSurf object cache.

To update an existing shut-down image directly, use `--in-place` (omit the
new-output argument). It checks for open image files with `lsof`, validates
the filesystem before and after installation, backs up the old disk, and
publishes the update atomically. User files and an existing trust bundle are
preserved. It refuses a mounted/open image:

```sh
node ports/netsurf/install.mjs --in-place native fuse-x/build/ROOTFS.IMG
node ports/netsurf/install.mjs --in-place ripple build/fuzix/fuzix.img
node ports/netsurf/tests/install.mjs
```

Boot the copy using `scripts/run-fuse-x.sh --run-only --disk /tmp/netsurf-native.img`
or `scripts/run-riplix.sh --run-only --visual --disk /tmp/netsurf-rvm.img`.
Start Nano-X/FuseDE (`startx`), then launch `netsurf` from a guest shell. It opens
`http://wiby.me/` by default. Pass a URL or local path to open another page;
`netsurf /usr/share/netsurf/index.html` opens the installed local showcase,
including the image gallery and Wiby's random-page link.
Normal guest networking, DNS configuration and a current guest clock
must be available, as for `htget`/Lynx. `netsurf --help` lists the controls:

- Ctrl+L / Ctrl+O focuses and selects the address; type a hostname, absolute path or
  `http:`/`https:`/`file:`/`data:` URL and press Enter. Bare hostnames use HTTPS
  (also on the command line); enter `http://` explicitly when needed. Escape cancels. Editing currently appends
  at the end; full cursor editing and a file chooser are future UI work.
- Options in the toolbar (or Alt+O) opens a dropdown. Images toggles both
  inline and CSS background image fetching, then reloads the page. Up/Down and
  Enter also operate the menu; Escape or a click outside dismisses it.
  JavaScript is shown disabled/unavailable: no script engine is linked.
- Back/Next toolbar buttons or Alt+Left/Right navigate history. F5 reloads;
  Home in the toolbar opens `http://wiby.me/`; Random opens
  `http://wiby.me/surprise/` and follows its selected destination.
- Arrow keys, PageUp/PageDown, Home/End, the mouse wheel and scrollbar track
  scroll the document when an HTML field does not consume those keys.
- Click links and text inputs. Typing, deletion, selection and Tab are passed
  to NetSurf's input engine. Ctrl+C/X/V/A operate address selections, page
  selections and editable fields; in Fuse-X use the native Edit menu or
  Command+C/X/V/A with the shared macOS text clipboard. Address pastes must be
  a single line and fit the 1,024-cell editor; rejected pastes do not truncate
  the destination. The VM server has a local clipboard without host sharing.
- Ctrl+Q or the window's close control exits.
- Escape outside the address editor stops the current load.

The frontend has one top-level window and no client framebuffer. It clips
core redraws to the document area, coalesces dirty rectangles and publishes a
batch of Nano-X primitives. One persistent server clipping region avoids
allocation/query round trips on each clip change. Caret updates invalidate
small rectangles. Input is processed in batches of up to 16 pending events
before painting, so a wheel gesture does not require one repaint per pulse.
Nano-X keeps counted wheel runs in its bounded queue instead of disconnecting
a busy reader after 64 pulse/release events. Scrolls redraw the visible viewport; retained-pixel scroll
copies are not yet implemented. CSS backgrounds are enabled in redraws while
image fetching is enabled.

Validated on 2026-10-18: the Fuse-X graphical pixel/resize/close test and the
real Riplix/RVM HID/navigation/form/Unicode-input/idle test pass, with zero exit statuses and
clean post-shutdown filesystems. This is focused local-page acceptance, not a
browser conformance or performance benchmark. The GUI executable contains
about 5.4 million RVM instructions and occupies about 42 MiB of
executable octets; the native executable occupies about 22 MiB. These
sizes exclude guest runtime heap, stack and Nano-X backing storage. Cold RVM
startup loads the browser and worker packages and can take tens of seconds; this work
does not establish an interactive browser latency budget.

The GUI uses **Ripple Web**, a redistributable bitmap subset of Liberation
2.1.5, with sans, serif and monospace families and real regular/bold/italic/
bold-italic faces. CSS font sizes map from points to 96-DPI pixels, bounded to
8–64 pixels. Hinting and rasterization happen offline at 16 and 32 pixels;
other sizes use integer bilinear resampling. Painting, wrapping, selection and
hit testing share the same proportional advances and grapheme boundaries.
The toolbar uses a compact sans face; address scrolling and its caret use
pixel measurements rather than character counts.

Four-bit antialiased coverage is alpha blended over the actual destination,
including dark backgrounds and images. Three bounded client atlases share the
ordinary image-resource LRU with photographs, with at most nine glyph
rectangles per drawing request. Repaints reuse uploaded atlases; colours are
applied by the GC without rebuilding the font. There are no host font calls,
new MMIO commands, or guest TrueType dependencies.

The installed `/usr/share/netsurf/web.nfa` contains 12 faces and two strikes
for Latin, Greek, Cyrillic, combining marks and common punctuation. It takes
about 3 MiB of file octets (twice that in physical C16 storage), loaded once
per browser. The loader validates its version, record extents and every
compressed glyph before use; missing or corrupt data gives an explicit
reinstall diagnostic. Unicode outside the subset retains the shared bitmap
fallback. Supported canonical combining pairs are composed for rendering;
complex shaping, bidi, a full CJK repertoire, colour emoji, kerning, small caps
and downloadable fonts remain future work. See [font provenance](fonts/README.md).

The Nano-X input bridge decodes the server's UTF-8 octet stream before passing
Unicode scalars to NetSurf. Polygon fills use bounded scanlines; arcs, paths,
and SVG remain unsupported. Large pages remain subject to the
existing C16 coordinate/memory limits.

Plain text without an explicit charset now defaults to UTF-8. An explicit
HTTP/data-URL charset still selects its declared codec. Font-size multiplication
is widened before scaling: upstream's `128 * 1024` overflowed C16 `int` and
reduced plain-text line height to zero. Word wrapping preserves whole graphemes
and every source octet, including unbroken words, very narrow viewports,
CRLF/LFCR and tabs. Numeric octets represent U+FFFD in the NUL replacement path
to avoid the separately recorded high-octet string-literal compiler defect.

The core checks cover these cases on the host and both guest systems. The
Fuse-X pixel test opens a real host-mounted UTF-8 file, compares Cyrillic
glyphs against the antialiased strike and checks reflow at 340, 600 and 1,200 pixels:

```sh
python3 ports/netsurf/tests/fonts.py
python3 ports/netsurf/tests/fonts_gui.py
python3 ports/netsurf/tests/text_gui.py
# Optional local reproduction; the file is copied to a private test share.
python3 ports/netsurf/tests/text_gui.py --article fuse-x-shared/article.txt
```

Captures and logs are saved under `build/netsurf/text-gui/`. The installer also
includes the Unicode data/font notices under `/usr/share/licenses/unicode/`.

## Loading policy, caching and progress

Embedded images are **off by default**, including CSS backgrounds. Use
`netsurf --images URL` or Options > Images to enable them for this browser
session. `--no-images` explicitly disables them; the last flag wins.
`--no-javascript` explicitly selects the current safe default. `--javascript`
reports that this build has no JavaScript engine, rather than pretending to
execute scripts. Image codec support remains linked in either mode. The
frontend has no favicon display, so it no longer starts unused favicon fetches.

The existing upstream low/high-level **memory cache** handles response
freshness and conditional requests. Its minimum target is now 1,048,576 C
cells, four times the previous target; NetSurf assigns 75% to its source
cache and 25% to its built-in image-cache target. Our image handler additionally
imposes its exact aggregate decoded-pixel quota. These are cleanup targets,
not a reservation or a hard bound on document memory. Fresh cacheable URLs can
avoid a worker/network fetch; expired responses with ETags can use conditional
requests and 304 bodies. Reload deliberately revalidates and F5 also invalidates
subresources. Servers can require revalidation. RAM objects are discarded on
exit; the graphical browser now enables a **persistent disk backing store**.
There is no separate transport-level response cache layered over NetSurf.

The default directory is `$HOME/.netsurf-cache` (or `/.netsurf-cache` without
HOME). It is created privately, on the guest filesystem, so saved resources
survive browser exits and orderly OS shutdown/reboot. `netsurf --cache-dir DIR`
selects another directory; its parent must exist. `--no-disk-cache` uses only
RAM. `--clear-cache` clears disk storage at startup, and **Options > Clear disk
cache** clears it while browsing. This leaves the current page and RAM cache
usable. The Options menu reports whether disk storage is available. If another
browser owns that directory, or it cannot be created, this instance reports a
diagnostic and continues with RAM caching.

Storage is bounded to 128 resources and 8,388,608 C cells (16 MiB physical
payload on the C16 filesystem), plus filesystem allocation overhead. Metadata
counts against that limit; the existing 256 Ki-octet per-response limit remains.
Least-recently-used unpinned slots are evicted in the running browser; restart
ordering uses file modification times. Active source buffers cannot be evicted
or replaced until released. Corrupt/truncated entries become cache misses.
Each versioned, checksummed record contains both headers/URL metadata and the
source body. A temporary file and same-directory rename publish the pair;
incomplete temporary files are removed on the next open. This is disposable
cache storage, not a journal or a guarantee of survival after power loss.
Normal guest shutdown supplies the existing filesystem/host durability flush.

Fresh HTTP/HTTPS objects can skip DNS, the worker and TLS completely. Stale
ETag/Last-Modified objects remain on disk for conditional requests and 304 body
reuse. `no-cache` requires revalidation; `no-store` is never persisted and
invalidates an older disk copy. POST, error responses, empty bodies, session
cookies, credential-bearing URLs and responses with unsupported `Vary` fields are omitted
from disk caching. `Vary: Accept-Encoding` is supported because every request
uses identity encoding. This profile does not yet maintain other request-header variant
keys or persistent authenticated sessions. Failed/cancelled fetches are not
saved, and cached HTTPS content does not permit certificate bypass on network
requests. Saving begins after a short idle interval and is attempted on normal
browser exit.

This should principally improve repeat visits and reuse of stylesheets/images.
Cached image sources still need decoding after a browser restart; decoded
pixels and layout are RAM-only. First visits, uncacheable sites and full reloads
still require network work. No whole-browser speedup factor is claimed.

Persistent-cache acceptance, using disposable real guest disks:

```sh
python3 ports/netsurf/app.py --web --source ports/netsurf/tests/disk.c --target native -o build/netsurf/diskcachecheck.fxe
python3 ports/netsurf/app.py --web --source ports/netsurf/tests/disk.c --target ripple -o build/netsurf/diskcachecheck.rxe
python3 ports/netsurf/tests/disk.py --target native
python3 ports/netsurf/tests/disk.py --target ripple
```

The fixture checks fresh HTTP and verified HTTPS across process exits and a
full kernel restart, ETag/304 and no-cache, no-store invalidation, Vary/cookie
exclusion, forced fetch, clearing, and rejected wrong-host certificates. Storage
checks exercise a 70,000-cell body, quota/slot eviction, active-buffer lifetime,
corruption, truncation, write failure, directory locking and unavailable paths.

Validated on 2026-10-18 on native Fuse-X and Riplix/RVM JIT, with clean
filesystems after both shutdowns. The large HTTP fixture crosses a C16 bank;
Age/max-age and remaining freshness also exceed signed 16-bit range. The
cache uses the existing NetSurf source cache, not a download shortcut around
its HTTP policy. Graphical render/resize or HID/history/idle regressions pass
on both targets. Cold RVM download checks allow 180 seconds, including worker loading and JIT
startup. The browser transport retains its 240-second request deadline.

The expanded guest SDK now supplies public `ceil`; the port keeps only its
private `ns_ceilf` conversion through `floor`, avoiding duplicate libm exports.

During network activity, the status bar reports connection/response-header
waiting, then the received entity octets and Content-Length percentage when
available. Unknown lengths show received octets and a moving activity bar.
The percentage belongs to the current response, not an invented total for all
page resources. Progress updates are limited to eight per second and do not
invalidate the document. Normal NetSurf status/error messages return when the
transfer finishes. Two independent HTTP workers can run concurrently, so a
slow image does not monopolize the entire request queue. The status bar shows
one active response at a time. Retained images do not speed up first-time TLS,
decode or HTML layout.

```sh
python3 ports/netsurf/app.py --web --source ports/netsurf/tests/cache.c --target native -o build/netsurf/cachecheck.fxe
python3 ports/netsurf/tests/cache.py --target native
python3 ports/netsurf/tests/progress.py
python3 libraries/tests/test_image_nano.py
```

The native cache fixture verifies one network request for two fresh-cache
retrievals, two requests with ETag/304 body reuse for an expired response, and
two complete requests for `no-store`. Progress acceptance checks both the
reported transfer state and actual green status-bar pixels. The independent
image adapter oracle compares scaling, alpha, CSS phase and clipping pixels;
its eight-repaint case falls from 24 tile uploads to zero after two initial
source-upload chunks. This is an operation count, not a whole-browser latency
benchmark.

Validated on 2026-10-18: both guest kernels pass the 29-check cache probe and
exact-pixel image fixtures. The RVM/JIT image run also checks the mouse-opened
Options dropdown, Escape, keyboard image enabling, scrolling and four direct
image reloads. Native and RVM navigation/resize or input/idle regressions pass;
native known/unknown-length progress is checked in actual pixels. All use
private disks and finish with clean filesystems.

Local-file validation uses the same supported `long` formatter. The file
fetcher records whether `If-None-Match` was supplied separately from its value:
mtime zero is valid for installed files. This lets unchanged local resources
reuse their cache entry instead of fetching/decoding again and consuming the
image quota twice. `tests/filecache.py` checks zero/current timestamps,
unchanged-source sharing and replacement while the old source remains pinned.
The graphical check loads four 128x128 images at the exact pixel-budget limit
and requires one decode each, with no quota refusals:

```sh
python3 ports/netsurf/app.py --core --source ports/netsurf/tests/filecache.c --target native -o build/netsurf/filecachecheck.fxe
python3 ports/netsurf/tests/filecache.py --target native
python3 ports/netsurf/tests/bench_fetch.py --local --repeat 1 --output build/netsurf/local-image-check.json
```

## Images

The graphical build (`--gui`) links the shared, browser-independent decoder in
[`libraries/image`](../../libraries/image/image.h), plus a thin content-handler
adapter. PNG, GIF's first frame, baseline JPEG and uncompressed BMP work in
HTML images, CSS backgrounds and direct image URLs. GIF animation, progressive
JPEG, WebP, SVG and ICO are not included. See the [library guide](../../libraries/README.md#images)
for exact format, color and resource limits, C use and the owned RFL `lib/image`
wrapper. `--images` links the content handlers into other core frontends; those
frontends must register them and implement their bitmap callbacks.

NetSurf keeps at most 65,536 decoded pixels across its entire content cache,
with a 512-pixel dimension limit and 256 Ki-octet input limit per image. Its
handler limits temporary decode work to 393,216 C cells. Decoded RGBA is used
directly as the frontend bitmap, without a second RGBA allocation. Quota or
format failures leave normal alternate text; they do not disable other images.
The legacy C16 content-size counter saturates, while a separate 32-bit pixel
counter enforces the exact aggregate bound. Cache eviction/retry policy is
basic: a later image can be refused while earlier cached images consume it.

The content handler probes incoming image prefixes for supported formats and
dimensions. Unsupported formats (including WebP behind `.png`/`.jpg` names),
oversized dimensions and images exceeding the remaining decoded-pixel quota
fail before their complete source is downloaded. The full decoder still
validates accepted containers. Known HTTP entity lengths above 256 KiB fail
at the response headers; streaming responses retain the same running limit.
Incremental HTML reflow is enabled while images arrive.

`tests/streaming.py` exercises a held-open response alongside successful and
oversized requests, queued-worker release, prefix rejection and repeated
scrolling while a delayed image arrives. It uses real guest processes and
Nano-X on private disks:

```sh
python3 ports/netsurf/app.py --web --source ports/netsurf/tests/streaming.c --target native -o build/netsurf/streamcheck.fxe
python3 ports/netsurf/app.py --gui --source ports/netsurf/tests/scroll.c --target native -o build/netsurf/scrollcheck.fxe
python3 ports/netsurf/tests/streaming.py --target native
# Repeat the builds with --target ripple and .rxe outputs for RVM JIT.
python3 ports/netsurf/tests/streaming.py --target ripple
```

A separate reusable Nano-X adapter converts/scales visible tiles to RGB565,
with an optional 0..255 alpha plane. The server blends against the actual
window backing pixels. CSS repetition and clipping preserve source phase;
there is no full-page framebuffer, per-pixel transport call or host decoder.
The browser now retains up to three converted server images (65,536 cells
combined), with LRU eviction and explicit invalidation before RGBA destruction
or modification. Small CSS patterns expand once and use batches of up to nine
rectangles. Repaints reuse RGB565 uploads; scale changes reuse source images.
Oversized images, patterns and allocation/quota refusals use the bounded
visible-tile fallback. One connection slot stays available for that fallback.
Existing server image quotas remain unchanged. Hosted image drawing now also
accepts the existing 1920x1080 native window bounds. The server draws opaque
unscaled images directly through its clipping-aware blitter. Other colour and
alpha images use four-row RGB565 stripes (8 KiB VM / 15 KiB native scratch)
and that same blitter, replacing per-colour fill-rectangle calls. Bulk copies
use the existing GX raster/DMA path when large enough. Masked theme resources
keep the existing span renderer. **Refresh the Nano-X server on both targets** before
using this browser: alpha uploads require the new `FDE_IMAGE_ALPHA` extension.
The installer's optional last argument can include that server:

```sh
node ports/netsurf/install.mjs native fuse-x/build/ROOTFS.IMG /tmp/netsurf-native.img build/netsurf/netsurf.fxe build/netsurf/htget.fxe fuse-x/build/apps/nano-X.fxe
node ports/netsurf/install.mjs ripple build/fuzix/fuzix.img /tmp/netsurf-rvm.img build/netsurf/netsurf.rxe build/netsurf/htget.rxe build/fuzix/nano-X.rxe
```

Enable Images and open `/usr/share/netsurf/images.html` for the installed gallery. Its PNG has
transparent edges; the other examples exercise JPEG, interlaced GIF and BMP.
The fixtures are generated locally and require no external image service.

```sh
python3 libraries/tests/test_image.py --target both
python3 ports/netsurf/tests/images.py --target native
python3 ports/netsurf/tests/images.py --target ripple
```

The decoder passes 61 independent format vectors: 495 checks on the host and
496 on each C16 guest. Allocation-failure and mutation checks pass 18,519 cases
with guarded allocations and UBSan; no ASan result is claimed because this
macOS runtime stalls in sanitizer initialization before `main`.
Graphical tests verify exact RGB565 transparency, scaling, CSS tiling, ordinary
inline/table images, HTML dimension attributes and clipped source coordinates
on both systems, plus HTTP GIF and verified HTTPS PNG,
scrolling, direct image navigation, repeated reload and clean shutdown. JPEG
pixels are compared against independent `djpeg` output with rounding tolerance.
HTML width/height attributes use a separate legacy dimension parser: its
shift constants and color packing now use explicit 32-bit arithmetic on C16.
Without this, valid decoded images could still render as zero-size placeholders.
`NETSURF_TRACE_IMAGES=1` prints per-image decode results for diagnosis.
No input file or resource is passed to a host image decoder during guest use.
Captures and logs go under `build/netsurf/images-native/` and `images-ripple/`;
`--build` and `--rvm-test-binary` select alternate artifacts/harnesses.

## HTTP and verified HTTPS

`http/fetch.c` registers real NetSurf HTTP/HTTPS fetchers. A browser keeps at
most two independent `/bin/htget --worker` processes, with nonblocking request,
response and diagnostic pipes. Workers survive successful requests, retaining
parsed trust anchors and a connection to one origin. Reusing a verified TLS
connection avoids another process load, trust parse and handshake for every
image. The pool prefers an idle worker already connected to the requested
origin. Additional requests remain in NetSurf's queue.

The generic pipe protocol is documented in
[`ripple_http.h`](../tls/ripple_http.h). It frames final HTTP headers, dechunked
body chunks and an explicit success/error marker; process EOF is an error.
Requests and responses can span several pipe reads/writes. Poll batches remain
bounded, and available entity bytes are flushed before another network wait.
The downloader's implementation lives in `ports/tls/ripple_http.c`; the
imported `htget` command is a thin entry point to that shared implementation.
Its file-download and single-response CLI modes remain available.

Workers retain only fully consumed, unambiguous HTTP/1.1 responses. Explicit
`Connection: close`, HTTP/1.0, close-delimited bodies, skipped redirect bodies
and malformed framing discard the connection. A stale reused socket gets one
fresh GET attempt only before any response octets arrive. Cancelling a fetch
kills/reaps its worker, including buffered response bytes; quitting reaps the
entire pool. Cookies and conditional headers belong to each request and do not
leak into the next one. TLS stays in a separate process heap.

This is also an explicit component boundary: imported NetSurf is GPLv2-only,
whereas the pinned wolfSSL release offers GPLv3/commercial licensing. NetSurf
does **not** link wolfSSL or exchange its objects. The independent, generally
usable `htget` CLI sends HTTP response headers and entity bytes through normal
process pipes. Its normal file-download mode remains available.

The worker verifies certificate chains, validity dates and SAN identity using
the guest trust store, with SNI and TLS 1.2/1.3 from the existing TLS port.
There is no certificate bypass. `netsurf --cacert FILE` chooses another guest
trust file. Existing TLS connections retain the policy with which they were
verified; new connections check current trust-file contents before reusing
parsed anchors. Detailed certificate-chain UI/padlock metadata is not supplied by
this transport; the address shows the actual current scheme.

HTTP status/headers, cookies, conditional request headers, chunk decoding and
relative redirects are passed to the real browser core. NetSurf follows HTML
meta-refresh, including Wiby's `/surprise/`, and submits GET forms. Redirects
may change schemes: the address reflects the destination, and HTTPS never
silently falls back to unverified TLS. The standalone downloader's existing
HTTPS-to-HTTP redirect refusal is unchanged.

Bounds: one entity is limited to 256 KiB of source octets, headers to 16 KiB,
one line to 1,024 cells, URLs to fewer than 1,280 cells and outgoing extra
headers to four. Each request has a 240-second browser deadline, with shorter
transport operation deadlines. IPv4 and identity content encoding are used;
compressed responses, POST and uploads report errors. These source bounds
are not DOM/heap quotas: small but pathological markup can still exceed the
process window. Arbitrary modern websites are outside this profile.

Build and run deterministic real-kernel network checks:

```sh
python3 ports/netsurf/app.py --web --source ports/netsurf/tests/network.c --target native -o build/netsurf/networkcheck.fxe
python3 ports/netsurf/app.py --web --source ports/netsurf/tests/network.c --target ripple -o build/netsurf/networkcheck.rxe
python3 ports/netsurf/tests/web.py --target native
python3 ports/netsurf/tests/web.py --target ripple
# Opt-in: live websites change and the random destination is unpredictable.
python3 ports/netsurf/tests/internet.py --target native
python3 ports/netsurf/tests/internet.py --target ripple
python3 ports/netsurf/tests/web_gui.py --target native
python3 ports/netsurf/tests/web_gui.py --target ripple
```

The persistent-worker tests exercise split records and a request larger than
4 KiB, exact response boundaries, chunked trailers, cookie isolation, redirects
with unread bodies, malformed framing, stale plaintext/TLS connections, and
worker lifetime/shutdown. They use an independent OpenSSL Ethernet peer and
private guest disks:

```sh
python3 ports/tls/app.py --tls-build build/tls --source ports/tls/tests/httpworker.c --target native -o build/netsurf/httpworker.fxe
python3 ports/netsurf/app.py --web --source ports/netsurf/tests/reuse.c --target native -o build/netsurf/reusecheck.fxe
python3 ports/tls/tests/test_httpworker.py --target native
python3 ports/tls/tests/test_httpworker.py --target native --browser
# Repeat builds/tests with --target ripple and .rxe outputs for RVM JIT.
python3 ports/netsurf/tests/bench_fetch.py --full-trust --repeat 3 --output build/netsurf/fetch-timing.json
```

The timing fixture loads and renders eight PNGs through the actual Nano-X
browser with disk caching disabled. It reports startup-through-document-ready
wall time, TCP connection counts and TLS handshake counts, checks that every
image decoded, and verifies a clean filesystem. `--build DIR` selects a saved
browser/downloader pair for a baseline. Local fixture latency is not Internet
latency, and these small images do not measure large-image decoding throughput.

Performance validation on 2026-10-19 compared the saved original executables
with this build under the same pinned Fuse-X kernel. A fresh browser rendered
eight 32x32 PNGs per sample, with disk caching disabled and the complete public
trust bundle plus the fixture CA. Medians of three runs, including browser
startup through document-ready:

| Transport | Original | Persistent workers | Connections | TLS handshakes |
|---|---:|---:|---:|---:|
| HTTP | 3.20 s | 2.02 s | 9 → 2 | 0 |
| HTTPS | 5.53 s | 3.17 s | 9 → 2 | 9 → 2 |

HTTP ranges were 2.50–3.64 / 1.70–2.51 seconds; HTTPS ranges were
5.46–7.10 / 2.98–3.20 seconds. Other development builds were running, so these
are indicative local timings. Request/connection counts were consistent in
all samples. Every sample fetched all nine resources and decoded all eight
images. Raw samples and executable/kernel hashes are saved in
`build/netsurf/fetch-final-{before,after}.json` when reproducing this comparison.
The benchmark pins executable contents for each run. `FUSE_X_TEST_KERNEL` can
select an isolated native kernel while another build changes the main one.
Native acceptance used the compatible pre-shared-library compiler snapshot;
this comparison does not measure the concurrent shared-library work.

Both targets pass 131 worker checks, 108 browser-reuse checks, 22 local-file
cache checks and the complete 18-case HTTP/HTTPS regression. Native also passes
the 29-check conditional-cache probe, ordinary downloader TLS/atomic-file and
cancellation tests, exact-pixel image acceptance, and live scrolling/parallel
fetch acceptance. Guest roots pass consistency checks after shutdown.
The normal packages track the worker protocol header; update browser and
`htget` together when changing it.

Image decoding now chooses PNG predictors per row and has contiguous 8-bit
RGB/RGBA paths; the shared DEFLATE reader keeps a symbol's bit state in locals.
On the same saved native kernel, medians for 50 decodes of 128x128 images were
1.47 → 0.71 seconds (unfiltered RGB), 6.65 → 2.53 (filtered RGB), and
10.44 → 2.68 (filtered RGBA). These are decoder-only guest measurements, not
whole-page timings. Other builds were active; raw three-run samples are in
`build/netsurf/decode-pass/decode-matrix.json`. The decoder uses guest C code on
both targets; no host codec or ISA extension is involved.

HTML image references now explicitly share a handle already owned by that
same document, including speculative pre-box loads. A new low-level handle
replays headers/data asynchronously, letting the existing high-level cache
find the already decoded content. Cancellation retains separate reference
ownership. This does not make a `no-store` response fresh, persist it, or use a
referring URL as a document identifier. An independent retrieval and a new
navigation still go through normal HTTP cache policy.

The `--no-store` timing fixture now requires exactly nine requests and eight
decodes over both HTTP and HTTPS; previously it observed ten and nine.
`tests/shared.c` also checks pending/late references, release before replay,
URL isolation and two HTML navigations in one browser. Both documents fetch
all their images again, including while the old document remains in history:

```sh
python3 ports/netsurf/app.py --web --images --source ports/netsurf/tests/shared.c --target native -o build/netsurf/sharedcheck.fxe
python3 ports/netsurf/tests/shared.py --target native
# Repeat the build/test with --target ripple and sharedcheck.rxe.
python3 ports/netsurf/tests/bench_fetch.py --no-store --output build/netsurf/no-store.json
```

Native RAM scalars above bank zero bypass per-cell device routing. Wide
accesses look up/detach each touched private page once; shared kernel RAM takes
one lock. Bank-zero MMIO and display accesses retain ordered cell semantics,
and page/bank/private-kernel crossings retain copy-on-write isolation. The
runtime's common poll is an inline counter; clock/window/signal work remains
on its original once-per-1024 slow path. Focused runtime tests cover the memory
boundaries and devices. A manual comparison of generic versus RAM scalar
routing is available with
`cargo test --release -p rcc-native-runtime scalar_ram_throughput -- --ignored --nocapture`.
Normal native builds relink the kernel/runtime as well as rebuilding NetSurf;
the RVM uses the portable decoder/sharing changes with its existing runtime.

Remaining targets: image-cache quota/eviction behavior, idle connection expiry
without losing the worker's trust cache, and further compiler/address-space
work on ordinary guest-memory accesses during layout and decoding.

The deterministic peer checks HTTP, external CSS, redirects, meta-refresh,
404 content, chunked bodies, malformed/truncated/oversized responses,
mid-response cancellation, verified TLS and rejected wrong-name/untrusted or
truncated/unclean TLS. Public checks use the actual guest HTML/CSS engine and
guest TLS against Wiby (HTTP/HTTPS), example.com (HTTPS), and a real Wiby
random destination. The test Ethernet relay forwards raw TCP without
decrypting TLS or substituting content/certificates; this is public interop
coverage, not another vmnet/NAT routing test. Private disks are checked after
shutdown. Logs/captures are under `build/netsurf/`.

Validated on 2026-10-18: all 15 deterministic network cases pass on each
target. Public HTTP/HTTPS rendering passes on both, including Wiby, example.com
and Wiby's meta-refresh to different older sites. Native graphical Wiby
rendering and RVM graphical Wiby rendering plus actual search-form submission
pass. Both the network and GUI roots pass filesystem checks after shutdown;
the existing RVM HID/navigation/input regression also passes.

The RVM graphical harness writes its diagnostic log only when new output
arrives. Rewriting an unchanged file after every VM batch dominated an earlier
CPU sample and caused misleading startup/input timeouts. With that removed,
the local graphical acceptance completes in about 36 seconds and the two-page
public Wiby/form run in about 79 seconds on the development host. These include
boot, program loading and shutdown and are not interaction latency benchmarks.
Both GUI runners accept `--rvm-test-binary PATH` to reuse an already compiled
`rvm/tests/netsurf` test executable without another shared Cargo build.

## C16 adaptations

- Protocol octets occupy one C16 cell, matching guest file and socket APIs.
  The port uses `ns_octet_t`, not a false definition of exact-width `uint8_t`.
  In-memory sizes still count addressable cells.
- ParserUtils' internal UCS-4 pivot stays four serialized big-endian octets
  per scalar. Explicit loads/stores replace host-word casts; the pivot buffer
  is allocated in octet cells. This also handles non-BMP characters and
  sequences split across input chunks.
- UTF-8 lead-byte construction explicitly masks to eight bits. UTF-8, ASCII,
  ISO-8859 and Windows-125x codecs are built. UTF-16's separate host-word path
  is excluded and codec creation reports `PARSERUTILS_BADENCODING`.
- CSS 22:10 fixed-point math uses signed 32-bit bounds and defined saturating
  add/subtract. Integer conversion accepts a 32-bit argument. Numeric parser
  bounds, bytecode fields, bloom shifts, color packing and computed-style
  masks use appropriately wide operands.
- Upstream's original generators produce charset aliases, entity tables,
  element lookup and property parsers. Perl hash iteration is fixed for
  reproducible entity generation. Host tools do not execute inside the guest.
- The C16 profile supplies the upstream alignment marker without an extra
  alignment requirement. RVM links the shared software floating-point runtime
  for the CSS conversion helpers; native Fuse-X uses its existing arithmetic
  lowering. This does not enable floating-point printf or additional libm APIs.
- LibDOM exception classes and Hubbub-binding results use explicit 32-bit
  constants/types; C99 enumerators cannot represent their high class bits in
  the guest's 16-bit `int`. XML character ranges and scalar arguments retain
  all 32 bits. Numeric HTML properties format through a matching `unsigned
  long` argument. Public binding headers use upstream's installed include
  layout in the working tree. XML/Expat bindings are excluded.
- Dependencies use upstream's release `NDEBUG` profile. Without it, Hubbub
  prints a state name for every tree-building token, which is inappropriate
  for an interactive guest browser.
- LibDOM's private string alternatives temporarily use separate tagged
  storage to avoid RCC's nested union static-initializer defect. This adds
  two C16 cells (four physical octets) per string wrapper, with no public API
  change. The correct-behavior regression remains red on both backends.

Tests exercise string interning/refcounts, a 70,000-cell string, substrings
around a bank boundary, codec round trips, buffered output, split UTF-8,
Windows-1252, HTML entities/non-BMP text, CSS supplied one octet at a time,
descendant/class/ID matching, cascade, colors, dimensions and saturation.
The DOM coverage uses the actual Hubbub binding: one-octet input chunks,
Unicode/entities, IDs/classes, omitted table tags, links, input values and
32-bit numeric properties, independent deep clones, text mutation,
remove/reinsert, rejected cyclic insertions, repeated document lifetimes,
and a streamed 70,000-character text node. The parser is destroyed before
the retained document is queried. Script execution stays disabled.
They run the same C acceptance source on the conventional host and both
guest ABIs. This is focused acceptance coverage, not the complete upstream
test suites or a demonstration that arbitrary pages fit memory.

Validated on 2026-10-18: **273 checks, zero failures** on the host, native
Fuse-X and Riplix/RVM JIT. Both guest programs return zero and both temporary
filesystems pass consistency checks after shutdown. There are 441 dependency
translation units, plus RVM's software arithmetic runtime. The test root has
at least 64 MiB of physical storage and grows with the linked executables,
whose octets occupy C16 filesystem cells. The linked RVM acceptance package
contains 2,735,551 instructions and is about 21 MiB; the native package is about
11 MiB. These are executable sizes, not process heap sizes.

Both guest runs report `NETSURF_HEAP growth_cells=443803`: about 867 KiB of
additional physical heap storage reserved across the complete acceptance run.
This measures the change in the program break, with bank gaps excluded. It
includes allocator slack and the large-text/string stress cases, and excludes
globals, stack, code and kernel buffers. It is neither peak live DOM memory
for a representative website nor a browser layout/repaint benchmark.

## Compiler findings and remaining gates

The port exposed and fixed inherited static function linkage in RCC:
`static int f(void); int f(void) { ... }` now retains internal linkage.
The focused regression is `test_inherited_static_linkage`; it failed before
the compiler change and passes afterward. This prevents private helper
collisions when linking multiple imported C libraries.

LibDOM's accessor macros also exposed RCC's rejection of empty external
declarations. RCC now accepts this common compatibility extension, including
semicolons following function-generating macros. The focused regression
`test_empty_external_declaration` failed before the parser change; malformed
nonempty declarations remain errors.

Seven separately recorded compiler defects remain open, with correct-behavior
red rct tests in `c-test/tests/bugs/`:

- `test_typedef_hidden_parameter`: expressions using a variable that hides a
  typedef can be mistaken for declarations. The working sources rename the
  specific conflicting typedefs/parameters.
- `test_restrict_array_parameter`: array parameter qualifiers are rejected.
  The working bloom helper omits this optimization-only qualifier.
- `test_string_hex_octets`: numeric escapes in C strings above ASCII are
  incorrectly UTF-8 encoded again. The acceptance fixture uses literal UTF-8
  source text; numeric octet arrays test the codecs independently.
- `test_nested_static_pointer_initializer`: nested union initializers lose
  active member types, breaking pointer relocations and field widths. LibDOM's
  working string wrapper separates its mutually exclusive alternatives.
- `test_named_variadic_macro`: GNU named variadic macro parameters are rejected;
  the port uses standard C99 variadics for logging.
- `test_function_suffix_attribute`: GNU function suffix attributes are rejected;
  the C16 profile omits optimization/diagnostic attributes.
- `test_variable_length_matrix`: C99 variable-length arrays are rejected;
  frameset scratch arrays have explicit bounds, checked before frame creation.

In particular, LibCSS's escaped non-ASCII list-counter strings and the MIME
sniffer's non-ASCII image signatures need the string-literal fix or audited
numeric-octet adapters. The MIME sniffer now uses numeric arrays, including
embedded NULs and correct lengths; LibCSS's counter strings remain a gate.
UTF-16, large-document counters/coordinate arithmetic, long scheduler delays,
date ranges, stylesheet edge cases and upstream conformance
suites remain gates. ParserUtils' upstream UTF-8 policy is also older than a
full RFC 3629 conformance claim.

LibDOM 0.4.2 returns raw title text, leaving display whitespace normalization
to its caller. Its HTML `createElement` override also lacks the base document
implementation's name validation; acceptance does not claim DOM conformance.

Next: measure DOM, layout, stack and peak allocations on representative web
pages, then improve image cache policy and fonts. JavaScript and SVG
are outside this initial profile.
