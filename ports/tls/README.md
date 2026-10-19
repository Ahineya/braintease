# Guest TLS for Ripple

This port keeps TLS and certificate verification in Riplix userspace.
The host supplies cryptographic entropy and optional public-key and AES-GCM record acceleration
through bounded device requests. Software crypto remains available; there is
no host TLS proxy or host socket shortcut.

## Source and data model

`upstream/` contains original wolfSSL 5.9.4 sources from the archive pinned in
`AUDIT.json`. `upstream-manifest.json` hashes every imported file.
`vendor.py` verifies the archive checksum before importing the selected client
sources. Upstream copyright and licensing files are retained: wolfSSL offers
GPLv3 under the conditions in `COPYING`/`LICENSING`, or a commercial license.

RCC keeps its C16 ABI: `char` and `int` are 16 bits, `long` is 32 bits and
`long long` is 64 bits. One conceptual protocol octet occupies one C cell.
The profile uses upstream `WC_16BIT_CPU` and `WOLFSSL_WIDE_BYTE` support.
`SIZEOF_LONG`/`SIZEOF_LONG_LONG` follow wolfSSL's octet-based configuration
convention, not RCC's `sizeof` result.

`NO_64BIT` is required by the Riplix userspace ABI: its system headers use it
to select 32-bit `time_t`. It does not remove RCC's 64-bit integer support.
The wide-byte wolfCrypt profile still uses a real 64-bit `long long` for
arithmetic. Every library unit and consumer must use the same configuration;
mixing the guest's 32-bit time structures with a 64-bit declaration is wrong.

Upstream's reference C2000 configuration exercises wolfCrypt without TLS or
X.509. Its existence is not proof that this complete TLS profile works. The
port therefore has separate primitive and actual guest/OpenSSL acceptance
tests. Compilation alone is not a passing HTTPS test.

## Adaptations

`adapt.py` verifies the source manifest and creates working copies under the
build directory. It restores explicit casts to conceptual `byte` to 8-bit
truncation with upstream `WC_OCTET`, preserving unary-operand boundaries and
single evaluation. This includes big-integer export: the small SP export
loop otherwise retained 16-bit values in serialized public keys. It also
counts initialization-counter bits using `CHAR_BIT` instead of a literal 8.
The generated `ripple-adaptations.json` records the cast counts by source.
Memory copies, moves and erasure of SP digits use `sizeof(sp_int_digit)`
C cells, while serialization keeps the original octet counts. Nine memory
operations needed that distinction; using protocol word sizes for erasure
otherwise overwrote adjacent objects. Integer sign-bit calculations also use
`CHAR_BIT`. Original imported files remain unchanged.

SP digit arithmetic and variable-point multiplication keep the compact
implementation. `adapt.py` selects the original precomputed base-point tables
and their stripe helpers for P-256/384/521. It preserves the compact ladder
for arbitrary points: the windowed alternative failed a full-width C16 ECDH
vector and is not used by the public operations. Fully unrolled RSA arithmetic
also exceeds RVM's per-function spill-frame capacity. The generated report
records these selections; both small and full-width scalar fixtures matter.

RCC now supports the required [packed integer bit-fields](../../docs/compiler-bit-fields.md),
unsigned 64-bit preprocessing arithmetic, grouped declarators, typedef-array
parameter adjustment and constant object `sizeof` bounds. The C16 ABI stays
unchanged; the port does not require GNU statement expressions.

The client profile enables TLS 1.2/1.3, SHA-1/256/384/512, HMAC/HKDF,
P-256/384/521 ECDHE/ECDSA and RSA public verification through 4096 bits with
SP C32 math. Connections negotiate AES-128-GCM/SHA-256, with RSA or ECDSA
authentication in TLS 1.2. SHA-384/512 remain available for certificate
signatures; AES-256-GCM is covered by the primitive tests. The profile excludes
CBC, old TLS versions, server mode and optional protocol/algorithm families.
The profile remains bounded; it is not a claim that every Internet site's
certificate algorithms and TLS configuration are supported.

`ripple_tls.c` supplies socket I/O callbacks, monotonic `select()` deadlines,
cancellation, host seeding and a small synchronous client facade.
`ripple_tls.h` is the public API. New connections verify the chain, validity
dates and DNS/IP SAN identity, and send SNI for DNS names. DNS certificates
must contain a matching SAN; a matching common name alone is insufficient.
Alternate paths may terminate at a local trusted anchor instead of an
unrelated cross-sign supplied by the peer. `/etc/ssl/cert.pem` is
the default guest trust store; `htget --cacert FILE` selects another one.
Read failures and unsupported entropy fail closed. Parsed anchor contexts are reused within one process after comparing the
selected file's complete contents on every connection. Forked children rebuild
their context; inherited TLS sessions reject reads/writes, and closing one in
the child does not change the parent's socket flags. TLS session resumption is
explicitly disabled and remains separate work.

