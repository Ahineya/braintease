# YouTube Music and reusable guest media libraries

Accepted direction, 2026-10-22: build a self-contained terminal YouTube Music
client for Fuse-X and Riplix x64. Search artists, albums and songs, open their
tracks, and play results in order or with Music's continuous shuffle behavior.
Keep queues transient initially; saved playlists and other account features can
follow. No runtime dependency on a Mac mini, remote extractor or transcoder.
RVM is outside the performance acceptance target for this work.

## Architecture

Use JavaScript for service APIs and changing player extraction, C for media
transport/demuxing/decoding and PCM submission, and RFL for the TUI where its
existing widgets and Music queue behavior fit. Prefer maintained upstream
implementations for formats, codecs and extraction. Write small platform
adapters and application policy locally.

```text
RFL or C terminal interface and transient queue
    guest JavaScript service adapter on QuickJS
        verified guest HTTPS and account session storage
        upstream player extraction and challenge solving
    C streaming media reader
        WebM demuxer -> libopus decoder -> existing PCM device
```

The extraction worker is a local guest process with a bounded input/output
protocol. It must remain separate from audio refill so parsing a player cannot
starve playback. Cache prepared player logic by content hash and solver version;
use explicit cache limits, atomic updates and eviction. Treat player code as
untrusted: a solver context gets no filesystem, network or process capabilities.
An outer worker can fetch inputs and communicate with the restricted context.

The current QuickJS terminal host supports modules, full contexts, typed arrays,
Promises and asynchronous OS bindings. Its `fuse:http` GET subset buffers at
most 256 KiB, has four request headers and no request body. Extend the existing
HTTP/TLS boundary for POST/form/JSON bodies, streaming, cancellation, long signed
URLs, range reads, credential handling and suitable response limits. Preserve
verified certificates, framing checks and cross-origin credential stripping.
Larger limits must retain explicit resource budgets and backpressure.

## Upstream choices

