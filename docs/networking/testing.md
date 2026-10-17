# Networking tests and evidence

Run commands from the repository root with the appropriate release toolchain
built. The default build path shares runtime artifacts; build different bank sizes
**sequentially**, or set `NET_PRIVATE_RUNTIME=1` to isolate dependency builds
inside the example output directory. The DHCP launcher selects this isolation.
Tests normally cover RVM and native;
RVM JIT is an additional mode. RBF execution is explicitly opt-in because its
build and execution cost makes it unsuitable for every networking iteration.

## What has been validated

These are recorded implementation milestones as of 2026-10-16, not a claim that
a concurrently changing toolchain has been rebuilt and retested. Re-run the
relevant matrix after compiler, runtime or protocol changes.

| Milestone | RVM | Native | RBF |
|-----------|-----|--------|-----|
| Device/queue contract and guest-memory adapter checks | Passed | Passed | Earlier bounded adapter tests passed |
| Bidirectional ping with the actual host OS | Passed; interpreter/JIT coverage | Passed | Earlier live milestone passed |
| Guest ICMP to the VPS IPv4 address | Passed | Passed | Earlier live milestone passed |
| Guest DNS for `syma-lang.org`, then three validated pings to the answer | Passed | Passed | Not repeated for DNS |
| DHCP acquisition, retry, conflict/DECLINE recovery and two simultaneous leases | Interpreter/JIT packet tests passed | Packet tests passed | Not repeated for DHCP |
| DHCP lease → acquired DNS server → VPS ping | JIT/live shared vmnet passed | Live shared vmnet passed | Not repeated for DHCP |
| Host ping to DHCP lease and automatic helper cleanup | JIT/live passed | Live passed, executable copied outside repository | Not repeated for DHCP |
| Independent UDP/DNS packet fixture | Interpreter and JIT passed | Passed | Opt-in; not part of routine DNS matrix |
| TCP active-open, nine independent wire cases and ordinary host TCP echo | Interpreter/JIT passed | Passed | Live JIT DHCP/connection/traffic confirmed; full echo exceeded 600-second check |
| TCP listeners + guest HTTP server | Interpreter/JIT C units, five wire cases and curl/host matrix passed | C units, five wire cases, curl/host matrix and browser rendered | Not repeated for server |
| HTTP GET: incremental parser/stream units, guest DNS → TCP → HTTP and ordinary host HTTP | Interpreter/JIT units and DNS wire fixture; JIT live matrix and three interpreter live cases passed | Units, wire fixture and live HTTP passed | Not repeated for HTTP |
| Refactored persistent monitor and automatic helper cleanup | JIT/live host ping passed | Live host ping and executable outside repository passed | Not repeated for this refactor |

The DNS milestone included malformed/mismatched replies, compression failures,
out-of-order CNAME/A records, unrelated addresses, retries, timeout, NXDOMAIN,
SERVFAIL, truncation and missing A records. Focused C regressions cover DNS name
bounds, clock wrap/cancellation, UDP bindings, odd/empty payloads, checksums,
backpressure and invalid lengths. Linker regressions cover unused storage,
retained pointer/callback relocation and small memory banks.

Do not infer one host's success from another or label an IPv4-only test as DNS
coverage. The older `test-net-vps.py` resolves names on the host unless given
an explicit address; only the guest DNS demonstration establishes this goal.

## Offline C and linker regressions

```sh
./rct test_vm_network test_net_protocols test_net_dns_names test_net_dhcp test_net_lifecycle \
  test_link_dead_global_storage link_reachability --backend rvm --timeout 30
./rct test_link_dead_global_storage --backend rvm --bank-size 4096 --timeout 30
cargo test --manifest-path src/ripple-asm/Cargo.toml --lib
```

`rct` disables automatic networking for its offline runs. `test_vm_network`
checks raw device behavior with the link down. `test_net_protocols` uses a fake
NIC to check the guest protocol library. These C tests need no vmnet helper or
Internet connectivity. The protocol tests link C runtime functions; use the
native example/packet fixture below for native coverage rather than assuming
the native single-file test runner links every runtime C module.

## Independent local packet peer

The Python fixtures implement an independent Ethernet peer over a temporary
Unix datagram socket. They require local socket permission, but no vmnet,
external network or privileged network setup. A restrictive execution sandbox
may need to allow the local socket bind.

