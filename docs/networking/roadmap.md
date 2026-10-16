# Networking roadmap

Agreed sequence: **DHCP → stack reliability → TCP → HTTP client → HTTP server
→ high-level RFL networking**.
DHCP, stack reliability, TCP active/passive opens and HTTP client/server are
implemented. High-level RFL networking is the next milestone. Current behavior
and limits are in the
[overview](README.md) and [library guide](library.md). It does not define new
public function signatures or imply those functions exist.

The original goal is complete: the guest resolves `syma-lang.org` using its own
UDP/DNS implementation and pings the answer. The guest C HTTP client/server
milestones are also complete. The next goal is typed RFL networking and
pleasant, general-purpose async over that shared guest stack.

## Requirements shared by every milestone

Keep Ethernet on the existing DEVICE_* commands. Protocol state machines,
checksums, retransmissions and HTTP parsing stay in the shared guest C stack
on every target; the later RFL layer wraps it through the architecture ABI.
Keep one octet per cell and the existing ISA, including RBF compatibility.
Host setup remains the runtime/backend's responsibility; guest network
configuration remains the guest stack's responsibility.

Build reusable layers in or alongside `runtime/net/`, exported through small
headers and included in `libruntime.par`. Examples should supply application
behavior, not duplicate packet or stream handling. Shared internals may be
refactored as necessary; exact file names and API signatures are implementation
decisions to review with the first bounded prototype.

Use caller-owned state and explicit limits for buffers, connections, listeners,
parsers and retries. Expose progress, would-block, timeout, peer closure and
errors distinctly. Define send-buffer ownership and callback lifetimes before
applications depend on them. Keep unused modules and their data removable by
the linker.

Poll-driven operation is the baseline. Supply a monotonic clock and drive
protocol timers even without incoming traffic. No phase requires threads,
interrupts, POSIX sockets, rOS multitasking or runtime argument support.

Each phase needs independent packet/stream fixtures plus a small live test;
[Testing](testing.md) defines the normal RVM/native matrix. RBF runs remain
explicit, bounded checks, not a prerequisite for every iteration.

## 1. DHCP client

**Outcome:** a guest can start without a compiled-in IP, mask, gateway or DNS
server, obtain a lease, print its configuration, resolve a name and ping it.
Keep explicit static configuration available for deterministic fixtures and
networks without DHCP. Failure to acquire a lease must be visible; do not
silently choose a potentially conflicting static address.

**Status: implemented and validated on RVM and native.** The
[DHCP guide](dhcp.md) documents the public API and its limits. An explicit
unconfigured context supports bounded DHCP broadcast bootstrap without widening
ordinary application delivery. The guest handles DISCOVER/OFFER/REQUEST/ACK,
NAK, retries, option validation/overload, ARP probes and DECLINE, T1 renewal,
T2 rebinding and expiry. Lease accounting uses seconds across wrapping
millisecond clock readings. Static configuration remains available.

Shared `net_configure`/`net_unconfigure` operations clear ARP and pending sends
and advance a configuration generation. Active DNS lookups cancel on a changed
configuration, while unchanged lease renewals preserve them. No saved-lease
restore or DHCPRELEASE is provided in this version.

**Acceptance:**

- A controlled DHCP peer proves first acquisition, dropped messages/retries,
  wrong IDs/MACs, malformed options, NAK, renewal, rebinding and expiry.
- Two guests obtain distinct leases without manually choosing static IPs.
- On live macOS vmnet, the guest prints the acquired configuration and uses
  the acquired DNS server for the existing DNS-to-ping demonstration.
- The host can ping the leased address; existing static ping/DNS tests still
  pass. No host API supplies a pre-decoded lease in place of guest DHCP.

