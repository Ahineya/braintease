# Networking

Ripple networking exposes a raw Ethernet NIC through the existing `DEVICE_*`
mailbox. Guest C implements the protocols in `runtime/libruntime.par`; RVM,
RBF and the native runtime provide the same host device interface. There are
no host socket or hostname-resolution shortcuts in the guest protocol stack.

**The original milestone is complete:** bidirectional host ping, followed by
guest DNS resolution of `syma-lang.org` and ping to its returned IPv4 address.
The complete DNS-to-ping exchange was validated on RVM and native. Earlier
ICMP/device milestones were also validated on RBF; new DNS execution there
remains opt-in. DHCP now supplies guest configuration too, including renewal,
rebinding, expiry and address conflict checks; see the [DHCP guide](dhcp.md).

## Guide

| Page | Contents |
|------|----------|
| [Build and run](running.md) | Ping monitor, DNS/ping launcher, configuration, host backends and troubleshooting setup |
| [HTTP server](http-server.md) | Guest TCP listen/accept, hardcoded HTML, browser/curl and bounded request handling |
| [HTTP client](http.md) | Streaming GET, bounded response parsing, guest DNS and the automatic host HTTP demo |
| [Terminal browser plan](terminal-browser.md) | FUZIX/Fuse-X browser feasibility, HTTP targets, Lynx/Links decisions, curses and porting gates |
| [Local IPC and capacity](local-ipc.md) | Guest Unix stream sockets, socketpair, lifetime, readiness and larger TCP pools |
| [select contract](select.md) | Implemented descriptor readiness, C16 ABI, graphics/PTY/socket integration and tests |
| [TCP client](tcp.md) | Streams, retransmission, flow control, half-close and the automatic host echo demo |
| [DHCP](dhcp.md) | Lease acquisition, renewal/rebinding, conflict detection, configuration changes and examples |
| [Reliability and lifecycle](reliability.md) | Neighbor expiration/retry, link changes, pending sends and matched ICMP errors |
| [Guest library](library.md) | Current C APIs, ownership, UDP callbacks, DNS, timeouts and linking |
| [Device contract](device.md) | DEVICE_* sharing, descriptor layouts, frame queues, transport and checkpoints |
| [Testing](testing.md) | Reproducible commands, existing coverage, recorded validation and evidence requirements |
| [RFL library](rfl-library.md) | Owned DHCP/DNS, network reactor, async HTTP requests/responses and HTML serving |
| [RFL async design](rfl.md) | Implemented traits, polling, ownership, composition, task scopes and async/await |
| [Roadmap](roadmap.md) | DHCP → stack reliability → TCP → HTTP client/server → RFL, with acceptance criteria |

## Try it

From the repository root, with the release toolchain built:

```sh
scripts/run-net-http-server.sh
scripts/run-net-http.sh
scripts/run-net-tcp.sh
scripts/run-net-dhcp.sh --monitor
scripts/run-net-dhcp.sh rvm syma-lang.org --jit
```

The HTTP server prints its leased URL for your browser and keeps serving the
guest page. The HTTP client demo acquires a lease and streams a 13,033-octet HTML page from a
temporary ordinary host HTTP service. The TCP demo exchanges 4,097 binary octets with a temporary
ordinary host TCP echo service. The monitor defaults to native execution with
host-only Ethernet. The DNS/ping
launcher explicitly selects shared-mode vmnet/NAT. On macOS the runtime starts
its embedded helper on the first NIC query; only the helper requires sudo.
Capture is disabled in these automatic launchers. See [Build and run](running.md)
for prerequisites, native execution and selecting another guest IP.

## Current implementation