Build one-shot ping and run both directions, including corrupt checksums:

```sh
scripts/build-net-ping.sh rvm
scripts/build-net-ping.sh native
scripts/test-net-smoke.py
scripts/test-net-smoke.py --jit
```

The persistent monitor adds startup announcement, repeated requests, guest
logging, empty/odd/56-octet payloads and continued execution after replies:

```sh
NET_DEMO=monitor scripts/build-net-ping.sh rvm build/network/monitor/rvm
NET_DEMO=monitor scripts/build-net-ping.sh native build/network/monitor/native
scripts/test-net-smoke.py --monitor --jit
```

Build the DNS demo with its default hostname, local IP and DNS server. The
fixture deliberately expects those defaults; a demo previously rebuilt with
custom `--ip`, `--dns` or hostname must be rebuilt first:

```sh
NET_DEMO=dns-ping BANK_SIZE=8192 scripts/build-net-ping.sh rvm build/network/dns/rvm
NET_DEMO=dns-ping scripts/build-net-ping.sh native build/network/dns/native
scripts/test-net-dns.py
scripts/test-net-dns.py --mode rvm --jit
```

The DNS peer verifies outgoing IPv4/UDP checksums, request fields and retry IDs.
It injects wrong addresses/ports/IDs/questions, invalid lengths, fragments,
corrupt checksums and malformed compressed names, then supplies a valid
out-of-order CNAME chain with an unrelated A record. The guest must ping the
selected synthetic address with the correct binary payload. Other cases check
DNS error statuses, a lost first query and an entirely silent server.
`--case success`, `--case retry` or another case listed by `--help` selects a
focused run. The fixture never calls a host DNS resolver.

## Stack reliability

`test_net_lifecycle` has 59 deterministic assertions covering ARP expiration,
changed MACs, exponential request intervals, failure/cooldown recovery, local
backpressure, explicit invalidation, DNS cancellation, INFO/RX/TX link loss,
restoration/MAC changes, and DHCP starting while disconnected. It also checks
quoted ICMP validation, old-attempt/replayed/expired quotes, echo error events,
MTU diagnostics, fixed cache/history bounds, fresh-binding error isolation, clock wrap and
counter saturation.
It passes on native, RVM interpreter and RVM JIT. `test_net_dhcp` separately
proves that a bound lease is removed on loss and a fresh lease acquired on return.

The independent DNS packet peer adds these focused cases to its normal matrix:

```sh
scripts/test-net-dns.py --case icmp-unreachable --case icmp-time-exceeded \
  --case icmp-parameter --case icmp-mtu --case icmp-old-retry
scripts/test-net-dns.py --mode rvm --jit
```

It sends malformed checksums, short/fragmented quotes, wrong source/ID/ports,
then a valid router error. The guest must report status 11 (unreachable) or 12
(path error), rather than timing out. The old-retry case quotes the first query
after its retry has been sent; the current query must still resolve and ping.
All 12 DNS/ICMP cases pass independently on native, RVM and RVM JIT.

Build and run the normal static and DHCP monitors for real host ping and helper
cleanup (`test-net-auto.py --jit`, then `--dhcp --jit`). Repeat DHCP → acquired
DNS server → VPS ping with the launcher below. Link loss/restoration and hour-long
lease transitions use controlled NIC/clock tests; the live backend is not
artificially disconnected. No full Doom or RBF run is required.

## DHCP

The fake-NIC regression `c-test/tests/runtime/test_net_dhcp.c` drives virtual
time through acquisition, malformed options, wrong IDs/MACs/servers, NAK,
option overload, ARP conflict/DECLINE, renewal, rebinding and expiry. It also
covers finite leases longer than one millisecond-clock wrap, infinite leases,
DNS cancellation on configuration change, preservation of an explicitly
installed replacement IP, and flushing the final DECLINE through local TX
backpressure before terminal failure. A bound address conflict must not emit
an ARP reply claiming the relinquished IP. Renewal now re-resolves an expired
server mapping; bound lease loss waits for the link and reacquires on return.
All 56 assertions passed on native,
RVM interpreter and RVM JIT.

The independent Ethernet fixture uses real wire packets and paced time. Build
its two variants with the default hostname:

