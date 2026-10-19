# Terminal browser porting plan

Assessment and implementation direction recorded on October 17, 2026. This is
a browser plan with initial implementation work. The shared
[`select()` prerequisite](select.md) and [C16 curses](../../ports/fuzix/CURSES.md)
now pass guest tests on both targets. The [Lynx port](../../ports/lynx/README.md)
compiles all 82 configured source units with RCC's own preprocessor. Both
native Fuse-X and RVM/Riplix link and run Lynx, passing version and two local HTML
dump tests. With guest TLS it uses roughly 308,000 static cells across five banks;
the packages are about 39 MiB native and 43 MiB on RVM. Guest HTTP, forms and console/Telnet navigation
now have an independent Ethernet acceptance harness. The port adds complete
request writes, a 64 KiB response limit and small-cache defaults. Browser heap
arena measurements are opt-in; stack peak and a broader hostile-HTML audit remain
open.
It covers both [Riplix on RVM](../../ports/fuzix/README.md) and
[native Fuse-X](../../fuse-x/README.md).

## Current HTTP and HTTPS implementation

The [port README](../../ports/lynx/README.md) gives build, install and test commands.
Both normal launchers include Lynx in their app build and image-update paths,
including its configuration, help and license. Existing configuration is
preserved. `install.py` remains available for standalone packages and stopped
disks. Normal guest disks are not changed by acceptance tests.

The guest owns HTTP parsing, HTML rendering, DNS and TCP. The local Ethernet
fixture covers split headers/chunks, redirects, complete long requests, link
following/back, GET text/select encoding, scrolling, actual resize dimensions,
slow-load cancellation, transfer overflow, errors and recovery. A browser on one
Telnet PTY must leave a second remote shell usable. Missing terminal capabilities
produce a browser error instead of a NULL-screen access. Native raw-input queue
backpressure has its own slow-reader paste regression. Lynx restores terminal
state before suspension and repaints after `fg`, using its own signal handler
over the shared curses primitives.

Defaults are two cached documents, a 2,048-character URL bound and 15-second
connect/read waits. The hard transfer cap is 65,536 HTTP octets **per response**, including headers
and chunk framing; HTTPS counts these after TLS decryption, excluding TLS
record overhead. It also applies without a
Content-Length. This does not bound every possible HTML layout allocation.
`LYNX_RESOURCE_REPORT=1` reports the guest heap arena's high-water reservation;
`build/runtime-*.json` retains acceptance measurements. Stack peak is not yet
instrumented. Tests are focused acceptance coverage, not a complete C16 or
hostile-input audit of upstream Lynx.

The October 17 interactive fixture reserved 226,976 heap cells on native and
226,971 on RVM (about 443 KiB each), in addition to 231,149 and 231,107 static
cells respectively. This covers forms, long requests, scrolling, oversize
rejection and error recovery; it is not a worst-case renderer bound.

Live guest tests have rendered example.com, Wiby's home page and its `retro`
search results on both targets. Some current HTTP search results instead return
a JavaScript verification page. That is a fetched HTTP response, not successful
verification or a claim that the destination's content is usable. TLS,
JavaScript and anti-bot compatibility remain separate work. Live smoke tests use
private guest addresses and disks; repeated fresh guests can encounter stale
shared-vmnet NAT state when reusing an address.

## Goal and decision

A terminal browser for small, traditional HTML sites is feasible. Start with
plain HTTP, keyboard navigation and HTML forms. TLS is a separate milestone;
it is not required to reach example.com or search Wiby over HTTP.

Keep the stripped Lynx port as the chosen browser. Its static tables exceeded
one bank, so application globals now extend across banks. Local rendering and
interactive HTTP run on both targets. Continue resource and malformed-document
coverage alongside browser integration.
Do not begin by changing RCC's C data model or importing a full desktop browser.
Use the existing guest sockets and protocol stack on both execution targets.

The initial useful browser should support:

- Loading a URL, following relative and absolute links, back navigation and
  scrolling through a document.
- Text, headings, paragraphs, lists, preformatted text and useful table output.
- Editing text fields, moving focus, selecting options and submitting GET
  forms, including URL escaping and form encoding.
- Interrupting a slow load while keeping keyboard input responsive.
- Redirects, chunked responses, explicit page/cache limits and useful errors
  for unsupported URLs or content.

