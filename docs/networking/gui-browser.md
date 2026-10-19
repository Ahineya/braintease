# A graphical web browser for FUZIX and Fuse-X

The next browser should remain a guest Nano-X application. RVM gets one
composited desktop; Fuse-X can present the same client through its hosted
windows. HTTP, DNS, TLS, certificate verification and document parsing stay
in the guest. A host WebView or TLS proxy would be a different architecture.

This investigation follows the working Lynx text-browser port. `htget` and
Lynx now use the shared guest TLS library. NetSurf integrates them through an
independent guest download worker; TLS support is not automatic for every
socket client. The audit below records the earlier feasibility investigation;
the implementation milestones at the end describe current behavior.

## Candidate comparison

| Browser | What it offers | Main porting cost | Assessment |
|---|---|---|---|
| Dillo 3.3 | Small HTML/CSS renderer and an established browser UI | C++ and FLTK; RCC currently compiles C, and host FLTK cannot be linked into the guest C16 ABI | Attractive software, but not the shortest route with the current compiler |
| NetSurf | C renderer, HTML/CSS libraries, framebuffer frontend with its own widgets | Several supporting libraries, C16/octet and allocation audits, a Nano-X plotter/frontend | Best candidate for a deliberate rendering spike and a more capable eventual browser |
| Links 2 | C browser with text and graphical frontends | Existing source audit found a configure requirement for at least 32-bit `unsigned`; bypassing that check would not fix its arithmetic | Plausible alternative if its C16 audit is smaller than NetSurf's dependency port |
| Historical Dillo / ViewML | Old lightweight browser work, including a Nano-X lineage for ViewML | Obsolete dependencies; ViewML still uses C++/FLTK/KHTML compatibility layers | Historical reference material rather than a ready-made C shortcut |

NetSurf's official site describes a 16 MB machine target. That is a useful
order-of-magnitude warning, not a measured Ripple footprint. Guest process
heap, stack, global cells, the shared physical page pool and executable
storage are separate limits. Read the current resource profile and measure
`free`/`ps` before choosing ceilings; native Fuse-X retains the C16 data ABI
even though its instructions run on the host CPU.

## Porting sequence

1. Audit NetSurf's small dependencies and fixed-point arithmetic with RCC,
   keeping the C16 ABI. Test strings, UTF-8, CSS numeric ranges and layout
   coordinates before attempting a complete browser build. Establish a
   process-memory budget and measure it on both targets.
2. Compile a small NetSurf renderer/dependency subset with RCC. Start with a
   local, bounded HTML document, a simple stylesheet and the existing fixed
   font. Measure parsing/layout peak allocations, stack depth and repaint
   cost. Keep JavaScript, SVG and optional image/font libraries out of this
   first experiment.
3. Connect one retained document surface to Nano-X. Implement clipping,
   rectangles, text, scrolling, pointer input and keyboard input, followed by
   links and simple forms. The current owned RGB565 image resources can carry
   retained surfaces; use dirty rectangles and bulk transfers rather than
   issuing per-pixel server requests.
4. Add the existing guest HTTP/DNS path and verified TLS. Drive library
   WANT_READ/WANT_WRITE states, deadlines and cancellation. Check buffered
   plaintext before sleeping in `select()`; the kernel only knows the TCP
   descriptor's readiness. Preserve URL bounds, redirect policy and incomplete
   download handling at the browser layer.
5. Port image decoders and improved fonts individually, with octet fixtures
   and allocation limits. An arbitrary web image must be decoded in the
   guest; the desktop's preconverted theme assets do not solve this part.

A browser does not require replacing Nano-X or the desktop. Keep document
rendering, browser chrome and the shared FuseDE widgets distinct so the
future custom UI can replace the first browser controls. RGB565 compositing
and DMA can accelerate retained image copies; they do not remove HTML layout
or image-decoding work.

Networking already has working guest TLS and acceptance coverage on native
Fuse-X and RVM JIT; see [the TLS port](../../ports/tls/README.md). Public HTTPS
works on both targets. RVM startup, trust-store parsing and network/crypto waits
must still fit an interactive browser's latency budget.

## Follow-up source audit: 2026-10-18

The recommendation remains **NetSurf for the first rendering feasibility
experiment**, with graphical Links as the alternative if NetSurf's memory
or dependency cost is excessive. This is a candidate choice, not a completed
port. Dillo would additionally require a guest C++ toolchain/runtime and a
compatible FLTK backend, or a substantial rewrite of its rendering layer.
Replacing its FLTK drawing calls alone does not remove its C++ dependency.

The official `netsurf-all-3.11.tar.gz` bundle was downloaded into the ignored
`build/gui-browser-audit/` directory. Its computed SHA-256 is
`4dea880ff3c2f698bfd62c982b259340f9abcd7f67e6c8eb2b32c61f71644b7b`.
No browser sources were added to the managed port or installed on a guest disk.