```sh
NET_DEMO=dhcp-ping NET_PRIVATE_RUNTIME=1 BANK_SIZE=8192 \
  scripts/build-net-ping.sh rvm build/network/dhcp/rvm
NET_DEMO=dhcp-ping scripts/build-net-ping.sh native build/network/dhcp/native
scripts/test-net-dhcp.py --timeout 60
scripts/test-net-dhcp.py --jit --timeout 60
scripts/test-net-dhcp.py --case success --clients 2 --timeout 60
```

Cases are `success`, `retry` (lost first DISCOVER), and `conflict` (ARP conflict,
DECLINE, backoff and a different offered address). Each validates DHCP fields,
UDP/IP checksums, rejected malformed/mismatched replies, three ARP probes and
the final wait before announcement. The peer advertises its own DNS server,
then independently validates a guest DNS query, three outgoing resolved pings
and an incoming host-style echo request/reply. It never resolves the name on
the host. `--clients 2` runs guests concurrently against the same peer and
asserts distinct leases. `--mode` and `--case` select focused tests.

For actual macOS DHCP and host ping, including helper cleanup:

```sh
scripts/run-net-dhcp.sh rvm --monitor --build-only
scripts/run-net-dhcp.sh native --monitor --build-only
scripts/test-net-auto.py --dhcp --jit --timeout 50
```

For a real lease, guest DNS using the leased server, and VPS ping:

```sh
scripts/run-net-dhcp.sh native syma-lang.org
scripts/run-net-dhcp.sh rvm syma-lang.org --jit
```

These live tests passed on native and RVM JIT with capture disabled. They
recorded the leased address/mask/router/DNS and three validated VPS replies;
the host-only test also validated three actual host pings per guest. Renewal,
rebinding and expiry use the fake clock rather than waiting for an hour-long
vmnet lease. RBF remains optional and was not rebuilt/run for this milestone.

For isolated C regression builds during concurrent runtime work, the builder
accepts `NET_SOURCE=/absolute/test.c`, `NET_FAKE_NIC=1` for tests providing their
own raw NIC, and `NET_PRIVATE_RUNTIME=1` for private VM dependencies. These
settings were used for the native/RVM protocol, DNS-name and offline-device
regressions; ordinary `rct` registration remains available above.

## TCP

The four registered fake-NIC tests have 107 assertions in total:

- `test_net_tcp`: handshake, partial writes/ACKs, sequence wrap, active close,
  TIME_WAIT, reset, configuration/link loss and cancellation.
- `test_net_tcp_receive`: ordered/overlapping/duplicate data, receive-window
  clipping, ring reads, EOF/half-close, simultaneous close, malformed options,
  closed-port reset limits and explicit urgent-data rejection.
- `test_net_tcp_timers`: retransmission/backoff, RTT ambiguity, delayed local
  acceptance, unsent-data ACK rejection, clock wrap, deadlines, zero windows,
  pending-frame invalidation, connection capacity and slot reuse.
- `test_net_tcp_errors`: matched hard/soft ICMP, MTU diagnostics and old-quote
  isolation after tuple reuse.

All 107 pass on native, RVM interpreter and RVM JIT. The normal unit builds use
8192-cell banks; the capacity test owns five TCB objects and uses 16384.

```sh
./rct test_net_tcp test_net_tcp_receive test_net_tcp_errors --backend rvm --bank-size 8192 --timeout 30
./rct test_net_tcp_timers --backend rvm --bank-size 16384 --timeout 30
```

For the independent Ethernet peer, build the static fixture variants. The
forced initial sequence proves wraparound independently of the C tests:

```sh
NET_DEMO=tcp-echo NET_PRIVATE_RUNTIME=1 BANK_SIZE=8192 \
  scripts/build-net-ping.sh rvm build/network/tcp/rvm -DTCP_INITIAL_SEQUENCE=0xfffffff0UL
NET_DEMO=tcp-echo NET_PRIVATE_RUNTIME=1 \
  scripts/build-net-ping.sh native build/network/tcp/native -DTCP_INITIAL_SEQUENCE=0xfffffff0UL
scripts/test-net-tcp.py
scripts/test-net-tcp.py --mode rvm --jit
```

All nine cases pass separately on native, RVM interpreter and JIT: success,
loss, reorder, zero, partial-ack, malformed, reset, early and refused. Successful
streams compare 4,097 binary octets exactly, exceeding every fixed TCP and demo
buffer. The peer negotiates MSS 137, splits echoes into 113-octet pieces and
wraps its own sequence too. Loss covers SYN, data, ACK, echo, peer FIN and the
final ACK; zero-window probes and partial ACKs must preserve the byte stream.
Malformed traffic includes incorrect tuples, offsets, options, checksums,
fragments, ACKs and RSTs. Early EOF and reset must surface explicit failures.
The peer never uses a host TCP socket or Internet connection.