Recorded coverage: [DHCP tests](testing.md#dhcp), including two guests, malformed
traffic, simulated long leases and live guest DHCP → DNS → VPS ping.

Reference: [DHCP, RFC 2131](https://www.rfc-editor.org/rfc/rfc2131.html).

## 2. Stack reliability and shared lifecycle

**Outcome:** configuration changes, a long-running application and common
network failures have explicit behavior before TCP depends on them.

**Status: implemented and validated on RVM and native.** The
[lifecycle guide](reliability.md) specifies expiring neighbors, bounded ARP
retry/cooldown, explicit cache invalidation, link epochs, pending-send cleanup,
DHCP reacquisition and DNS cancellation. Local queue backpressure does not
consume an ARP peer's response window. Counters remain fixed and saturating.

Validated ICMP unreachable, time-exceeded and parameter errors correlate the
quoted packet against bounded recent accepted transmissions before notifying
UDP/echo work. DNS reports specific network/path errors; a malformed, unrelated,
cancelled or previous-attempt quote cannot cancel a current query. No new
DEVICE_* pins, host protocol substitution or scheduler were introduced.

**Acceptance:** deterministic tests change a peer's MAC, expire a route, lose
and restore the link, fill queues, inject relevant/unrelated ICMP errors and
change local configuration. Tests cover retry cooldown, pending-send
cancellation, history/cache bounds, clock wrap and DHCP lease removal/reacquisition.
[Recorded checks](testing.md#stack-reliability) include independent wire fixtures
and live host/DNS-to-VPS regressions. RBF remains opt-in.

IPv4 reassembly, path-MTU adaptation, outbound ICMP error generation and host
transport/helper reconnection remain explicit limitations; they are not hidden
parts of this completed guest lifecycle milestone.

## 3. Minimal TCP library

**Outcome:** guest applications can connect to a host service and accept
connections using the same reusable TCP implementation.

**Status: active/passive TCP implemented and validated on native, RVM interpreter
and RVM JIT.** See the [TCP guide](tcp.md) for ownership, timers and the exact
supported subset, and [TCP tests](testing.md#tcp) for evidence. The guest
exchanges 4,097 binary octets against both an independent packet peer and an
ordinary host TCP socket. Four caller-owned connections fit a bounded table,
each with 512-octet TX/RX buffers and one segment in flight. Passive listening and accept are now implemented too; the
[HTTP server guide](http-server.md) documents their first application.

The design and acceptance requirements for this phase are:

Start with one active connection and fixed buffers against a controlled peer.
Keep the state machine separate from packet serialization and application
I/O. Then add passive open/listening before the HTTP server milestone; the
client milestone may precede that addition.

The implementation needs tuple/checksum/header validation, a three-way
handshake, 32-bit sequence arithmetic, ordered byte delivery, acknowledgements,
retransmission deadlines/backoff, peer flow control and explicit close/reset
behavior. SYN/FIN sequence accounting, sequence wrap, duplicate segments,
partial acknowledgements and receive-window bounds require direct tests.
Define handling of out-of-order data without unbounded buffering; a small
implementation may discard it and acknowledge the current receive position.
Handle segmentation and MSS limits without relying on IP fragmentation.

Start conservatively with a small amount of data in flight. Respect the peer's
advertised window, specify zero-window behavior, and define loss/congestion
response before relying on external-network tests. A happy-path handshake alone
is not a completed TCP milestone. Document supported options and limitations
rather than claiming a complete general-purpose TCP stack.

Keep application streams independent of segment boundaries. Reads and writes
may make partial progress; callers must be able to retain/retry unsent data and
observe EOF separately from an empty receive buffer. Specify orderly shutdown,
half-close, reset, timeout and TIME-WAIT/resource reclamation. Establish an
initial-sequence-number policy appropriate to the intended test scope.

**Acceptance:**

- Guest-to-host echo transfers more data than one segment and the fixed
  application buffer, preserving every octet without unbounded allocations.
- The independent peer drops data and ACKs, duplicates/reorders segments,
  advertises small/zero windows, closes early and resets connections.
- Tests cover corruption, invalid ACKs/tuples, sequence wrap, idle timeouts,
  repeated connection reuse and bounded cleanup after failure.
- Passive open adds a bounded listener/backlog and host-to-guest echo
  using the same core. Slow or abandoned clients cannot exhaust state forever.

Reference: [TCP, RFC 9293](https://www.rfc-editor.org/rfc/rfc9293.html). Select and
document the initial supported subset against that specification during design.

## 4. HTTP client and shared HTTP utilities

**Outcome:** a guest resolves a name, connects over TCP, requests a plain HTTP
resource and prints or streams its response body.

**Status: implemented on native, RVM interpreter and JIT.** The
[HTTP guide](http.md) defines ownership, deadlines, bounded framing and the
one-command temporary host service. Independent guest DNS → TCP → HTTP tests
verify a 10,230-octet body, and ordinary host HTTP tests stream 13,033 octets
through a 173-cell reader. The response decoder and ASCII/decimal utilities are
separate from connection orchestration. [Coverage](testing.md#http) includes
split boundaries, invalid lengths, early close and stalls. RBF and public VPS
HTTP are not claimed as tested. TCP listener/accept and phase 5 are now implemented as well. The original design/acceptance requirements follow.

Begin with a controlled host HTTP service and a small HTTP/1.0 request, explicit
connection closure and a `Host` header for virtual hosting. Parse status and
headers incrementally: a line or delimiter may span any number of TCP reads.
Support Content-Length and connection-close body framing with explicit limits
and premature-EOF detection. Reject unsupported transfer encodings explicitly;
chunked decoding can be a separate extension. Do not assume every response fits
one packet, one memory bank or one application buffer.

Extract bounded ASCII/decimal formatting, header parsing, body framing and
stream read/write progress into shared modules usable by the server. Do not
create another monolithic demo. HTTPS/TLS, redirects and persistent connections
are separate work, not implicit requirements of this first plain HTTP client.

**Acceptance:** a controlled service splits every parsing boundary, sends a
body larger than the receive buffer, advertises invalid/conflicting lengths,
closes early and stalls. The guest either emits the exact body or a specific
error, without silently truncating it. Afterward use an explicitly configured
plain HTTP endpoint on the user's VPS if available; do not assume its current
web service supports cleartext HTTP.

References: [HTTP/1.0, RFC 1945](https://www.rfc-editor.org/rfc/rfc1945.html),
[HTTP/1.1 framing, RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html).

## 5. HTTP server serving hardcoded HTML

**Outcome:** an ordinary host browser or `curl` fetches a page served by guest
C over the raw NIC and guest TCP stack. This is an agreed goal after the client,
not an optional host-side mock of HTTP.

**Status: implemented.** Native, RVM interpreter and RVM JIT serve the hardcoded 984-octet HTML
page to ordinary host curl clients; a browser also rendered the native page.
The shared library handles bounded incremental requests, partial responses and
phase deadlines. Caller-owned listen/accept pools and an optional compact
TIME_WAIT table permit normal repeated requests without aborting successful
closes. [The guide](http-server.md) defines ownership and the supported subset;
[testing](testing.md#tcp-listeners-and-http-server) records malformed/split/stalled
host requests, pool reuse, independent packet loss/reorder/window tests and
existing-stack regressions. RBF and public Internet serving are not claimed as
tested. The original design/acceptance requirements follow.

Add the TCP listener/accept path if it was deferred during the active-open
prototype. Begin with one serviced connection at a time, fixed resource limits
and a configured unprivileged port such as 8080. The example prints its actual
static or leased IP and listening port after becoming ready. Start with host
reachability; an inbound public Internet service or NAT port mapping is not
required for this milestone.

Serve `GET /` with hardcoded ASCII HTML, `Content-Type: text/html`, the exact
Content-Length in wire octets, and connection closure. Parse request lines and
headers incrementally, including a browser's HTTP/1.1 request and required Host
handling, while returning a deliberately small documented response format.
Unknown paths get 404; unsupported methods get an explicit error, not the page.
Bound request-line/header sizes, total receive time and idle time. Malformed,
slow or disconnected clients must release their connection state. Request
bodies, uploads, keep-alive, chunking, TLS and filesystem-backed content are
outside the first version.

Put reusable request parsing, routing/method dispatch and response serialization
in libraries alongside the client utilities. The example should contain its
HTML and handler/application loop. Preserve the distinction between buffered
write acceptance, bytes acknowledged by TCP and final connection closure.

**Acceptance:**

- Host `curl` and a browser display the expected guest-generated HTML. The
  host/backend does not run a replacement web server or inject an HTTP reply.
- Tests compare status, headers, Content-Length and body exactly, then repeat
  requests to prove connection slots and buffers are reused.
- Split requests, oversized headers, unsupported methods/paths, a stalled
  client, an early disconnect and transport loss/retransmission are covered.
- Existing DHCP, DNS, ping and TCP tests pass. Record RVM/native outcomes
  separately; select only a small optional RBF smoke request when practical.

## 6. High-level RFL networking

**Outcome:** RFL applications can resolve names, use UDP/TCP, fetch HTTP and
serve the hardcoded HTML page through a typed library over the shared guest C
stack. Follow the C client/server milestones with this layer; do not duplicate
TCP/IP in RFL or substitute host sockets for native execution.

See [RFL networking and async design](rfl.md) for the proposed staging,
ownership questions, executor requirements and acceptance targets. Implement
and validate [static traits](../../rfl/traits.md) before relying on generic I/O
abstractions. Concrete polling wrappers can be prototyped earlier; traits do
not block the preceding C networking work.

Static traits and associated types are implemented. The first polling library
now supplies Future/Poll, bounded wakeup slots, timers and deterministic one/two
root runners. General-purpose async is the goal: general places, resource
ownership, automatic cleanup on every exit, task composition and suspended-borrow
rules are required milestones before async syntax. The [RFL design](rfl.md)
distinguishes implemented polling APIs from those language/runtime requirements.
Generic trait parameters remain an independent extension driven by library needs.
The production network reactor and typed RFL C-stack wrappers remain to be built.

### Saved checkpoint — 16 October 2026

**Work parked here at the user's request to switch tasks.** Resume from this
checkpoint; the polling foundation is complete, general-purpose async is not.

Implemented:

- Static traits, Printable-based library printing, and associated output types.
- [`std/task`](../../rfl/std/task.rfl): Context, bounded wakeup slots, coalescing,
  generation checks, slot retirement and wrapping-clock timer registration.
- [`std/future`](../../rfl/std/future.rfl): Future/Poll, checked callback futures,
  immediate results, cooperative yielding, lazy timers and explicit cancellation.
- [`std/executor`](../../rfl/std/executor.rfl): Reactor, block_on, optional turn
  budgets and fair scheduling of two roots with different output types. Stop and
  error paths retire wakeups and cancel unfinished work.
- A function-value return-type inference fix, with a regression test: local and
  captured callables and C function pointers supply numeric comparison context.

Validation recorded at this checkpoint: **271 release tests passed**, using
`cargo test --release -p rfl --tests --quiet -- --test-threads=4`.
[`tests/futures.rs`](../../rfl/tests/futures.rs) adds eight tests: seven execution
cases checked on both native and RVM, plus the inference regression.
The [deterministic example](../../rfl/examples/futures.rfl) ran on both targets,
printing `hello from a future`, `125`, and `2` on separate lines. Its timer uses a
fake clock; this is not a real-time or live-network validation. No new explicit
BF networking/futures run was required; routine RBF checks remain opt-in.

Current limits: cancellation is guaranteed at the runner boundary, **not on
arbitrary variable/scope destruction**. Callback-future copies alias one
operation. There is no async/await syntax, compiler-owned async frame, general
spawn/join-handle system, production clock/idle/network reactor or RFL socket
wrapper. Existing C protocol tests do not prove those absent RFL layers.

Resume with **general place borrowing and resource ownership**, then automatic
deterministic cleanup across scope exit, replacement, return, `?`, loop exits,
container destruction and parent cancellation. Settle resource copy/clone/move
behavior before adding cleanup hooks. Follow with owned composition and task
lifecycles, safe suspension rules/state-machine lowering, and network/library
integration. Preserve the completed polling tests as acceptance cases; the
[detailed design](rfl.md#required-path-to-pleasant-async) records the remaining
semantics and validation targets.

## Separate and later work

Runtime application arguments remain deferred at the user's request. The native
[target guide](../../rcc-native/README.md#compatibility-layout-and-abi) already
describes `main(argc, argv)` support; inventory that implementation before
planning shared VM/native argument delivery. The current DNS example still
uses a build-time macro. These networking milestones must not block on a new
argument ABI or concurrent compiler changes.

Other later work includes integrating DNS TCP fallback, EDNS/cache policy,
CNAME follow-up queries, Linux TAP, controlled-time replay files, broader TCP
support and throughput, TLS/HTTPS, richer web serving, IPv6 and additional
NICs. rOS driver locking, scheduling and daemons can be
added when the OS has those foundations.