CA PEM files use wolfSSL's table Base64 decoder, the same path upstream uses
for ordinary public certificates. Private/encrypted key decoding keeps its
constant-time path. This changes decoding cost, not trust, date, signature or
hostname checks. The adaptation is recorded in `ripple-adaptations.json`.

The caller retains ownership of the TCP descriptor. The facade temporarily
sets nonblocking mode, restores its flags on release, and attempts one
nonblocking `close_notify` without waiting for a reply. `htget` allows 120
seconds to establish TLS and 30 seconds per read/write operation. Network waits
are cancellable; an individual synchronous crypto operation must finish before
the next cancellation check. Truncated or unauthenticated TLS records are
errors. A close-delimited HTTP body requires authenticated TLS closure.

The socket bridge now waits for queued data and the FIN exchange before
retiring a closed endpoint. The Ethernet test peer also handles closed-tuple
reuse and never sends FIN ahead of data blocked by a zero receive window.

A future event-loop client can use the same wolfSSL objects and WANT_READ /
WANT_WRITE callbacks, checking `wolfSSL_pending()` before sleeping. Merely
calling the kernel's `select()` on a socket does not expose plaintext already
buffered by the TLS library. Other HTTP clients need an explicit transport
integration; DNS and ICMP do not become HTTPS protocols.

## Diagnosing socket failures

wolfSSL error `-308` is `SOCKET_ERROR_E`, a generic transport failure. It does
not identify a certificate or cipher problem. The socket callbacks now retain
the underlying guest errno and whether reading or writing failed. A TCP reset,
for example, reports `socket read failed: Connection reset by peer (TLS error
-308)` instead of replacing the real reason with `Input/output error`.
`RIPPLE_TLS_PROFILE=1` also prints elapsed/work/wait timing and accelerator
state when a handshake fails, including failures before the connection is
established. Neither change retries a failed handshake or weakens verification.

`tests/test_socket_error.py --target native|ripple --executable PATH` resets
an established TCP connection during ClientHello. The original executable
reproduces the uninformative `-308` message; the updated executables retain
the reset reason and errno on both guest kernels. Existing positive HTTPS and
certificate/cancellation checks remain independent of this transport test.

The 2026-10-18 VacationLovers investigation downloaded
`https://www.vacationlovers.net/` successfully through guest TLS on both kernels
with a blind TCP relay. Native `htget`, the installed native `htget` binary,
and installed Lynx also succeeded over real macOS shared/NAT networking on a
private disk/address. The server accepts the configured AES-128-GCM and P-256
profile, and the pinned trust bundle validates its chain. This does not explain
a failure in an already running session: its underlying socket error and
failed-handshake profile are still needed before assigning a cause.

## Build and acceptance

```sh
# Standalone library IR; guest applications normally invoke this automatically.
python3 ports/tls/build.py --target native
python3 ports/tls/app.py --tls-build build/tls --target native -o build/tls/htget.fxe
python3 ports/tls/app.py --tls-build build/tls --target ripple -o build/tls/htget.rxe

# Test helpers use private disks and an independent OpenSSL Ethernet peer.
python3 ports/tls/app.py --tls-build build/tls --test-crypto -o build/tls/cryptocheck.fxe
python3 ports/tls/app.py --tls-build build/tls --target ripple --test-crypto -o build/tls/cryptocheck.rxe
python3 ports/tls/tests/test_crypto.py --target native
python3 ports/tls/tests/test_crypto.py --target ripple
python3 ports/tls/tests/test_https.py --target native
python3 ports/tls/tests/test_https.py --target ripple
python3 ports/tls/tests/test_adapt.py
python3 ports/tls/tests/test_peer_flow.py
```

RVM tests use JIT. The HTTPS fixture requires local Unix-datagram socket
access and the OpenSSL command-line tool for temporary test certificates.
Test keys/certificates are private fixture material and are not installed on
normal guest disks. Public trust anchors have their own pinned provenance in
[`roots/`](roots/README.md). Existing guest trust files are preserved.

The ordinary application builds use this port automatically:

```sh
python3 fuse-x/apps.py htget
make -C ports/fuzix BUILD="$PWD/build/fuzix" "$PWD/build/fuzix/htget.rxe"
```