A separate test uses an **ordinary host kernel TCP socket**, guest DHCP and
automatic host-only vmnet with capture disabled:

```sh
scripts/run-net-tcp.sh
scripts/run-net-tcp.sh rvm --jit
# Reuse those DHCP-enabled builds, including the interpreter:
scripts/test-net-tcp-live.py
scripts/test-net-tcp-live.py --mode rvm --jit
```

Native, RVM interpreter and JIT all passed the exact 4,097-octet exchange,
peer EOF, guest half-close and helper cleanup. The temporary server binds only
to the host vmnet address, not every host interface. Guest TCP constructs and
processes every segment; the service only echoes application bytes. See the
[TCP guide](tcp.md) for supported limits and the caller's ISN responsibility.

The existing protocol/DNS/DHCP/lifecycle/device C regressions are repeated after
the integration changes. An archive-linked TCP unit also validates extraction
and function-pointer relocation. A 4096-bank monitor links successfully with
all TCP objects omitted, and has the same binary size as the normal link that
includes unused TCP objects. RBF remains excluded from the routine matrix.

An explicit RBF live run is available with automatic host echo too:

```sh
scripts/run-net-tcp.sh rbf --jit
```

It builds the Brainfuck image and allows 1800 seconds by default
(`--timeout SECONDS` overrides it).
Guest stdout, host/telemetry stderr, fixture results and the RBF runtime report
are separate files under `build/network/tcp-echo/rbf/`. Keeping telemetry
separate prevents progress dots from corrupting success-marker checks.
The default `test-net-tcp-live.py` modes remain native and RVM.

The explicit RBF JIT check acquired a real lease, established its host TCP
connection and continued exchanging pattern data, but did not finish the full
4,097-octet exchange within the 600-second test bound. Helper cleanup passed
on termination. This is partial interoperability evidence, not a full RBF echo
pass; the launcher now allows 1800 seconds for an explicit run. The native/RVM
full-stream results above remain separate.

## HTTP

The registered `test_net_http_parser` and `test_net_http_client` tests have
81 assertions (43 parser, 38 client). They cover every two-part split of a
response, single-octet input, zero output capacity/backpressure, binary body
cells, status handling, Content-Length/close framing, EOF, header/line/count/body
limits, decimal overflow, duplicate/conflicting lengths, unsupported encodings,
malformed CRLF/field syntax, partial writes/reads, DNS success/failure,
cancellation, request injection/length bounds, maximum port/host/path sizes,
clock wrap and a total deadline even while the caller stops reading.

```sh
./rct test_net_http_parser test_net_http_client --backend rvm --timeout 30
scripts/test-net-http-unit.py
scripts/test-net-http-unit.py --mode rvm --jit
```

The private unit runner links only the needed protocol C modules, explicitly
supporting native (the native single-file rct runner does not link them). It
uses 16384-cell banks because the stream test also owns a fake network, request,
response, capture buffers and many test strings. The actual HTTP demo uses
8192-cell banks. Results/build logs are under `build/network/http-unit/`.

An independent Ethernet peer supplies a synthetic DNS answer, then exchanges
TCP packets and a 10,230-octet HTTP body. It verifies guest DNS question/ports/
checksums, a compressed A answer, Host/request serialization, sequence wrap,
MSS 137, variable read boundaries, exact body output and guest close. It never
opens a host TCP socket or calls a host resolver:

```sh
NET_DEMO=http-get NET_PRIVATE_RUNTIME=1 BANK_SIZE=8192 \
  scripts/build-net-ping.sh native build/network/http-dns/native \
  -DHTTP_DHCP=0 '-DHTTP_HOST="web.ripple.test"'
NET_DEMO=http-get NET_PRIVATE_RUNTIME=1 BANK_SIZE=8192 \
  scripts/build-net-ping.sh rvm build/network/http-dns/rvm \
  -DHTTP_DHCP=0 '-DHTTP_HOST="web.ripple.test"'
scripts/test-net-http.py
scripts/test-net-http.py --mode rvm --jit
```