POST forms, cookies, uploads, compression and broader character-set support
need explicit later acceptance criteria. Search on Wiby only needs a GET form.
Start with ASCII display and a documented fallback for unsupported characters;
keep HTTP octets and decoded display characters separate. Do not require CSS,
JavaScript, images or a graphical display for this milestone.

## HTTP targets and evidence

Direct host-side HTTP requests during the assessment returned `200 OK` for:

| URL | Observation |
| --- | --- |
| <http://example.com/> | Small HTML page; response used chunked transfer encoding |
| <http://wiby.me/> | HTML search page served without a redirect to HTTPS |
| <http://wiby.me/?q=retro> | Search results served over HTTP |
| <http://wiby.me/surprise/> | HTML containing a meta-refresh to another site |
| <http://www.retronetwork.net/text/> | HTTP site found in Wiby search results |
| <http://toastytech.com/> | HTTP destination returned by Wiby's surprise page |

These are observations from October 17, 2026, not availability guarantees or
guest browser tests. The Wiby homepage uses `method="get"`, `action="/"` and
an input named `q`. Its search needs ordinary HTML form handling. Following
the surprise page automatically also needs meta-refresh support; an initial
implementation may instead expose its destination as a selectable link.
Individual search results can still require HTTPS or redirect there.

The guest already has a downloader, for example:

```sh
htget http://example.com/ /example.html
htget http://wiby.me/ /wiby.html
```