### Actual probes and source findings

- Unmodified `libwapcaplet/src/libwapcaplet.c` compiled to RCC IR using the
  guest include paths. This is a useful first dependency result, without a
  claim of linked/runtime acceptance for the whole library.
- The unmodified parserutils UTF-8 compilation stops at the missing
  `<inttypes.h>` header. Its public interfaces and codec implementations also
  depend on `uint8_t`, which the C16 SDK deliberately does not provide.
  Codec byte counts, UCS-4 stores, masks and native-memory endian probes need
  an octet/cell audit; typedef substitutions alone do not establish correctness.
- `libcss/include/libcss/fpmath.h` uses 22:10 `int32_t` fixed-point values,
  but clamps arithmetic with the native `INT_MIN`/`INT_MAX`. An unchanged-header
  probe of `css_int_to_fixed(96)` produces 98,304 on a conventional host and
  32,767 on both the standalone Ripple JIT and native C16 backends. The latter
  represents about 32 pixels, rather than 96. A fixed-point division probe
  shows the same clamp. This is an upstream ABI assumption, not an RCC
  miscompile. Port adapters must use bounds for the actual fixed-point type
  and separately audit integer coordinates, shifts and allocation products.
- NetSurf's framebuffer frontend can use an internal bitmap font, so FreeType
  is not mandatory for the first page. It requires libnsfb and has its own
  small widget layer; GTK, Qt and FLTK are unnecessary for that frontend.
- The normal browser enables libcurl and Duktape. Disable JavaScript for the
  first port. For HTTP, choose explicitly between porting libcurl and writing
  a NetSurf fetcher over the existing guest sockets/TLS. The latter avoids
  the initial curl dependency, but still requires correct HTTP framing,
  redirects, headers, cancellation and integration with NetSurf's scheduler.

The probes and their logs are retained under `build/gui-browser-audit/`.
They use the downloaded source without patching it. They do not run NetSurf,
parse a page or measure browser allocations. No compiler changes were needed.

### Memory is an early decision

The current Ripple process data/global/heap window has 23 banks of 64,000
C16 cells: 1,472,000 cells, about **2.81 MiB of physical storage**, before
subtracting globals and allocator overhead. Stack storage is separate.
Fuse-X's heap occupies banks 8 through 30, giving approximately the same
upper heap budget, with large globals in a separate region. These are process
address-window constraints; adding host RAM alone does not enlarge them.
See `ports/fuzix/ripple/mmu.h` and `fuse-x/memory.c` for the current contracts.

For scale, a 640×480 RGB565 surface uses 614,400 physical octets (600 KiB).
A conventional packed 32-bit image uses 1,228,800 octets (1,200 KiB). A naive
four-`char`-per-pixel adaptation would double the latter under C16. DOM,
layout, decoded images, TLS and caches all need additional storage. NetSurf's
public 16 MB machine example is not evidence that it fits this per-process
window. A realistic browser may require larger windows, stricter content/cache
budgets or a measured division of work across guest processes.

Do not retain a page-height framebuffer. Render the visible viewport or bounded
tiles, repaint damage, and scroll retained pixels. Keep initial page source,
DOM nodes, decoded-image dimensions and history/cache bounded. Measure peak
allocation and stack use before deciding whether the memory profile must grow.

### Graphics and Unicode integration

Nano-X already supplies windows, input, clipping, bitmap primitives, scrolling,
retained RGB565 resources and bulk raster operations on both targets. A libnsfb
surface backed by one Nano-X client is a possible starting point. A dedicated
Nano-X frontend using NetSurf's plotter interfaces could later avoid duplicate
framebuffers and integrate FuseDE controls more closely. Both are new adapters.

The terminal Unicode work provides reusable decoding, widths and bitmap glyphs,
but it does not turn Nano-X's general font server into a proportional Unicode
text engine. Start with bitmap font metrics and audited text callbacks. Better
font styles, proportional fonts and shaping are separate additions. Likewise,
the installed skin/icon image resources are not PNG/JPEG web-image decoders;
those codecs and their pixel layouts need their own ports and resource limits.

The first useful acceptance target is a local HTML/CSS page with headings,
paragraphs, a table, links and one editable form, rendered in a resizable window
on both systems. Add HTTP, verified HTTPS and one bounded image format afterward.
That demonstrates a real graphical browser while keeping modern JavaScript
applications outside the first milestone.

## Evidence and open gates