Native, RVM interpreter and RVM JIT pass the DNS → TCP → HTTP wire test.
These controlled configurations are separate builds from the DHCP-enabled live
client and do not require vmnet, Internet access or administrator rights.
Unix socket binding must be allowed by the execution environment.

A separate **ordinary host kernel TCP service** validates interoperability with
DHCP-enabled native and RVM guests. Build/run the launchers, then exercise the
service's response cases sequentially (one server uses port 8080):

```sh
scripts/run-net-http.sh
scripts/run-net-http.sh rvm --jit
scripts/test-net-http-live.py --mode native
scripts/test-net-http-live.py --mode rvm --jit
```

All 11 cases passed on native and RVM JIT: `length`, `close`, `split`,
`hold-open`, `empty`, `not-found`, `conflict`, `early`, `chunked`, `limit` and
`stall`. Success compares the complete 13,033-octet HTML body (larger than one
8192-cell bank), not only a checksum. Error cases require a specific failure
and prohibit `HTTP OK`. Holding the socket open after the declared length must
still complete the body. A truncated length fails with status 34, conflicting
lengths with 32, unsupported transfer coding with 16, limits with 33 and a
stalled body with 8. RVM interpreter also passed the live `length`, `close` and
`early` cases. The split live case paces individual header octets; exact
boundary coverage comes from deterministic decoder tests since a host TCP
stack can coalesce writes. Service threads, guest processes and owned vmnet
helpers are cleaned up on success and failure. Capture remains disabled.

The live fixture uses a numeric host address because host-only vmnet does not
advertise recursive DNS. The independent packet fixture separately proves the
complete guest name-to-HTTP chain. A named public VPS cleartext endpoint has
not been configured/tested; the previous guest DNS-to-ping evidence is separate.
RBF was not rebuilt or run for HTTP.

Existing protocol, DNS-name, DHCP, lifecycle and TCP handshake/receive/error
regressions also passed on RVM after adding HTTP. A compiler defect discovered
by a test is recorded as the intentionally red correct-behavior regression
`string_escape_octets`: numeric C string escapes above 127 expand as UTF-8.
HTTP uses explicit byte arrays where such data is needed; its tests pass.
The frontend fix is separate from this networking milestone.

## TCP listeners and HTTP server

The new registered units add 52 listener assertions, 37 request-parser assertions
and 23 server-session assertions. Together with the 81 HTTP client/parser
assertions, all 193 pass on native, RVM interpreter and RVM JIT. The standalone
unit runner now includes both HTTP directions:

```sh
scripts/test-net-http-unit.py
scripts/test-net-http-unit.py --mode rvm --jit
./rct test_net_tcp_listen test_net_http_request test_net_http_server --backend rvm --timeout 30
```

The listener uses a fake NIC and real TCP frames. It checks bind/pool conflicts,
bounded half-open/global slots, SYN-ACK retransmission and stable ISNs, invalid
ACKs, final-ACK data/FIN, accept ownership, explicit release, stop/rebind,
configuration/link loss, locally blocked TX, TIME_WAIT, full compact-table
backpressure, duplicate FIN ACKs, delayed-SYN protection, expiration and safe
attach/detach of the optional caller-owned table. Private listener builds use
16384-cell banks. The HTTP units check every two-part request split and
single-octet input, Host/length/version validation, malformed/oversized fields,
unsupported bodies/encodings, partial binary writes, exact serialization,
cancellation, session reuse at the same pool address, clock wrap, trickles and
request/send/close deadlines. The existing 107 TCP assertions also pass across
native, RVM interpreter and JIT after adding passive open. A private archive-linked
listener passes all 52 checks, and the existing monitor still links in 4096-cell
banks (3843 data cells), without allocating a compact closing table.

An independent Ethernet active-open peer checks the guest server over a local
Unix socket, using no host TCP stack or host HTTP implementation:

```sh
NET_DEMO=http-server NET_PRIVATE_RUNTIME=1 BANK_SIZE=16384 \
  scripts/build-net-ping.sh native build/network/http-server-wire/native \
  -DHTTP_SERVER_DHCP=0 -DHTTP_SERVER_TIMEOUT=15000UL
NET_DEMO=http-server NET_PRIVATE_RUNTIME=1 BANK_SIZE=16384 \
  scripts/build-net-ping.sh rvm build/network/http-server-wire/rvm \
  -DHTTP_SERVER_DHCP=0 -DHTTP_SERVER_TIMEOUT=15000UL
scripts/test-net-http-server.py
scripts/test-net-http-server.py --mode rvm --jit
```