Normal launcher image updates install `/etc/ssl/cert.pem` along with `htget`.
Inside the guest, for example:

```sh
htget https://curl.se/robots.txt /robots.txt
htget --cacert /my-roots.pem https://my-server.example/file /file
```

`htget --response [--header LINE]... URL -` is a separate streaming CLI mode
used by the NetSurf port. It outputs one response's final status/headers, a
blank line and the dechunked entity to stdout, with diagnostics on stderr and
a nonzero exit on framing/TLS failure. It accepts non-2xx statuses and leaves
redirects to the caller; normal file-download mode keeps its existing redirect
and atomic-replacement behavior. Streaming mode limits entities to 256 KiB,
headers to 16 KiB and extra request headers to four validated lines. Buffered
entity bytes are flushed before waiting for another packet. The caller must
check process exit status, not merely pipe EOF. See
[NetSurf's transport boundary](../netsurf/README.md#http-and-verified-https).

`htget --worker [--cacert FILE]` accepts repeated bounded GET requests through
stdin and returns framed headers/body/completion records on stdout. The
serialized octet protocol is defined in [`ripple_http.h`](ripple_http.h); no C
structures or TLS objects cross it. It uses the same HTTP parser and limits
as `--response`, while retaining one same-origin connection when framing
permits. Error/cancellation paths close that connection. HTTP/1.0, explicit
close, EOF framing and skipped redirect bodies are never retained. Stale
connections can retry a GET once before any response octet is read. Idle stdin
EOF releases the connection and exits; a truncated/invalid request exits with
an error. Callers cancelling during a response must terminate/reap that worker.
The original downloader entry point delegates to `ripple_http.c`, shared by
both modes. See the [NetSurf tests](../netsurf/README.md#http-and-verified-https)
for acceptance and image-page timing commands.

The earlier `audit/` SHA-256-only profile and `probe.py` preserve the initial
compile probe. They are not the production TLS configuration.

## Connection profiling

Set `RIPPLE_TLS_PROFILE=1` to report trust-store file reading versus parsing,
handshake work versus readiness waits, socket callback time, keyboard polling
and accelerator counts. Application-read profiles separate TLS work from
readiness waits, and operation counters 13/14 identify AES-GCM records. Receive/send/keyboard times are components
of handshake work or waits, not extra durations to add to the total. Profile
printing and clock reads introduce overhead; compare ordinary wall times too.

```sh
python3 ports/tls/tests/bench_https.py --target native --output build/tls-perf/native.json
python3 ports/tls/tests/bench_https.py --target ripple --output build/tls-perf/ripple.json
# Measure repeated trust setup within a redirect, using one client process.
python3 ports/tls/tests/bench_https.py --target native --client lynx --redirect
```

The runner uses disposable roots and a local independent OpenSSL Ethernet peer.
It compares a single fixture root with all 121 pinned public roots plus that
fixture root, against the same server and response. Each sample starts a new
client process. `--executable` selects one experimental client; `--sample DIR`
collects macOS CPU samples for native Fuse-X and perturbs wall timings.
`tests/trustbench.c`, built with `app.py --source`, separately compares the two
Base64 paths over every root, checks identical DER output, times an additional
SHA-1 pass over the DER, and rejects malformed ASCII and non-octet PEM input.
The SHA-1 pass is a diagnostic workload, not a breakdown of certificate loading.

The October 18 investigation found that the old native scheduler scanned all
loaded function tokens after every guest function call. Lynx's thousands of
functions made certificate loading much slower than the same TLS library in
htget. Image collection now receives a notification when the last image owner
is dropped and scans only when an image can actually have become stale. Retained process/frame references
keep code alive as before; unreferenced images still release their tokens.

Before that fix, local Lynx samples spent 7.5–8.0 seconds loading the full trust
store on Fuse-X and 10.3 seconds on RVM JIT, versus 49–52 ms and 342 ms with one
root. Native accelerated ECC/RSA callbacks accounted for about 27–33 ms per
handshake, so accelerating those operations further would not address most of
the delay. Detailed htget profiles also isolated parsing from disk I/O: one
native run spent 4,494 ms parsing versus 49 ms reading. These are development
machine observations, not latency guarantees; cold JIT work and other host
workloads affect the results.

With the runtime change alone and the same old Lynx executable, native full-store
loading measured 1,233–1,610 ms, and the local page command took 6.5–6.6 seconds
instead of 13.9–14.5 seconds. This isolates the runtime fix from the decoder
adaptation. A separate 121-root diagnostic measured 1,806 ms for constant-time
Base64 decoding versus 952 ms for the table decoder, with identical DER output.
The direct RVM JIT diagnostic showed a smaller decoder difference (1,828 versus
1,699 ms); it still spent 6,111 ms loading the full store. Both malformed ASCII
and wide non-octet PEM input were rejected. The decoder change is a limited
improvement on RVM, where avoiding repeated parsing remains a higher priority.

With both changes built and the heavy rebuilds finished, two native Lynx samples
loaded the full fixture bundle in 347–348 ms, completed TLS in 971–992 ms, and
finished the complete page command in 2.79–3.13 seconds. ECC/RSA callbacks took
8 ms total in each handshake. Raw baseline, intermediate and final profiles are
saved under `build/tls-perf/` when reproducing this investigation with `--output`.
Final RVM JIT Lynx samples loaded the bundle in 4.69–6.29 seconds and completed
the handshake in 1.41–2.24 seconds. Complete commands including process startup
took 15.1–19.9 seconds. RVM timings vary with cold JIT work and host scheduling;
the overall difference from earlier samples cannot all be attributed to the
decoder change. Reusing parsed anchors is still the largest TLS setup target.

The post-fix native CPU sample also shows `_Transform_Sha512` during an
AES-128-GCM/SHA-256 handshake. The pinned `internal.c` `HashRaw()` updates the
SHA-384 handshake transcript whenever SHA-384 is compiled in, even when the
client offers only SHA-256 cipher suites. SHA-384 uses the SHA-512 compression
function. This is extra software work beyond certificate-signature hashes;
selecting SHA-256 suites alone does not remove it. A future profile-specific
transcript policy must preserve SHA-384/512 for certificate signatures and
handle all supported handshake paths; globally disabling them is not equivalent.

## Transfer throughput and reuse

The Unix network profile now uses a 4 KiB RX ring, advertises a 1460-octet MSS,
and retains the bounded 512-octet, single-flight TX implementation. Four TCBs
fit one C16 bank; the complete active/backlog pools fit Fuse-X's 16-bank shared
arena. Standalone contexts retain the smaller 512-octet RX/MSS profile.
The kernel drains at most sixteen frames per network poll, including raw ICMP
notifications, before driving TCP and descriptor wakeups. Queued frames no
longer wait behind the previous 10 ms whole-poll throttle; idle table scans
remain limited to once per tick so many open sockets do not monopolize RVM.
Ring copies use at
most two contiguous spans. The generic htget/network stream helpers now wait
on select readiness instead of sleeping a decisecond after EAGAIN.

The downloader coalesces request headers into one buffered write and copies
entity spans in blocks through 4 KiB input/output buffers. HTTP framing still
uses the incremental parser. Streaming mode flushes available body bytes before
waiting for more input, retains its 256 KiB limit and propagates failures.

TLS uses the optional stateless AES-GCM record device described in
[the crypto ABI](../../docs/arch/crypto.md). The guest still owns keys, nonces,
sequence numbers, record framing, peer authentication and TLS state. Accelerated
records use AES-128/256, 96-bit nonces, 128-bit tags, at most 64 AAD octets and
18432 data octets. Unsupported shapes and old devices retain the software path;
authentication failures return an error without releasing plaintext.

```sh
python3 ports/tls/tests/bench_transfer.py --target native --executable build/https-throughput/htget.fxe --output build/https-throughput/native.json
python3 ports/tls/tests/bench_transfer.py --target ripple --executable build/https-throughput/htget.rxe --output build/https-throughput/ripple.json
# A controlled one-way data-frame delay, not public Internet latency:
python3 ports/tls/tests/bench_transfer.py --target native --delay-ms 200 --output build/https-throughput/delayed.json
```

This independent OpenSSL fixture drives Ethernet separately from terminal I/O,
validates the saved body, and reports setup-inclusive wall time separately from
response-queue-to-command-completion body time (including final cleanup).
The guest's `HTTP response detail` separately measures header/body reading and
body-file writing before TLS teardown, rename and process exit. Its server keeps one segment in
flight, so results measure regressions rather than maximum network bandwidth.
`--full-trust` includes all pinned roots. The existing `bench_https.py` remains
the trust/handshake benchmark, including redirect reuse in a single process.

On the development machine, the matching 8 KiB HTTPS fixture improved from
10.3 to 46.7–59.4 KiB/s on Fuse-X and from 2.0 to 8.9–11.7 KiB/s on RVM JIT.
A 64 KiB response measured 131.6 and 30.1 KiB/s respectively. These are
response-queue-to-command-exit rates, include cleanup, and exclude connection
setup; the first connection still pays startup, trust loading and handshake
costs. RVM's 64 KiB guest response phase measured 1,131 ms, compared with
2,128 ms through command exit. With 200 ms artificially added to each server
data frame, the single-flight fixture remained latency-limited at 2.25 KiB/s
on Fuse-X, up from 1.07 KiB/s. Public-site rates depend on latency, server
behavior, page size and rendering too. Raw local measurements are under
`build/https-throughput/final-*.json` when running this investigation.

`tests/test_trustreuse.py` checks repeated connections, same-size replacement,
missing trust files, hostname rejection, replacement while a verified session
is alive, and fork isolation. It deliberately uses equal-size bundles so a
size/mtime-only cache would fail. The first connection still parses PEM. Lynx
and redirecting htget commands can reuse the context. NetSurf uses up to two
persistent `htget --worker` processes. Successful
requests retain the parsed-anchor cache and may reuse the same verified
HTTP/1.1 connection, avoiding both trust setup and a new handshake. A new
connection still verifies its peer and checks the current trust-file contents.

Further work includes streaming hash acceleration and explicit peer/policy-keyed
TLS session resumption for connections that cannot stay open. SHA-384 transcript work remains measurable even when
only SHA-256 cipher suites are offered; do not globally disable SHA-384/512
certificate support to remove it.

## Acceptance status

Known-answer tests pass inside both native Fuse-X and RVM JIT: SHA-1/256/384/512,
long-input hashes, HMAC, HKDF, AES-128/256-GCM including AAD and rejected tags,
fresh host-seeded DRBG output, and P-256/384/521 public-key generation and ECDH.
The ECC fixtures include full-width scalars and independently generated OpenSSL
answers, not only scalars one and two.

The native suite and the RVM protocol/lifecycle runs pass TLS 1.2/1.3, RSA and ECDSA authentication,
P-256/384/521 key exchange, IP SANs, cross-signed paths, binary/chunked bodies,
redirects, wrong names/SANs, missing SANs, expired/untrusted certificates,
record tampering, truncated/unclean closure, downgrade rejection, cancellation,
missing entropy and preservation of existing output files. RVM's lifecycle
checks were rerun with the launcher's `--guest-ctrl-c` flag after the harness
initially omitted it; the corrected run returns status 130, retains the shell
and passes filesystem checking after clean shutdown. Both engines' entropy,
plain HTTP/DNS and socket/Telnet regression checks pass too.

Public HTTPS passes on native for example.com and on RVM JIT for curl.se's
`/robots.txt`, using the installed Mozilla roots. `tests/test_internet.py`
uses a blind TCP relay on the private Ethernet fixture: the guest performs
all TLS and verification against the real remote server. This is public
interoperability coverage, not a new vmnet/NAT routing test.

The original software-only RVM profile was materially slower at public-key operations. Example.com's server
closed after about 15 seconds while RVM was still verifying its chain; the
same guest successfully fetched curl.se with a more generous server deadline
(about 47 seconds on the first run, 15 seconds with the normal build later).
In the local acceptance run, native P-256
handshakes took roughly 2–5 seconds; RVM cases took about 6–43 seconds depending
on the algorithms and cold JIT work. These are observations under concurrent
development workloads, not controlled benchmarks or speed guarantees.

The native application manifest currently reserves 84,201 static C cells
(about 164.5 KiB). The 188,900-octet default trust file temporarily occupies
that many C cells while loading, in addition to parsed trust objects and record
buffers. Tested requests fit the existing process window. Heap/stack peaks,
broad malformed-certificate fuzzing and a timing audit remain separate work;
successful fixtures do not establish every Internet site's compatibility.

## Crypto acceleration

The optional [crypto coprocessor](../../docs/arch/crypto.md) accelerates EC public
keys, ECDH, ECDSA verification, public RSA arithmetic and AES-GCM records
through `/dev/crypto`.
TLS and certificate trust decisions remain in this guest library. The same
packages work without the device, using their software implementation.

Set `RIPPLE_TLS_SOFTWARE=1` in the guest to force software crypto, and
`RIPPLE_TLS_PROFILE=1` for stage timings and accelerator counters. These are
process environment settings, for example:

```text
RIPPLE_TLS_PROFILE=1 htget https://example.com/ /example.html
RIPPLE_TLS_SOFTWARE=1 RIPPLE_TLS_PROFILE=1 htget https://example.com/ /example.html
```

Lynx also links this library. Its transport uses `ripple_tls_open_poll()` so its
keyboard cancellation remains responsive during network waits. This adds a
per-connection UI polling callback to the existing synchronous facade; it does
not alter certificate verification or the socket ownership contract.