| Component | Candidate and acceptance decision |
| --- | --- |
| Player challenges | [yt-dlp EJS](https://github.com/yt-dlp/ejs), which supports QuickJS; prove pinned upstream fixtures in our actual guest before adopting it. EJS is the solver, not a complete YouTube extractor. |
| Service APIs | Assess [YouTube.js](https://github.com/LuanRT/YouTube.js) against our JS host; it needs Fetch and browser-style support APIs. Bundle required modules on the development host; do not require Node or npm inside the guest. Compare its adapter cost with a small service-specific module backed by maintained EJS. |
| Audio decoding | Port the reference [libopus](https://opus-codec.org/docs/) decoder first. Test numeric and octet assumptions, compare reference PCM, then measure sustained decoding. |
| Container | Assess [nestegg](https://github.com/mozilla/nestegg), a C WebM demuxer. Verify Opus headers, delay/pre-skip, timestamps, lacing, seeking and end trimming against reference files. |
| Additional formats | MP4/AAC, Ogg Opus, MP3 and Vorbis follow their own use cases and tests. A first WebM/Opus path does not require libxml or a full FFmpeg port. DASH XML parsing is needed only if the selected retrieval path consumes manifests. |
| Audio output | Reuse the existing PCM ownership, readiness, drain and gain contract. Extend Music's bounded engine rather than creating a second device API. |

Retain source pins, hashes, licenses and generated adaptation steps. Keep
upstream source pristine. Do not silently replace guest codecs or networking
with host services.

## Google authentication

Keep account login distinct from playback extraction. Google's
[device authorization flow](https://developers.google.com/identity/protocols/oauth2/limited-input-device)
can show a URL and code for approval in an existing browser session. Google
controls any further verification; the application cannot promise to avoid
2FA. The flow requires an appropriate registered OAuth client.

An API OAuth token does not establish playable stream access. The current
[yt-dlp extractor documentation](https://github.com/yt-dlp/yt-dlp/wiki/Extractors#logging-in-with-oauth)
reports that YouTube extraction no longer supports OAuth; browser cookies are
used where account access is required. Validate current behavior before
choosing the account UX. A one-time browser-assisted session import is compatible
with independent guest operation. Keep credentials in private regular files,
never in command arguments, logs, package payloads or solver contexts.

Player signatures, throttling challenges and proof-of-origin/attestation tokens
are separate mechanisms. Passing EJS fixtures proves none of account acceptance,
attestation availability or live playable URL access by itself.

## Delivery gates

1. **Extraction feasibility.** Run pinned upstream parser/solver code and real
   player fixtures on pristine host QuickJS and C16 Fuse-X QuickJS. Compare
   exact signature and `n` outputs with upstream vectors. Record resource
   budgets, timings, failures and a failing negative control. Then retrieve a
   public track through guest HTTPS, extract its audio URL, transform it in
   the guest and read a bounded media range. Distinguish fixture compatibility
   from live extraction and successful playback.
2. **Local compressed audio.** Port libopus and the selected demuxer as reusable
   guest components. Decode controlled files and compare with upstream output;
   cover mono/stereo, lacing, pre-skip/end trim, truncated input and cancellation.
   Prove decoding faster than playback and stable PCM under UI load.
3. **Streaming transport.** Extend HTTP/TLS and the JS adapter; test request
   bodies, redirects, signed URLs, chunking/ranges, stalls, partial failures and
   cancellation. Feed bounded demux/decoder buffers without downloading an
   entire track or repeatedly copying it through JavaScript.
4. **Integrated public playback.** Search/open albums and artists; pass typed
   track descriptions to the C engine. Support ordered playback, pause, volume,
   next/previous and Music's shuffle bags/history. Avoid a repeated track at
   cycle boundaries; skip failed tracks with bounded retries and visible errors.
5. **Account UX and packaging.** Establish the working login/session route,
   persistence, expiry/revocation handling and private import. Publish target
   packages and shared dependencies through pkg. Accept x64 playback only when
   its TLS/crypto and real PCM capabilities are available.

Use disposable images for guest tests. Do not modify user disks or deploy a
network service as part of these gates. Keep large downloaded player fixtures,
generated bundles and build products in ignored build storage with bounded
retention; the repository keeps pins, scripts and concise results.

## Initial feasibility findings, 2026-10-22

The reproducible [restricted extraction probe](../../ports/ytmusic/extraction/README.md)
compares pinned upstream vectors with pristine host QuickJS and real C16 Fuse-X.
Prepared EJS player execution passes all five vectors and the failing negative
control, but takes about 103 seconds and peaks at 83.5 MiB of tracked engine
allocations with `read-v1`. Full EJS preprocessing hits its 600-second deadline
while parsing; this is not an acceptable cold-start implementation.

The maintained YouTube.js dependency slicer is a better warm-path candidate:
its prepared source is about 176 KiB rather than 3.5 MiB. Guest execution of
that source passes the same five vectors and negative control in about three
seconds, peaking at 5.7 MiB of tracked engine allocations. These are individual
experimental runs using different variants of the same player version; they
are not a controlled multi-player benchmark. Full YouTube.js guest preparation
also hits the 600-second deadline in parsing, reaching 67.6 MiB of tracked
allocation before interruption. Cold guest preparation is therefore still a
performance blocker; live stream URL extraction remains unproven.

The player is not decoded through JavaScript. These costs arise before media
playback, from parsing and compiling changing service code. Meriyah builds
787,589 syntax nodes and 1,686,609 objects/arrays for the pinned 2.4 MB ES6
fixture with the analyzer's parser options. This makes a rich general-purpose
AST expensive even though the resulting challenge code is much smaller.
Keep a guest-produced, versioned prepared-code cache; do not treat host-prepared
fixtures as a production workaround. Evaluate cold-start costs before adopting
either backend, and repeat the measurements under ordinary LP64 when available.

The [libopus audit](../../ports/opus/README.md) passes RCC frontend compilation
for all 129 generic fixed-point translation units with upstream's serialized
pseudostack allocation mode. VLA/alloca configurations are currently blocked.
Linking, C16 integer/octet correctness, actual PCM comparison and decoding
throughput remain separate gates. No shared codec package or playback claim
follows from frontend compilation alone.

## Compiler and QuickJS optimization pass

The extraction fixtures also exercise compiler/runtime changes: balanced C
switches and native dispatch tables, bounded direct calls through acyclic
helpers, reconstruction of fixed local addresses when resuming, checkpoints on
actual CFG cycles, validated wide reads and same-bank
address arithmetic in hosted `read-v1`, and release of expired read pins before
scalar stores. The latter avoids manufacturing COW sharing on private pages;
real fork sharing still detaches normally. No pointer/data-layout migration or
new loader ABI is involved. Checked mode and freestanding import gating remain
available; `read-v1` remains explicit.

QuickJS reuses a bounded set of small released call frames and replaces
bit-at-a-time count-leading/trailing-zero helpers with binary decisions.
Stack accounting, lowered memory limits, allocation failure unwinding and
generator/async ownership remain checked. The default regression command in
the [extraction probe](../../ports/ytmusic/extraction/README.md) runs both
prepared-player workloads; its `--full` option additionally requires successful
cold preparation. A timeout is still a failed gate. Consult that probe's current
results before treating warm-path improvements as extraction readiness.

### Checkpoint, 2026-10-22

This stage ends with working offline prepared-player tests and a blocked cold
preparation gate. On the otherwise idle Apple M4 mini, sequential single-guest
runs changed EJS process wall time from 46.97 to 46.59 seconds and YouTube.js
from 6.53 to 6.09 seconds. These are small single-run differences, not evidence
of a major speedup. Earlier laptop timings overlapped busy jobs and are not a
reliable controlled comparison.

Final cold YouTube.js preparation still exceeded its 600-second JS deadline
while parsing, reaching 53,831,447 tracked cells (102.7 MiB). It freed all
tracked allocations, returned to the shell and passed the filesystem check.
This interrupted parse does not establish its eventual memory requirement.
Full cold EJS was not rerun with the final compiler. Both prepared backends
passed the five exact upstream vectors and failing negative controls. Compiler,
runtime, fork/scheduling, RFL, QuickJS numeric/full-context and memory-failure
checks also passed; x64 dispatch/phi object generation was checked separately
from actual host execution.

The [results receipt](../../ports/ytmusic/extraction/performance-results-2026-10-22.json)
records hashes, timings and validation counts. The
[probe README](../../ports/ytmusic/extraction/README.md) contains reproduction
commands and limitations. The runner rejects concurrent checks sharing a build
directory, and cold timeouts remain failures rather than accepted baselines.

The next investigation should target continuation slot transfers and repeated
whole-function global rebinding, both hot in the cold-run profile. Batching
transfers needs a separate helper/ABI and lifetime review coordinated with x64;
it was not implemented here. Rerun cold preparation before claiming extraction
readiness, then proceed to live guest HTTPS/URL extraction and codec gates.
Ordinary LP64 remains a separate migration path in the accepted x64 plan.

No installed guest disk or normal kernel was replaced. About 200 MiB of
redundant local test products were removed; one matching measured probe/library
pair, the private test kernel, source pins and small evidence files remain in
ignored build storage. The isolated mini workspace was removed after collecting
the results. Live playback, account login, media decoding and the application
TUI are still future work.

## C16 and native x64 migration

Follow the [x64 master plan](../x64-master-plan.md) and
[native ABI stages](../arch/native-x64.md). Current Fuse-X and x64 RXE programs
retain C16 layouts. The x64 path introduces System V AMD64 LP64, ordinary byte
addressing and ring-3 ELF64 applications, initially static and eventually
dynamically linked. A native C ABI is not Linux executable compatibility.

Keep upstream codec/demuxer sources shared. Generate C16-specific integer/octet
adaptations only for C16 builds; native builds use normal upstream C types and
layouts. Provide explicit-width protocol fields, sample counts, timestamps and
logical-octet lengths. Never serialize native structs, banked pointers, `long`
or `size_t` as a cross-process/file format. Native and C16 components exchange
versioned encoded messages; they cannot directly share pointers or C objects.

Use normal library APIs within each ABI. Existing C16 guest shared libraries
and future native ELF shared libraries are distinct artifacts with independent
ABI identities, dependencies and symbol versions. Package for target + ABI,
not CPU alone. Keep libraries buildable statically during bring-up, and ready
for dynamic packaging as each loader supports it. Do not invent an ELF dynamic
loader or alter the native syscall/kernel work in this application lane.

A narrow platform adapter owns file-octet conversion, allocation, clocks,
network readiness and PCM device marshalling. An ordinary host C build serves
as an upstream reference and future LP64 porting check; only an actual guest
run establishes Riplix runtime support. Avoid tying new codec code to DEVICE
mailboxes, hosted runtime internals or today's checkpoint scheduler.