All five cases (`success`, `loss`, `reorder`, `zero`, `malformed`) pass on native,
RVM interpreter and JIT. The peer checks outgoing checksums, MSS 113, wrapped
request sequences and exact headers/984-octet HTML. Loss drops the first SYN-ACK,
response data, data ACK, FIN and last ACK; retransmitting the peer's FIN after
pool release must get an ACK from compact TIME_WAIT state. Reordering injects
future request bytes before the expected prefix. A zero window requires actual
one-octet probes before opening a small window. A corrupt SYN cannot create a
connection. The longer 15-second HTTP phase budget is fixture configuration
for the combined loss scenario; the normal application keeps five-second phases.

Actual macOS host checks use the default DHCP-enabled builds and capture-disabled
host-only vmnet. The host connects to the leased address printed by the guest:

```sh
scripts/run-net-http-server.sh --test
scripts/run-net-http-server.sh rvm --jit --test
# Reuse those builds for the interpreter/individual modes:
scripts/test-net-http-server-live.py --mode native
scripts/test-net-http-server-live.py --mode rvm --jit
```

Native, RVM interpreter and RVM JIT pass curl plus 18 named HTTP/stream checks, with 26 completed
connections per run. This exceeds the four-live-connection table without
aborting successful closes. The checks compare the exact hardcoded page,
Content-Length, Content-Type, Connection and Allow fields, then cover split and
half-closed requests, 404/405, missing/duplicate Host, invalid/nonzero lengths,
chunked requests, Expect, oversized header/target, unsupported version, early
EOF, request stall, withheld client FIN, reset and eight further valid requests.
The live stall caught a TCP idle deadline racing the 408 response: the demo now
allows 15 seconds at TCP while keeping a five-second HTTP request deadline.
Timeouts are explicit and ordinary subsequent requests must still succeed.
Guest processes and owned helpers are removed after every test.

A real browser also rendered the native page from its DHCP address, including
its title, text and inline CSS; the browser's favicon request received 404.
No host web server supplied that page. RBF was not built/run for this milestone,
and no public HTTP service or NAT port mapping was configured. The shared C
implementation and original ISA remain the intended execution path on RBF.

## Live host and VPS tests

On macOS, after building the monitor variants above:

```sh
scripts/test-net-auto.py --jit
```

This uses ordinary runtime-owned host-only vmnet attachment, pings the guest
from the actual host OS, checks guest logs and verifies helper cleanup after
abrupt termination. The native executable is also copied outside the repository
to check that its embedded helper is sufficient. See [Build and run](running.md)
for alternate static IPs when another guest already owns `.250`.

For the complete guest DNS/Internet milestone:

```sh
scripts/run-net-dns.sh native syma-lang.org
scripts/run-net-dns.sh rvm syma-lang.org --jit
```

These select shared-mode vmnet/NAT with packet capture disabled. Record the
printed DNS server, guest-resolved answer/TTL, three validated echo replies and
NIC counters. Do not bake a current VPS address into the test or pre-resolve
it on the host. External DNS, routing or VPS availability failures must be
reported separately from a local fixture failure. The user's VPS is the
agreed live target; other public endpoints are not required for routine tests.

