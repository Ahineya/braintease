# NetSurf performance investigation — 2026-10-19

Significant improvements are feasible without abandoning C16 or moving TCP,
TLS, image decoding or JavaScript into a host browser. There are two independent
constraints: the small guest TCP implementation limits transfers over latency,
and native compatibility memory/continuation machinery makes CPU work expensive.
The browser's resource policy then adds failed fetches and repeated connections.

The measurement tables below describe the original baseline. Implementation is
in progress in the workspace; they must not be read as timings for the candidate
build. Benchmark runs use disposable disks and do not replace the live image.

## Hosted shared-memory expansion

The original native runtime exposed 256 banks, with only banks 240–255 shared
by kernel globals, sockets, pipes and other supervisor allocations: 1,024,000
C16 cells, or 1.95 MiB. Larger active TCP buffers exposed this unrelated cap;
worker pipes could fail to allocate and browser resources would disappear.

The hosted runtime now accepts all 65,536 banks in the existing 32-bit logical
pointer format, with sparse higher-bank storage. The first expansion made all
banks from 240 upward shared (about 7.78 GiB). A follow-up partitions that space
so applications can grow too: new SDK heaps use private banks 256–32767,
**about 3.88 GiB per process**, while shared kernel storage uses 240–255 and
32768–65535, **about 3.91 GiB**. Kernel globals and existing application addresses
are unchanged. Old 8–30 and 48–239 heaps remain usable.

These are address-space ceilings, not physical reservations. Extra heap pages
allocate only as written; fork shares them COW, with another directory level
keeping detachment bounded, and exec drops the previous sparse heap. Startup
requires roughly half a MiB of extension directory handles, not gigabytes of
zeroed memory. The host OS determines how much physical RAM can be supplied.
Connection counts and browser image/work budgets remain independent policies.
Compact inline TCP/backlog storage is retained. RVM and Brainfuck are unchanged.

The freestanding x86_64 runtime needs its own physical-memory growth policy.
For its current backing, `cc.py --heap-profile bounded` and Make's
`FUSE_X_HEAP_PROFILE=bounded` select the earlier 48–239 heap consistently in
kernel validation and SDK/application compilation, with distinct fingerprints.
Use separate build directories and matching shared libraries per profile.

Runtime tests cover high-bank scalar and bulk access, crossing the original
bank-255 boundary, the final address, allocation beyond the old kernel arena,
allocator exhaustion/reuse, private COW and shared writes across forks.
`python3 ports/fuzix/tools/test_native_heap.py` also passes hosted 64 MiB
allocation/80 MiB reallocation, sparse zeros, legacy coexistence, high-heap
pipe copies and setjmp, fork/exec isolation, and allocator edge checks. A
separate bounded kernel and matching SDK pass the same limit/boundary profile
checks in disposable hosted guests; freestanding QEMU acceptance belongs to
the x86_64 workstream. The eight-case local browser/image matrix passes after
the transition (`browser-private-heap.json`).
The existing boot/free RAM figures remain provisional accounting; they do not
report this sparse address-space ceiling or the Mac's available physical RAM.

## Candidate implementation and validation

The first implementation pass now includes:

- Caller-owned windowed TCP buffers (kernel streams use 8 KiB TX / 16 KiB RX),
  bounded multi-segment sending, congestion-window growth, duplicate-ACK/partial
  recovery, RTO backoff and a bounded out-of-order bitmap. Compact standalone
  streams remain available. Kernel socket reads copy up to two RX spans directly
  rather than imposing a 1,500-octet read clamp.
- Native logical-cell copy/fill services used by bank-aware SDK operations, plus
  NIC descriptor/frame bulk copies. Bank-zero MMIO keeps its ordered semantics.
- A combined Nano-X/HTTP-pipe/timer wait, configurable 1–4 HTTP workers, and
  bounded draining of small cancelled responses for safe connection reuse.
- Readiness-driven resolver I/O, an eight-entry positive A cache with CNAME TTL
  minima, TTL-zero exclusion, a five-minute cap and resolver-list invalidation.