| Layer | Implemented behavior | Main limitation |
|-------|----------------------|-----------------|
| NIC/device | Discover MAC/link/MTU; TX/RX queues; statistics; synchronous guest-memory copies | One interface, Ethernet MTU 1500 |
| Host attachment | Lazy macOS host-only or shared vmnet; external Unix-datagram peer | No bundled Linux TAP helper |
| Configuration / DHCP | Static `NetConfig` or guest DHCP acquisition, renewal, rebinding, expiry and ARP conflict detection | One lease per context; no saved-lease restore or DHCPRELEASE |
| Ethernet/ARP | Four-entry expiring cache, bounded retry/backoff, MAC refresh and explicit invalidation | Fixed 60-second lifetime; no arbitrary unsolicited neighbor learning |
| IPv4 | Local unicast and default-gateway routing, bounded parsing/checksums | No fragment reassembly; broadcast restricted to DHCP bootstrap |
| ICMP | Echo and matched unreachable/time-exceeded/parameter errors; UDP callbacks, DNS and TCP diagnostics | Eight recent accepted packets, 30-second history; no path-MTU adaptation |
| UDP | Four bindings, callbacks, checksums, empty/binary payloads and backpressure | Payload limit 1472 octets; no reliability guarantee |
| DNS | IPv4 A queries, compressed names, bounded in-answer CNAME chains, matching, retries and timeouts | Classic 512-octet UDP; no TCP fallback, EDNS, AAAA or cache |
| TCP | Active/passive opens, bounded pools, retransmission, flow control, half-close and compact TIME_WAIT | Four live connections, two listeners, 512-octet buffers, one segment in flight |
| HTTP | Streaming GET client and bounded single-request server, incremental parsing and exact response lengths | Plain HTTP; no request bodies, chunking, TLS, persistent connections or redirect following |
| RFL | Owned network reactor, DHCP/DNS futures, buffered Request/Response and async HTML serving | No direct RFL UDP/TCP streams or streaming HTTP bodies yet |

The DNS demo's hostname is compiled into the guest with `DNS_NAME`; the
launcher rebuilds it when a different name is supplied. The guest resolves
that name itself. Runtime hostname input is separate from DNS resolution and
is [deferred](roadmap.md#separate-and-later-work).

## Architecture and constraints

```text
application (DHCP/DNS/ping/TCP/HTTP client and server)
    ↕ guest protocol libraries in libruntime.par
    ↕ vm_network.h: NET_INFO / NET_TX / NET_RX / NET_STATS
    ↕ existing DEVICE_* mailbox, bank 0 cells 27–31
host-owned bounded frame queues
    ↕ Ethernet backend (macOS vmnet or a controlled packet peer)
host network / external network
```

Keep these decisions when extending the stack:

- Use the same DEVICE_* mailbox and ordinary instructions. RBF's VM executes
  in Brainfuck with 16-bit cells; networking must not depend on new ISA or
  native-only protocol execution.
- Store one wire octet per guest cell. Explicitly serialize network headers;
  C structures and the 16-bit C byte do not define the wire layout. Avoid
  packing two octets into a cell: the BF CPU makes bit operations expensive.
- Host workers own only host buffers/queues. They cannot access guest RAM,
  update old descriptors or overwrite another device's mailbox status.
- Keep protocol memory bounded and caller-owned. Polling and callbacks work
  in freestanding applications; rOS integration must not require a scheduler,
  daemon or other unfinished OS infrastructure.
- Supply time explicitly. NIC polls do not advance logical time; live examples
  use paced ticks, while controlled tests can drive a simulated clock.
- Keep native and VM behavior aligned. Native execution can run the same C
  faster, but does not replace guest TCP/IP with host sockets.

DHCP and the [stack reliability milestone](reliability.md) are implemented.
The [active-open TCP client](tcp.md) and [streaming HTTP client](http.md) are
implemented too, together with passive TCP listeners and the
[HTTP server serving hardcoded HTML](http-server.md). The first high-level
[RFL library](rfl-library.md) now supplies owned DHCP/DNS and async HTTP
client/server wrappers over that stack. Direct RFL UDP/TCP, streaming bodies
and richer HTTP remain follow-ups.