Manual helper attachment, `test-net-live.py` and the IPv4-only
`test-net-vps.py` remain available for isolating transport/routing problems;
see [manual attachment](running.md#bounded-protocol-tests-and-manual-attachment).
No full Doom/RBF timing run is part of networking acceptance.

## Evidence and timing

Existing fixtures save logs and JSON results under:

| Test | Output directory |
|------|------------------|
| One-shot packet smoke | `build/network/<mode>/` |
| Monitor packet and automatic live tests | `build/network/monitor/<mode>/` |
| DNS packet tests | `build/network/dns/<mode>/` |
| Isolated lifecycle/DHCP C test builds | `build/network/reliability/` |
| DHCP packet tests | `build/network/dhcp/<mode>/` |
| DHCP live monitor and helper cleanup | `build/network/dhcp-monitor/<mode>/` |
| DHCP → DNS → ping launcher/build | `build/network/dhcp-ping/<mode>/` |
| TCP C regressions / independent packet peer | `build/network/tcp/` |
| TCP live host echo | `build/network/tcp-echo/<mode>/` |
| HTTP private parser/client units | `build/network/http-unit/` |
| HTTP independent guest DNS/TCP peer | `build/network/http-dns/<mode>/` |
| HTTP live service/launcher | `build/network/http-get/<mode>/` |
| HTTP server independent wire peer | `build/network/http-server-wire/<mode>/` |
| HTTP server live curl/browser/host tests | `build/network/http-server/<mode>/` |
| Earlier IPv4-only VPS test | `build/network/vps/<mode>/` |

The ICMP packet fixture also writes generated-test captures. This does not
change the policy that automatic/live vmnet attachment has capture disabled.
Build outputs are disposable, not checked-in proof that every future revision
passes. Keep command/mode, configuration, timeout, outcome and relevant counters
with test results; retain packet traces only when intentionally requested for
that test environment.

NIC queries and repeated empty polls must not advance guest time. DHCP/DNS use a
caller-supplied monotonic 32-bit millisecond clock; the live demo explicitly
paces tick grants. Controlled protocol tests should also advance time directly
when testing retries and wraparound. Real peers continue using wall time while
a slow guest computes; queues cannot make RBF arbitrarily fast or suspend a
remote peer's deadlines.

Disconnected/controlled NIC state can be checkpointed; an attached live NIC
cannot claim exact replay. A controlled-time recorded packet replay format is
future work, not a feature supplied by the current socket fixture.

## Acceptance policy for the roadmap

[Each planned milestone](roadmap.md) needs focused deterministic failure cases
before depending on a live peer. Keep a separate real-host or controlled-host
integration check so a test oracle cannot accidentally become the only peer
that understands the implementation. The HTTP server milestone specifically
requires a normal host browser or `curl` to fetch guest-generated HTML.

Repeat relevant existing ping, UDP and DNS regressions as layers are added.
Record resource limits and test buffer exhaustion/reuse, partial progress,
malformed input and deadlines. Do not accept a new protocol based solely on
one happy-path request or on host-side substitution for guest code.


## RFL library (17 October 2026)

```sh
python3 scripts/test-rfl-network.py
python3 scripts/test-rfl-network.py --mode rvm --jit --skip-build
MAKEFLAGS=-j4 ./rct test_rfl_net_ownership test_net_tcp_retire --backend rvm --timeout 90
cargo test --release -p rfl --test c_libraries
```

The [RFL library guide](rfl-library.md) describes the API and remaining limits.
The controlled packet tests passed separately on native, RVM interpreter and
RVM JIT: guest DNS → HTTP with a 10,230-octet body/checksum, DHCP → acquired DNS
→ HTTP, and six successive server connections with loss, reordering, zero
windows and malformed input. Server metadata and response bytes are checked.
These are RFL applications using guest protocols, not C-only coverage. Build
and execution logs are in `build/network/rfl-http/<mode>/`. The normal DHCP
examples additionally demonstrate automatic runtime NIC initialization; live
RFL browser/curl interoperability is not claimed by these packet fixtures.

The fake-NIC bridge regression passes 35 assertions on native and RVM; the new
TCP retirement regression passes 15 on both. They cover leases surviving driver
shutdown, capacity/reuse, DNS cleanup, refcount overflow, unfinished DHCP,
stale listener generations, compact TIME_WAIT backpressure and duplicate FINs.
The RFL import regression covers translation-unit static function/callback
isolation and public global linkage. No routine RBF network run was added.

Two compiler defects encountered while building the bridge have registered,
correct-behavior RED regressions in `c-test/tests/bugs/`:
`test_conditional_null_pointer_return` and `test_call_many_u32_arguments`.
The first rejects a pointer/null conditional on return; the second exhausts
pinned registers for a large non-inlined u32 call. The library uses an explicit
branch and a compact options-array ABI respectively. Those tests remain open
compiler work; the protocol/bridge passes do not claim they were fixed.


The complete RFL release suite passed **359 tests, zero failures** with
`CARGO_BUILD_JOBS=2 cargo test --release -p rfl --tests --quiet -- --test-threads=4`.
The existing 193 C HTTP/listener assertions were also repeated successfully on
native and RVM, for 243 networking assertions per target including the new
bridge/retirement cases. The retained-reactor callback check passed on native,
RVM and JIT after releasing its network and driver owners.