- Worker gzip support with a streaming 32 KiB DEFLATE history, header/trailer
  validation, concatenation bounds and compressed/decoded size limits. Ordinary
  downloader CLI requests retain identity encoding. Workers require matching
  `libtls` shared-library builds, not just a replaced `htget` executable.
- Bounded JPEG reduction and progressive JPEG support, larger configurable
  hosted image/work budgets, lazy validation of used font glyphs, and separate
  hosted text/photo retained-resource budgets. See the current
  [browser configuration](../../ports/netsurf/README.md#images).

Focused runtime tests pass 54 checks (one manual throughput test ignored).
The shared TCP rct fixtures and the compiled kernel-profile check cover
SYN-only buffer attachment (no premature ACK), cumulative and partial ACKs, recovery,
reordering, FIN, local TX backpressure and sequence wrap. Native gzip passes
582 checks; the image decoder passes 579 checks against 66 format vectors
(578 on the conventional host), plus 20,078 host mutation/allocation-failure
cases under UBSan. The worker passes 183 HTTP/TLS checks, and the browser
reuse fixture passes 108. Native image pixel/alpha/CSS fixtures and font
resize/navigation checks pass on disposable kernels. The headless parallel
fetch probe passes 17 checks. The scripted scrolling fixture now delivers
input through the callback timer used by the combined event loop and passes
scrolling while the delayed image is still loading, followed by clean shutdown. DNS integration observes
exactly two queries apiece for the expiry, TTL-zero and resolver-change cases.
AddressSanitizer still stalls before main on this macOS host; no ASan pass is
claimed. QuickJS remains outside this implementation pass.

Candidate controlled transport medians (three samples, 256 KiB entity, a
16 KiB burst sender) are HTTP **237 / 758 / 2,483 ms** at 0 / 20 / 80 ms RTT,
and HTTPS **258 / 850 / 2,897 ms**. The original fixture used 4 KiB bursts,
so this compares the old/new receive-window configurations, not a single
isolated TCP algorithm. Some development checks/builds overlapped these runs;
these remain indicative figures. Raw data: `transport-final.json`.

A single native local-browser matrix passes all eight file/HTTP cases, including
all nine requests and eight decoded images for the small-image page. Times
were 1.07–1.18 seconds for local pages and 1.46–1.79 seconds for HTTP.
A new public ExplainingComputers observation opened **two HTTPS connections**
and decoded **16 images with no prefix rejection**, versus the original 13
connections/two decoded images. It took **14.87 seconds with images**, versus
9.63 seconds for the old browser that discarded most images. This is more
useful rendering, **not an end-to-end latency improvement on that public page**;
its photo decode/paint workload is now much larger. Images-off took 3.33 s.
Raw data: `browser-memory.json` and `browser-public-final.json`.

Remaining work includes visible-resource prioritization, decoded-content
cache eviction/lazy re-decode, row/tile reductions for PNG/GIF/BMP, progressive
scan presentation, and further compiler continuation/address-generation work.
TCP window scaling, SACK, real peer fairness/loss testing and physical NIC
acceptance also remain gates. The changes above do not establish ordinary
native-browser performance or hardware-network robustness.

## Measurements and their boundaries

Tests used the current ARM64/macOS Fuse-X kernel and managed native browser in
disposable roots. Browser tests include launch, font loading, parsing, layout and
paint; disk cache and screenshot capture were disabled. Tests from this audit ran
sequentially. Other workspace activity and host scheduling were not controlled.
Numbers below are observations, not hardware-independent performance promises.

Raw JSON, transcripts, binary hashes, private probe builds and CPU samples are
under `build/netsurf/speed-audit/`; `summary.json` collects the results and caveats.
The repeatable entry points are:

```sh
python3 ports/netsurf/tests/bench_transport.py --repeat 3 --output build/netsurf/speed-audit/transport-repeated.json
python3 ports/netsurf/tests/bench_browser.py --repeat 3 --output build/netsurf/speed-audit/browser-local.json
python3 ports/netsurf/tests/bench_browser.py --repeat 1 --public https://www.explainingcomputers.com/index.html http://wiby.me/ --output build/netsurf/speed-audit/browser-public.json
```

The transport fixture sends bounded multi-segment bursts, up to the advertised
window, and adds half the selected RTT in each direction. Its sender waits for a
cumulative ACK of each burst. It is a controlled sensitivity test, **not a Linux
TCP sender or a measurement of maximum vmnet throughput**. The older packet peer
and public relay send one segment at a time and must not be used to establish
the guest's maximum throughput. Tests retain real guest DNS, TCP, socket calls,
HTTP, TLS and certificate validation; the public relay does not decrypt TLS.

### Transfer sensitivity to latency

256 KiB response, three repetitions, medians from the guest's HTTP response timer:

| Artificial wire RTT | HTTP | HTTPS | HTTP throughput |
| --- | ---: | ---: | ---: |
| 0 ms | 522 ms | 597 ms | 490 KiB/s |
| 20 ms | 2,957 ms | 3,039 ms | 87 KiB/s |
| 80 ms | 10,627 ms | 10,769 ms | 24 KiB/s |

HTTP ranges were 510–569, 2,923–2,975 and 10,490–10,786 ms respectively. The
response timer starts after sending the request and includes waiting for its
reply and consuming its body; it excludes process launch, DNS, connect and TLS
setup. Whole-command timings and TLS work/wait counters are in the JSON.
Both schemes wrote the response to `/dev/null`, avoiding filesystem write cost.

HTTPS closely tracks HTTP as latency rises. That establishes a transport/wait
problem in this workload; it does not establish that every site's TLS setup is
cheap. The configured arithmetic accelerator was enabled during these tests.

### Browser observations

Three repetitions per controlled case, median launch-to-document-complete time:

| Page | Local file | HTTP | HTTP connections |
| --- | ---: | ---: | ---: |
| Text and no images | 1.61 s | 1.97 s | 1 |
| Eight 32×32 PNGs | 1.78 s | 2.25 s | 2 |
| Four 128×128 PNGs | 1.77 s | 2.31 s | 2 |
| Eight oversized, rejected images | 1.72 s | 2.62 s | 4 |

The tiny PNGs are highly compressible synthetic controls, not a representative
photo workload. Local oversized-image loading emitted sixteen reject callbacks
for eight URLs; that is not evidence of sixteen network downloads.

One live observation of ExplainingComputers took **3.19 s without images** and
**9.63 s with images**. The image-enabled load opened **13 HTTPS connections**,
decoded two images and logged fourteen prefix rejections. Wiby's homepage took
2.24/2.47 s without/with images. These use the conservative public relay and
include Internet variation; they are not normal vmnet measurements.

A separately built instrumented frontend placed 535–967 ms between entering
Nano-X setup and completing font loading, versus another 13–46 ms to initialize
the browser core. The region includes `GrOpen`, GC creation and `nf_load`, so
this is not an isolated font-parser timer. Font data is 3,104,355 logical octets:
about 5.92 MiB of C16 storage. Every browser process reads and validates the
entire file, including its RLE glyph records. Individual paint calls, including
browser chrome, measured roughly 7–278 ms in this small matrix. The instrumented
binary has a distinct hash; do not mix its totals with the managed-browser table.

### CPU and code generation

A freshly compiled probe decoded the same 128×128 RGB PNG 200 times, excluding
file reading from the timed region. The ordinary Clang `-O2` host reference took
a median **22.6 ms**; the current RCC/C16 application took **2.02 s**, about 89×
longer. Checksums matched. This compares the same portable decoder algorithm,
not identical ABIs, allocation implementations or generated machine code.

A separate three-second decoder sample had 2,139 execution-thread samples:
1,814 in native-runtime code, 205 in generated application code, and 120 other.
The roughly **85% runtime share** includes memory access, COW page bookkeeping,
pointer arithmetic and checkpoints. Sleeping GUI/loader threads are excluded.
It is not a percentage of browser page-load wall time. No JPEG throughput
measurement was completed in this audit.

The existing experimental QuickJS package also exposed a stale-build problem:

| JavaScript control | Existing package, median command wall time | Rebuilt package |
| --- | ---: | ---: |
| Top-level integer loop, 50,000 iterations | 46.14 s | 2.31 s |
| 2,000 object/JSON round trips | 6.29 s | 2.72 s |

Three repetitions returned the same checksums. The first loop sums `i` with
`sum=(sum+i)|0`; the second stringifies/parses `{a:i,b:"Ripple",c:[1,2,3]}`.
The pristine host QuickJS reference reported 1 ms and 3–4 ms for the inner work.
Guest `Date.now()` is coarsely quantized in this package; use host-observed
command wall time, including launch, when comparing guest packages.

The five adapted engine translation units, `quickjs.h` and `qjs-platform.c`
were byte-identical across these package builds. The rebuilt CLI/SDK/compiler
can differ: this is a package-rebuild comparison, not isolation of one compiler
pass. Package size fell from 38,167,443 to 27,441,062 host-file octets.
The legacy sample was dominated by continuation slot lookup/save and B-tree
insertion; 2,010 of 2,327 samples were runtime code. Current dense continuation
lowering is a strong explanation for the rebuild improvement. The new package
passed these controls only; it was not installed or given a full conformance run.

A further short sample of the rebuilt package, running 200,000 loop iterations,
no longer had the B-tree insertion hotspot. Continuation save/lookup still
accounted for 776 of 2,364 execution-thread samples, with 574 directly in
`JS_CallInternal`. This sample includes startup and waits; it is not an isolated
JS-dispatch percentage. It makes reducing continuation spills a concrete next
target alongside ordinary RAM access. Raw data is `quickjs-current-sample*`.

## Confirmed baseline implementation constraints

### Transport and kernel

[`net_tcp.h`](../../runtime/include/net_tcp.h), `runtime/net/tcp_poll.c` and
`tcp_payload.c` confirm:

- Kernel-profile RX capacity is 4,096 logical octets, advertised MSS is 1,460.
  Even ideal continuous delivery cannot sustain more than approximately
  `receive_window / RTT`: 50 KiB/s per connection at 80 ms RTT with this window.
  The controlled burst sender achieved less; do not attribute that entire gap
  to one guest algorithm.
- TX capacity is 512 octets, with one reliable segment in flight. At 80 ms RTT,
  the corresponding optimistic send ceiling is only 6.25 KiB/s before delayed
  ACKs and overhead. This matters for TLS client flights, POST and uploads;
  it is not itself the download receive-window limit.
- Out-of-order payload is discarded. There is no reorder queue, SACK, fast
  retransmit or growing congestion window. Retransmission starts at a minimum
  one second with backoff. Small demonstrations passing does not establish
  good behavior on lossy or reordered real networks.
- Kernel socket reads clamp each stream copy to 1,500 octets, even when the
  application asks for a large TLS record. Copies go through a shared scratch
  buffer. Writes validate/copy into scratch before copying into the TX queue.

The kernel already drains a bounded sixteen-frame burst and can bypass its
10 ms idle-poll limit when data is queued. TCP socket waits already use
readiness. Reintroducing faster unconditional timer polling is not the first fix.

There are additional costs worth measuring rather than declaring dominant:
all endpoint statuses are scanned around polling, mailbox descriptors and frames
are copied cell by cell, and hosted supervisor memory takes a mutex on scalar
access. `NetworkDevice::execute` does not use `Memory::read_into/write_from` for
frame payloads. This offers a concrete bulk-copy optimization, but this audit
does not assign it a percentage of network time.

The libc resolver is a separate latency issue: its UDP retry path still uses
`nu_wait()` in decisecond steps, and it has no TTL answer cache. New connections
call it again. It already validates replies and supports TCP fallback; don't
confuse this kernel libc resolver with the smaller standalone DNS demo.

### HTTP, TLS and browser integration

- Two persistent workers impose a global two-request limit. Success releases
  a worker for reuse; it does **not** kill it. Prefix rejection, cancellation
  and errors do kill it, losing TCP/TLS state and per-worker trust-cache reuse.
- Each worker retains only one origin. Origin changes close its old connection.
  TLS session caching is explicitly disabled. New trust contexts parse the
  public roots; subsequent connections in that process reread/compare the
  bundle and can reuse its parsed context.
- The browser's adapter supplies pipe descriptor sets, but the Nano-X frontend
  waits on Nano-X events and scheduled callbacks rather than combining those
  descriptors in its wait. Core fetch polling is scheduled every 10 ms; painting
  or layout can delay it further. A readiness-driven combined loop is warranted.
- HTTP asks for identity encoding. gzip responses are rejected. Modern text/CSS/JS
  would benefit from bounded streaming decompression. The existing 256 KiB
  source limit also excludes many real scripts and documents.
- Body records are bounded to 4,096 octets and use separate framing/body writes.
  Anonymous pipes are already supervisor-memory rings, not disk-backed pipes.
  Preserve that improvement; first measure batching and copy costs.

Keeping the HTTP/TLS worker boundary remains useful for allocation isolation
and the current NetSurf/wolfSSL licensing separation. Removing `htget` alone
doesn't repair TCP, C16 memory access or decoder costs.

### Images, fonts and drawing

The frontend rejects dimensions over 512 and budgets only **65,536 decoded
pixels across the whole content cache**. Four RGBA octet cells cost eight
physical octets per pixel on C16. JPEG is baseline-only; SOF2 progressive JPEG
is rejected both by the prefix handler and by the complete decoder. WebP is
unsupported and GIF returns its first displayed frame.

The ExplainingComputers source inventory includes large images displayed at
much smaller CSS sizes. Fetching them and allocating full source-size RGBA is
the wrong tradeoff. Merely raising the limits can consume the whole heap and
still leave source-size conversion/upload costs.

Text atlases and photos share a three-entry retained server-image LRU with a
65,536-cell budget. Text batches contain at most nine rectangles and perform
synchronous image protocol operations. Mixed fonts/photos can evict resources
and force conversions/uploads again. This is a source-confirmed opportunity;
no atlas-thrash percentage was measured. Full viewport repaint on scroll and
frequent chrome/status painting add further work.

The native backend still calls checked helpers for loads, stores and GEP.
Private memory uses COW page tables. Its direct-call analysis deliberately
excludes loops, recursion, indirect calls and unknown services. That conservatism
preserves scheduling, but leaves most codec/JS loops in resumable machinery.
Changing C `char` from 16 to 8 bits is not required to optimize these paths.

## Recommended implementation order

1. **Upgrade shared TCP and its measurement tools.** Keep a small bounded
   standalone/RBF profile. Give kernel/native profiles configurable per-active
   connection buffers, a real sliding sender, congestion control and loss
   recovery, a bounded reorder queue, and sensible window-update policy.
   Start with 16–64 KiB receive budgets; add negotiated window scaling when
   going beyond the unscaled window. Don't statically inflate every backlog
   object: current chunks have one-bank size checks and hosted shared memory
   is limited. Remove the 1,500-octet stream-read clamp with checked bulk copies.
2. **Improve the browser's fetch lifecycle.** Combine Nano-X, worker-pipe and
   timer readiness; coalesce repaint/progress work. Make worker concurrency a
   bounded configurable pool with document/CSS/visible-image priority. For a
   rejected resource, either drain a small, validly framed remainder under a
   deadline or close its connection while retaining its worker/trust context.
   Never reuse a connection with unread or ambiguously framed bytes.
   Add readiness-driven DNS and TTL caching, then bounded gzip/deflate.
3. **Fix native memory and continuation hot paths.** Recognize memory fill/copy
   loops and checked page spans; detach COW once per writable span, not once
   per stored cell. Specialize/in-line proven ordinary RAM access while keeping
   MMIO and fault behavior. Extend suspension analysis carefully so bounded
   compute regions keep native registers across safe calls. Preserve scheduling,
   signals, fork/COW, setjmp and aggregate-return invariants. Rebuild stale
   packages before using their timings to prioritize new compiler work.
4. **Deliver useful images.** Add a bounded scaled-decode API preserving source
   intrinsic dimensions separately from stored bitmap dimensions. Use scaled
   JPEG IDCT where supported and row/tile processing for other codecs. Add
   progressive JPEG decoding with bounded coefficient/scan storage and tested
   cancellation; displaying intermediate scans is a further incremental-render
   feature. Add eviction/lazy re-decode, larger platform budgets and separate
   text/photo retained-resource policy. Evaluate WebP, SVG and optional GIF
   animation after profiling representative pages. Font loading should become
   indexed/lazy or safely shared, with equivalent validation of used records.
5. **Integrate QuickJS as a browser engine.** First retain a current-package CPU,
   startup and memory benchmark. Implement NetSurf's JS heap/thread interface
   plus DOM identity, wrappers, finalizers/GC edges, navigation teardown, script
   loading/order, events, timers, Promise jobs, URL and network APIs. The existing
   Duktape-specific generated bindings are not a QuickJS adapter. Modern JS syntax
   is distinct from modern browser Web APIs and CSS support. Begin with DOM edits,
   an interactive form, timers and asynchronous fetch; then a small SPA fixture.

For JavaScript, use bounded Promise-job batches and paint/network turns; retain
interrupt/time/memory limits. QuickJS's interrupt callback can abort execution;
it is not by itself an arbitrary resumable JS time-slice mechanism. Define what
happens to a runaway script explicitly. Do not expose the terminal host's filesystem,
process or raw socket modules to websites. Origin policy, cookies and network
permissions belong in the browser adapter, including async POST/body support.
See the [QuickJS API](https://bellard.org/quickjs/quickjs.html).

## Real x86_64 acceptance

The current [x86_64 plan](../../tools/x86_64/README.md) retains C16 application
layout. Faster physical hardware or replacing vmnet with a NIC driver will not
remove the 4 KiB TCP window, 512-octet sender, dropped reordered data, worker
churn or compatibility memory costs.

Keep protocol logic independent of the Ethernet driver. Begin hardware work
with a polled/budgeted RX/TX interface and ownership rules; then add interrupts,
DMA rings and completion handling without moving protocol work into the device.
Compare against an ordinary Linux/BSD peer, not only our conservative Python
fixture. Measure queue drops, retransmits, advertised windows, RTT, socket wakeup
latency, CPU/copy cost and delivered stream checksums alongside throughput.

Required shared-stack gates before claiming robust Internet use:

- Multiple unacknowledged segments, partial ACKs, sequence wrap, split/coalesced
  reads, zero windows/reopening, delayed ACKs and concurrent readers/writers.
- Deterministic loss, duplication, reordering, burst loss, timeout/backoff and
  recovery; congestion-window behavior and fairness against another TCP flow.
- Link/configuration changes, matched ICMP/PMTU behavior, bounded fragment
  handling, FIN/RST races, resource exhaustion and cancellation under backpressure.
- Unpredictable ISNs and ephemeral ports. The current kernel `sequence()` mixes
  the VM RNG and time and explicitly does not provide cryptographic entropy.
  Resolver IDs are also predictable. Connect these to a seeded cryptographic
  source with a defined boot/failure policy; don't weaken TLS verification.
- Actual NIC-driver tests for DMA visibility, ownership and interrupt/poll races.
  Hosted protocol tests cannot establish those hardware properties.

Relevant primary contracts are [TCP](https://www.rfc-editor.org/rfc/rfc9293),
[congestion control](https://www.rfc-editor.org/rfc/rfc5681),
[window scaling](https://www.rfc-editor.org/rfc/rfc7323),
[ISN protection](https://www.rfc-editor.org/rfc/rfc6528), and
[HTTP framing/reuse](https://www.rfc-editor.org/rfc/rfc9112).

An order-of-magnitude improvement on selected workloads is credible: the
QuickJS rebuild already demonstrates one. A universal browser speedup or parity
with ordinary native NetSurf is not established. Network windows, CPU helpers,
resource policy and modern-Web compatibility need separate acceptance results.