The initial audit above is now followed by an active
[C16 dependency port](../../ports/netsurf/README.md). The pinned source import,
checked adapters and build drivers cover libwapcaplet, libparserutils, libhubbub,
libcss and libdom (441 dependency translation units per backend, plus RVM's
shared software floating-point runtime). The acceptance executable checks
actual HTML tree construction and CSS parsing/selection, rather than a
substitute renderer. All 273 acceptance checks pass on the host, native Fuse-X and real
FUZIX/RVM JIT; both disposable guest filesystems pass post-shutdown checks.
The README records reproducible host/guest tests, compiler fixes,
open compiler regressions and the intentionally excluded UTF-16 codec.

The DOM checks feed HTML one octet at a time, retain/query the document after
destroying the parser, verify implied table elements, links, input values,
32-bit numeric properties, cloning, edits and ownership, and stream a
70,000-character text node across bank boundaries. Dependencies now use their
release profile, disabling Hubbub's per-token state printing. A contained
LibDOM string-layout adaptation avoids a separately recorded RCC union
initializer defect; pristine sources remain unchanged.

Both guests reserve 443,803 additional heap cells (about 867 KiB physical)
across the acceptance run, including stress cases and allocator slack. This
program-break measurement excludes globals and stack and is not peak live
DOM storage for an ordinary page. The current RVM test package has 2,735,551
instructions and occupies about 21 MiB of executable storage.

### Browser-core acceptance

The managed import now includes NetSurf 3.11 and libnsutils 0.1.1 (2,136
pristine files total). The local-page build compiles another 126 translation
units: the actual browser, fetch/cache, HTML/CSS layout and redraw code, with
JavaScript, curl, disk backing store and image decoders disabled. Its acceptance
frontend records the core's plot operations using fixed-cell UTF-8 metrics;
it does not open a Nano-X window or draw through a substitute renderer.

The conventional host passes 68 checks: numeric and charset bridges, resource
tables, base64 octets, talloc ownership and bank-spanning resize, MIME-handler
registration, core initialisation, a loaded HTML/CSS data URL, title,
text and 32-bit background colour, viewport reflow, a 10 KiB local file
with a table delivered through multiple 4 KiB reads, and page-info rendering.
The last drawn text row
changes from y=149 at a 480-pixel viewport to y=212 at 180 pixels. These are
functional observations using the acceptance frontend's font metrics, not a
graphical performance or typography benchmark. Native Fuse-X and real
FUZIX/RVM JIT each pass 69 checks (the extra check measures heap growth),
return zero, and pass disposable-filesystem consistency checks after shutdown.
Their layout geometry matches the host. The port README records build and
execution commands.

The core acceptance reserves about 217,800 additional heap cells on either
guest, roughly 425 KiB physical, including the allocator stress cases and
slack. It allocates no viewport framebuffer. The RVM package contains about
5.23 million instructions and 313,500 global cells and occupies about 40 MiB
of executable storage. These measurements establish this small fixture's
feasibility; peak live allocations, stack depth and representative page
budgets remain to be measured.

The compatibility profile replaces iconv with the already tested ParserUtils
codecs, uses monotonic timing, rejects compressed/oversized message records,
and bounds frameset scratch storage. Its private regex fallback is namespaced
because FUZIX's V8 regex API has incompatible signatures. The same approach
isolates NetSurf's `size_t`-length `strndup` implementation from the guest SDK's
historical `int`-length declaration. That mismatch broke the RVM call ABI,
corrupting copied URLs and header names; stylesheet failures then caused
repeated error-page loads. Talloc's allocator magic/flags are also explicitly
32-bit, rather than truncated to the C16 native `unsigned`.
Compiler regression
tests record typedef hiding, GNU macro/attribute syntax, nested-union static
initializers and variable-length arrays; checked source adaptations keep the
browser build independent of those open fixes.

DOM/layout peak-memory and stack measurements and broader page coverage remain
gates. The Nano-X local-page frontend is now implemented; see below. A successful compile or small headless
page is not proof that arbitrary websites fit the process window. Lynx remains
the usable browser baseline while these gates are evaluated.

Primary references, checked 2026-10-18:

- [Dillo user documentation](https://dillo-browser.org/user_help.html) and
  [build dependencies](https://github.com/dillo-browser/dillo/blob/master/doc/install.md),
  [Dw architecture](https://dillo-browser.org/doxygen/dw-overview.html).
- [NetSurf architecture and frontends](https://www.netsurf-browser.org/about/),
  [project requirements](https://www.netsurf-browser.org/) and
  [libnsfb](https://www.netsurf-browser.org/projects/libnsfb/).
- [Official NetSurf 3.11 source bundle](https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz).
- [Links features](https://links.twibright.com/features.php).
- [ViewML design](https://microwindows.org/ViewMLDesign.html).

## Nano-X frontend milestone

`ports/netsurf/frontend/` connects the imported NetSurf core directly to the
existing Nano-X client protocol on both targets. One top-level window contains
browser chrome and a clipped document viewport. There is no duplicate client
framebuffer or page-height raster. The plotter uses supported rectangle, line,
bitmap-font text and bounded polygon/disc primitives. A persistent GC region
and coalesced invalidation avoid per-clip query round trips and idle repainting.
Scroll currently repaints the viewport rather than copying retained pixels.

The first UI supplies local file/data navigation, a location field, back/forward,
reload, scrolling, resize/reflow, links and editable HTML text fields. It uses
fixed 8x16 metrics and ASCII ROM glyphs with replacement for other scalars;
Unicode shaping/fonts, clipboard, advanced drawing, images and multiple
windows are explicit follow-ups. Browser chrome remains separate from plotters
and core services so FuseDE widgets can replace it later.

The browser is in the normal build and image inventories, with examples under
`/usr/share/netsurf/`. The standalone installer can write a new image or perform
a checked, backed-up atomic update of a stopped image using `--in-place`.
Build, install, controls and real-kernel graphical checks are documented in
[the port README](../../ports/netsurf/README.md#graphical-browser). This completed
step 3 above; the following milestone adds real network fetching.

## Guest HTTP/HTTPS milestone

`ports/netsurf/http/fetch.c` supplies asynchronous HTTP and HTTPS fetchers to
the real NetSurf core on both Fuse-X and FUZIX/RVM. A separate guest
`/bin/htget --response URL -` process performs DNS, HTTP framing and verified
TLS. Standard pipes carry final response headers, dechunked entity bytes and
diagnostics. The frontend polls nonblocking pipes in bounded batches and stays
available while the worker waits. Stop/navigation/exit kill and reap the
worker; completion requires both valid framing and a successful worker exit.
Up to two workers run concurrently; NetSurf queues additional requests.

This division keeps wolfSSL's trust/record/crypto allocations outside the
browser's DOM/layout process window. It also keeps GPLv2-only NetSurf separate
from the GPLv3/commercial wolfSSL release: no wolfSSL objects or symbols are
linked into NetSurf, and the independent HTTP CLI remains generally usable.
TLS and trust decisions still occur in the guest. The host test relay only
forwards TCP; normal guest networking can use the existing Ethernet backend.

The browser supports HTTP statuses, relative redirects, HTML meta-refresh,
external CSS, cookies, conditional headers and GET forms. Wiby's live HTTP
and HTTPS homepages work; its random-page route uses meta-refresh, and both
targets have followed it to actual older websites. Example.com's HTTPS page
also renders after guest certificate verification. The worker checks chains,
dates and SAN identity with the installed public roots and sends SNI. There
is no verification bypass; detailed certificate/padlock UI remains future work.

Requests have a 256 KiB source-entity limit, 16 KiB header limit, 1,024-cell
line limit and a 240-second browser deadline. Only IPv4, GET and identity
encoding are enabled. POST/uploads and compressed responses fail explicitly.
These bounds do not impose a live DOM allocation quota; pathological or
large pages can still exceed the C16 process window. Images, JavaScript,
advanced fonts and tabs remain disabled.

The port README records build/install commands and real-kernel tests.
Deterministic fixtures cover external CSS, HTTP/HTML redirects, chunked bodies,
404 content, bad framing, truncation, size bounds, cancellation and rejected
TLS certificates/closures. Separate opt-in public tests exercise Wiby,
example.com and a random destination, plus graphical captures and RVM keyboard
navigation. Disposable filesystems are checked after shutdown. Broader
compatibility, page memory/stack peaks and interactive latency remain open
engineering gates; public-page success does not close them.

## Streaming images and trackpad stability (2026-10-19)

The image handler now rejects unsupported formats and oversized dimensions
from incoming prefixes; known responses exceeding 256 KiB fail at their HTTP
headers. Two fetch workers avoid letting one slow resource monopolize the
queue. Images retain the existing aggregate decoded-pixel limit and complete
container validation. Incremental reflow lets arriving images update layout.

A separate Nano-X input issue could disconnect a scrolling browser even with
images disabled. Each wheel pulse previously queued both a button-down and a
synthetic release. A trackpad burst could fill the 64-event quota before the
browser finished drawing, marking its connection as bad. The server now
represents momentary wheel pulses as counted runs in private queue metadata.
The client still receives ordinary wheel-down events; no wire ABI changes.
Runs retain direction/window/modifier and intervening button/key ordering,
and accumulate at most 4,096 pulses. Motion updates can interleave without
allocating one node per pulse. The normal queue quota remains in force.

NetSurf processes up to 16 pending input events before repainting, reducing
repeated viewport drawing during a gesture while bounding time away from
fetch and timer callbacks. The event queue regression uses actual adapted
producer, peek and dequeue functions:

```sh
python3 ports/nanox/tools/test_events.py
```

The port README records the streaming/network/image acceptance commands.