These commands save HTML; they do not render it or provide form input. The
[port README](../../ports/fuzix/README.md#networking-and-remote-terminals)
describes existing guest live-network validation and fixture tests. Do not
confuse those results with the host checks above.

## Existing foundations and gaps

| Area | Foundation | Work needed for a browser |
| --- | --- | --- |
| Network | Guest IPv4, TCP, DNS, socket descriptors and nonblocking I/O | Browser-specific socket API audit and connection limits; shared select readiness now exists |
| HTTP | Adapted Riplix `htget` handles partial I/O, timeouts, chunking and redirects | Browser integration, forms, history and document/cache ownership |
| Terminal | Termios, ANSI output, terminal dimensions, PTYs and job control | Screen library integration, key decoding, editing and resize behavior |
| Memory | Bank-spanning heap; RVM MMU process data/heap window is up to 1,024,000 C cells | Measure renderer, tables, stack, cache and page limits |
| Toolchain | Existing multi-file C applications run on RVM and native | Audit browser assumptions about integer widths, C bytes and library APIs |
| TLS | Shared guest wolfSSL, HTTPS `htget` and Lynx; see [the TLS port](../../ports/tls/README.md) | Broader interoperability and hostile-input coverage |

The Riplix `htget` implementation and the freestanding runtime HTTP helper are
different layers. Their feature sets and buffering limits are not interchangeable.
A ported browser should normally use POSIX-style guest sockets with its own
HTTP implementation, rather than being forced through the RFL buffered HTTP API.
See [htget.c](../../ports/fuzix/upstream/Applications/netd/htget.c),
[runtime HTTP](http.md) and [RFL HTTP](rfl-library.md).

The guest has bounded network pools: the OS profile has 64 Internet socket
slots, 64 TCP connections, sixteen listeners and sixteen bound UDP ports. These are shared by applications;
Telnet sessions and DNS also consume resources. Begin with one page load
at a time, no background downloads or prefetch, a small bounded history/cache,
and prompt connection closure. A browser used through Telnet must leave room
for its own terminal connection and DNS traffic.

## C16 and memory decisions

[RCC's limits](../../runtime/include/limits.h) define 16-bit `char` and `int`,
32-bit `long`, and `CHAR_BIT == 16`. Native execution retains this compatibility
ABI. Linking host ncurses or OpenSSL objects directly is not a compatible port;
such facilities would need an explicit host adapter or a source port.

Audit the browser and every selected dependency for:

- Eight-bit byte assumptions in serialization, endian probes, pointer casts,
  character classification and charset conversion. Preserve one wire octet per
  C cell at the established socket boundary; serialize protocols explicitly.
- Sixteen-bit arithmetic overflow in lengths, allocation products, offsets,
  counters, bit masks and large literals. Use appropriate existing wide types
  where needed rather than globally redefining `int`.
- Large static language, entity and character tables. Globals can now span up
  to sixteen 64,000-cell banks. RVM globals share its data/heap window, leaving
  less heap for documents; native packages reserve a separate global region.
- Heap and stack use on representative documents. A network octet held in a
  `char` array consumes two physical octets here, and auxiliary layout objects
  add memory beyond the source document's size.
- Configure probes that execute target binaries or equate C byte counts with
  octet counts. Generate a reviewed target configuration with the host tools;
  do not run an ordinary host configuration and assume its ABI answers apply.

Use the [RVM memory description](../../ports/fuzix/README.md#protected-memory)
and [native executable limits](../../fuse-x/EXECUTABLES.md) as the constraints.
The initial global-data and local-rendering gates are complete. Measure
representative and adversarial interactive loads as the port expands. The former single-bank global limit has been lifted without
changing C16 pointer representation or the VM instruction set. Bank-boundary
initializers, wide values, heap separation, syscall spans and fork/exec pass
actual native and RVM guest tests. RCC enables bank-aware global references by default, including multi-file RVM
applications. `--banked-globals=false` retains compact legacy references for
small translation units.

## Browser candidates

### Links 2.30

Links has its own terminal engine, menus and editing widgets; a curses library
is not a prerequisite for its text mode. Graphics and image dependencies can
be omitted, as can SSL and optional compression/event libraries.

However, the inspected 2.30 `configure.in` rejects `sizeof(unsigned) < 4` with
an error requiring at least 32-bit integers. That probe itself assumes an
ordinary byte model; bypassing it does not fix the code's arithmetic and layout
assumptions. Its `select.c` also requires descriptor readiness. This makes
current Links a substantial C16 port, not just a dependency/build adaptation.

Keep it as a later candidate. If selected, reuse its existing widgets and audit
integer widths explicitly rather than first building a separate widget toolkit.
Sources: [official download and source archive](https://links.twibright.com/download.php),
[features](https://links.twibright.com/features.php) and
[user documentation](https://links.twibright.com/user_en.html).

### Lynx

Lynx requires a curses-compatible screen library and uses `select()` in its
networking code. SSL and a number of other features are optional. Its existing
HTML renderer and form handling would avoid writing those components anew.

Use a stripped configuration for the feasibility build: omit TLS, optional
compression, internationalized domain names, localization, directory editing,
external protocol helpers and other nonessential features where supported.
Measure static tables and rendering allocations. Do not infer compatibility
from the absence of the particular Links configure rejection: Lynx's C16
arithmetic, library requirements and runtime behavior remain unproven.
Sources: [official installation requirements](https://lynx.invisible-island.net/current/INSTALLATION)
and [2.9.2 source archive](https://invisible-island.net/archives/lynx/tarballs/lynx2.9.2.tar.gz).

The chosen approach is to retain Lynx and fix the compiler/runtime limits it
exposes. The local renderer is the first acceptance step; a custom HTML engine
is outside this plan.

## Terminal UI decision

The initial shared library is implemented; see
[the curses contract and tests](../../ports/fuzix/CURSES.md). Its expanded guest
probe covers input deadlines, keys, bounded editing, ownership, colors, composed
windows, resize, suspend/resume and restoration on native Fuse-X and RVM JIT.
[RFL Commander](../../rfl/apps/commander/) demonstrates themes and layered UI.
These are useful for later browser presentation work; Lynx currently uses its
existing interface. Browser-specific console and Telnet tests live in
`ports/lynx/test_browser.py`.


Start by adapting the curses implementation already present in the
[pinned FUZIX archive](../../ports/fuzix/vendor/), under
`fuzix/Library/libs/curses/`. Its headers are in
[Library/include/curses.h](../../ports/fuzix/upstream/Library/include/curses.h),
and the implementation is now linked explicitly by curses consumers. Ordinary
utilities still select top-level `Library/libs/*.c` without curses.

An assessment-time compile-only probe extracted the archive to a temporary
directory and compiled its 36 C sources with the ordinary RCC userspace
include paths, `-DRIPPLE_MMU=1`, `-DNO_64BIT` and `--bank-size 64000`.
Thirty-five compiled unchanged; `scan.c` failed on the unavailable `vsscanf`
interface in that snapshot. Recheck headers and implementation together before
fixing this; a declaration alone does not supply the function. This probe did
not link a library or run a screen application on either target.

The following bundled implementation defects motivated the now-tested adaptations:

- `keypad()` is a stub; decode arrows, Home/End, Delete and escape sequences.
- `nodelay()` sets a flag that `wgetch()` does not honor; integrate bounded
  input waits with [the select contract](select.md).
- `wgetstr()` has no buffer-length bound; provide bounded editing and do not
  use unbounded input for URLs or form fields.
- Define resize, redraw, terminal restore, suspend/resume and hangup behavior.
- Audit character/attribute packing before adding Unicode; existing attributes
  occupy upper bits of a screen cell.

Curses supplies terminal primitives, not HTML forms. Browser code still owns
focus order, text editing, checkboxes, select controls, textarea behavior and
form encoding. Port the subset the candidate actually needs before considering
a broader widget library. Existing shell editing and full-screen applications
are useful behavior references, not proof that a reusable form toolkit exists.

A terminal browser does not require changes to GX or Nano-X. It should work
through a console, a Telnet PTY and RVM's `gterm`. Native graphical terminal
availability is a separate [graphics milestone](../graphics/architecture.md).

## HTTPS complexity and implementation choices

HTTPS carries ordinary HTTP through TLS over TCP, usually on port 443. The
existing guest TCP stack can transport it; replacing TCP is not a prerequisite.
Running TLS inside the guest is a substantial port, considerably larger than
the select integration. Treat it as a separate major milestone and establish
the library's C16 feasibility before estimating a delivery schedule.

| Approach | Estimated complexity | Boundary and tradeoff |
| --- | --- | --- |
| Host HTTP-to-HTTPS gateway | Low to moderate | Host fetches and verifies HTTPS; the guest-to-gateway connection remains plaintext. General browsing also needs URL, redirect and cookie handling. |
| Host-provided TLS service | Moderate | A new explicit interface delegates TLS and certificate verification to the host. Avoids a C16 crypto port, but needs handle lifecycle, buffer conversion, readiness and failure semantics on each supported runtime. |
| TLS library inside the guest | High | Retains guest-owned TLS over the existing guest sockets. Requires a library port, platform services, resource measurements and cryptographic/interoperability validation. |

These are engineering assessments, not measured implementation times. A
host-provided service is an architectural extension, not an existing API.
Neither host option establishes that TLS executes inside the guest. A CONNECT
tunnel only relays bytes and does not supply the missing TLS implementation.

### Required work for guest TLS

1. **C16 library adaptation.** Audit octet loads/stores, integer promotions,
   shifts, overflow, endian conversions and serialization. This target has no
   exact-width `uint8_t`; merely changing typedefs or passing the compiler does
   not establish correct cryptography. Run known-answer tests for each enabled
   primitive and review the adapted code's timing properties.
2. **Secure randomness.** The existing
   [socket code](../../ports/fuzix/ripple/sockets.c) explicitly describes the VM
   RNG as non-cryptographic. The [host entropy device](../arch/entropy.md)
   now supplies OS cryptographic octets through `/dev/urandom` on RVM/Riplix
   and native Fuse-X, with failure and MMU/COW tests. Use that device to seed
   the cryptographic library; clocks and deterministic VM seeds are not
   adequate substitutes.
3. **Server authentication.** Supply trusted roots, certificate-chain and
   hostname verification, validity-date checks and SNI for shared-IP sites.
   The client now uses the guest UTC calendar and the pinned guest trust file
   `/etc/ssl/cert.pem`; updates preserve locally installed trust files and
   require deliberate replacement when refreshing roots. Store certificates as
   files where practical instead of filling the global-data bank. Disabling
   verification is not completion of the HTTPS milestone.
4. **TLS-aware I/O.** Connect library send/receive callbacks to guest sockets.
   Support partial transfers, handshake deadlines, cancellation, clean shutdown
   and errors. A TLS read may require a network write; decrypted input can also
   remain buffered after the socket stops reporting readability. Drive the
   library's requested read/write states and buffered plaintext, not only the
   socket's read set.
5. **Resource and performance validation.** Measure record buffers, certificate
   chains, handshake allocations, stack and static tables. A constrained
   configuration appears plausible within the MMU heap, but that is not a
   measured fit. Benchmark handshake arithmetic on RVM and native separately;
   native execution retains C16 ABI constraints.

The subsequent source audit selected wolfSSL 5.9.4. Its wide-byte wolfCrypt
support provided a starting point, with additional C16 TLS/X.509 adaptations
and compiler fixes. [The TLS port](../../ports/tls/README.md) records the pinned
source, configuration, trust bundle and actual guest acceptance status. The
library runs in guest userspace; it is shared by the native and RVM builds.

References: [Mbed TLS integration](https://mbed-tls.readthedocs.io/en/latest/kb/how-to/mbedtls-tutorial/),
[entropy requirements](https://mbed-tls.readthedocs.io/en/latest/kb/how-to/add-entropy-sources-to-entropy-pool/),
[wolfSSL porting guide](https://www.wolfssl.com/docs/porting-guide/) and
[wolfSSL TLS 1.3 support](https://www.wolfssl.com/docs/tls13/).

### First TLS milestone: HTTPS htget

The first consumer is HTTPS-capable `htget`, before integrating the transport
into Lynx. It uses the implemented fine monotonic clock and `select()` for
bidirectional TLS waits and cancellation. Browser integration must also check
decrypted plaintext buffered inside the TLS library before sleeping.

The proposed acceptance sequence is:

1. Pass enabled cryptographic primitive tests on the actual target, then
   complete a handshake against a controlled local server.
2. Download through one TLS connection with bounded memory, SNI, trusted-chain,
   hostname and validity-date verification enabled.
3. Reject an untrusted certificate, wrong hostname and expired certificate.
   Exercise entropy-source failure, handshake timeout, connection truncation,
   partial I/O and cleanup after repeated failures.
4. Preserve `htget`'s existing destination-file protection on every TLS failure;
   verify HTTP framing, redirects and a public HTTPS download in the guest.
   Record peak memory and handshake time for RVM and native separately.
5. Integrate the resulting TLS transport into the browser, including buffered
   plaintext, bidirectional readiness and cancellation while loading a page.

## Implementation order and acceptance gates

1. **Local rendering and resource spike.** Build a minimal Lynx configuration
   capable of dumping local HTML. Record necessary compiler/libc patches,
   global-data size, stack use and peak heap on small and adversarial documents.
   If it cannot fit, revisit the candidate before completing its UI port.
2. **Reusable terminal/network prerequisites.** Use the implemented
   [select interface and tests](select.md), integrate the required curses subset,
   and audit socket calls, including asynchronous connection completion and
   `getsockopt(SO_ERROR)` if the selected configuration uses them. These
   foundations may progress alongside the rendering spike.
3. **Offline interaction.** Navigate a local document, edit a bounded URL and
   form fields, follow links and exercise suspend/resume, resize and exit.
4. **Controlled HTTP.** Use an independent local HTTP/Ethernet fixture for
   relative links, redirects, chunked bodies, partial reads/writes, GET encoding,
   connection failures, slow responses, cancellation and memory limits.
5. **Live HTTP.** Fetch example.com, search Wiby and follow an HTTP result in
   the actual guest. Validate both console and Telnet operation, preserving
   other sessions. Record RVM and native results separately.
6. **Broader web support.** Define POST/cookie/compression and character-set
   milestones; follow the [HTTPS plan](#https-complexity-and-implementation-choices)
   for a separate TLS port, beginning with verified HTTPS downloads in `htget`.

Lynx now uses the shared guest TLS transport for direct HTTPS connections.
Other clients still need their own integration; a CONNECT tunnel alone does
not provide TLS to a client.

Completion means a responsive browser running in the guest, not only a host
build, successful compilation, an HTML download or a standalone runtime test.


The [graphical browser investigation](gui-browser.md) compares Dillo, NetSurf,
Links and ViewML, and separates a renderer spike from the guest TLS work.

## HTTPS transport and crypto device

Lynx links the shared [guest TLS client](../../ports/tls/README.md), which retains
certificate parsing, trust, hostname/SAN and date checks inside the guest.
HTTPS transport calls the same facade as `htget`, with a keyboard polling hook
for cancellation while waiting on sockets. HTTP and local-file browsing retain
their existing paths. HTTPS proxy tunneling is unsupported.

The optional [crypto device](../arch/crypto.md) accelerates public-key
calculations on RVM and native Fuse-X. Software crypto remains selectable with
`RIPPLE_TLS_SOFTWARE=1`; `RIPPLE_TLS_PROFILE=1` reports stage timings. Guest
`SSL_CERT_FILE` overrides the default `/etc/ssl/cert.pem` trust bundle for Lynx.
The acceptance fixture is `ports/tls/tests/test_lynx.py`, covering verified page
rendering, certificate rejection, link navigation and keyboard cancellation on
private disks. RVM uses JIT for this coverage.
