# Guest networking library

`<net.h>`, `<net_dns.h>`, `<net_dhcp.h>`, `<net_tcp.h>`, `<net_http.h>` and `<net_http_server.h>` are part of `runtime/libruntime.par`.
They implement Ethernet, ARP, IPv4, ICMP echo, UDP, DHCP, a small DNS A
resolver, active/passive TCP streams, an HTTP GET client and an HTTP server
in guest C. The host moves raw Ethernet frames through [DEVICE_*](device.md); it never
resolves guest names or supplies guest TCP/IP. Each wire octet occupies one
16-bit Ripple cell, including zero and 255.

For runnable examples and the demo's build-time hostname configuration, see
[Build and run](running.md). Tests and recorded validation are in
[Testing](testing.md). This page documents the current C API; future APIs are
not specified here.

## Initialize a context

The application owns all memory; the library allocates no heap and has no
hidden global packet buffers. A context contains two 1514-cell frame buffers,
four ARP entries, four UDP bindings and eight recent transmission records. Allocate it in static storage if the
stack is small. `net_init` discovers the MAC, checks the link/MTU, initializes
the context and sends a gratuitous ARP announcement when the link is up.
Initialization also works while disconnected; sends then return `NET_LINK_DOWN`.
Use `net_init` for explicit static configuration, as below. For dynamic
configuration, use `net_open` and the [DHCP client](dhcp.md); continue driving
`net_dhcp_poll` in place of `net_poll` throughout the application lifetime.
The following example uses static configuration:

```c
#include <net.h>

static NetContext network;
static NetConfig config = {
    0,                      /* NIC */
    {192, 168, 64, 250},     /* our address */
    {255, 255, 255, 0},      /* subnet mask */
    {192, 168, 64, 1},       /* default gateway */
    {1, 1, 1, 1},           /* recursive DNS server */
    12345                   /* caller-provided random seed */
};

/* Check status before using the context. */
int initialize(void) { return net_init(&network, &config); }
```

The illustrative seed is fixed for reproducibility. Supply entropy appropriate
to the application for query IDs and source ports. The small PRNG is not
cryptographically secure; tuple/ID/question matching does not provide DNSSEC
or protection equivalent to a hardened general-purpose resolver.

`net_poll(&network, now_ms)` checks link/timers, flushes a pending transmission, then handles at
most one frame. It automatically answers valid ARP and ICMP echo requests and
delivers UDP callbacks. Invalid IP/ICMP/UDP packets return `NET_REJECTED` and
increment `network.rejected`; they are ordinary rejected traffic, not fatal I/O
errors. `NET_AGAIN` means no packet or transient TX backpressure. `NET_LINK_DOWN`
is a recoverable link state; `NET_IO_ERROR` reports a failed device operation.
See [reliability and lifecycle](reliability.md) for recovery and ICMP errors.
Echo events expose peer, identifier,
sequence and payload until the next poll.

Serialize access to a context. Callbacks execute synchronously inside
`net_poll`; never recursively poll the same context. Other device commands
may be used between calls, since the mailbox transaction has already completed.

## Configuration changes

`net_open` initializes an unconfigured NIC without announcing an address.
`net_configure` validates and installs a configuration; `net_unconfigure`
removes it. Changed IP, mask, gateway or DNS increments `network.generation`,
clears ARP, drops the context's pending TX and clears stale echo events. UDP
bindings survive; frames already accepted by the host cannot be recalled.
Identical configuration is a no-op, so an unchanged DHCP renewal preserves
in-flight operations. Do not reset an active context with `net_init`/`net_open`;
stop its protocol operations before reinitializing it.

Active DNS work observes the generation and finishes with
`NET_DNS_CONFIG_CHANGED` on a change. Applications must likewise invalidate
other operations that depend on the old configuration. Explicit reconfiguration
terminates DHCP ownership on its next poll while preserving the replacement IP.

## Receive and send UDP

```c
static unsigned short packets;

static void receive(NetContext *net, const NetDatagram *packet, void *user) {
    unsigned short *count = user;
    ++*count;
    /* packet->source[4], source_port, destination_port, length and data
       describe one validated datagram. Copy data here if needed later. */
}

int bind_port(void) {
    return net_udp_bind(&network, 9000, receive, &packets);
}

int send_packet(void) {
    unsigned char destination[4] = {192, 168, 64, 1};
    unsigned char payload[3] = {0, 255, 7};
    return net_udp_send(&network, destination, 9000, 9001, payload, 3);
}
```

Ports are unsigned 16-bit values in host integer order. Destination port zero,
null nonempty payloads, and payloads above 1472 octets are rejected. Empty UDP
payloads are supported. `NET_NO_PORT` reports an occupied port or full binding
table; `net_udp_unbind` releases a binding and cancels its pending UDP frame and
recent ICMP correlation history. Unbound incoming ports are ignored. Attach
`net_udp_on_error` to receive [matched ICMP notifications](reliability.md#icmp-error-matching).

Sending may first require ARP for the destination or default gateway. On
`NET_AGAIN`, keep polling and retry the original send. `NET_OK` means the
context or host queue owns a copy, so the caller may reuse its payload. It does
not mean remote delivery. A full host queue leaves one pending frame in the
context; polling retries it before consuming another RX frame. Inspect
`vm_net_stats` for asynchronous host errors and drain accepted TX before exit.

Outgoing UDP always includes a pseudoheader checksum; a computed zero is
encoded as 65535. Incoming nonzero checksums are verified, and IPv4 UDP's zero
checksum is accepted. UDP length is bounded by the validated IP payload;
Ethernet padding is excluded. IPv4 fragments are rejected rather than partially
interpreted. UDP offers no retransmission or delivery guarantee. General-purpose
broadcast/multicast is unsupported; DHCP alone has a limited-broadcast path
for ports 68 → 67 and bootstrap replies from port 67.

## Resolve a name

```c
#include <net_dns.h>
#include <vm_devices.h>

static NetDns lookup; /* zero before first use */

int resolve(void) {
    VmClock clock;
    if (vm_get_time(&clock)) return NET_DNS_NETWORK_ERROR;
    if (net_dns_start(&network, &lookup, "syma-lang.org", clock.ms,
                      3000UL, 2) != NET_OK)
        return NET_DNS_NETWORK_ERROR;
    while (lookup.status == NET_DNS_WAITING) {
        if (net_poll(&network, clock.ms) == NET_IO_ERROR) {
            net_dns_cancel(&lookup);
            return NET_DNS_NETWORK_ERROR;
        }
        net_dns_poll(&lookup, clock.ms);
        if (lookup.status == NET_DNS_WAITING && vm_next_tick(&clock)) {
            net_dns_cancel(&lookup);
            return NET_DNS_NETWORK_ERROR;
        }
    }
    /* On SUCCESS, lookup.address[4] and lookup.ttl hold the result. */
    return lookup.status;
}
```

Use `rvm --clock paced` or `RCC_NATIVE_PACED=1` for live networking with the
example's logical clock. `GET_TIME` and NIC commands do not advance time;
`NEXT_TICK` does. Unthrottled ticks can exhaust a timeout before real packets
arrive. The library accepts any caller-supplied monotonic 32-bit millisecond
clock and handles unsigned clock wrap; it never estimates time from CPU loops.
Controlled peers can advance that clock explicitly for deterministic tests.

`net_dns_start` validates an ASCII hostname (labels up to 63 characters, total
up to 253, optional trailing dot), reserves an ephemeral UDP port and initializes
a query. `timeout_ms` is 1..60000 per attempt, including address discovery;
`retries` is 0..4 additional attempts. Retries use a fresh transaction ID.
`net_dns_poll` submits work and handles deadlines; receiving still requires
`net_poll` (or `net_dhcp_poll` for an active DHCP client). Completion and
`net_dns_cancel` release the UDP binding. Reuse a
completed object, or keep separate objects for concurrent queries within the
four-binding limit. Do not move an active object: its callback points to it.

Replies must match server address, UDP ports, ID, standard response header,
question name, A type and IN class. Parsing validates every record's bounds,
compressed owners and CNAME targets before accepting an answer. Compression
pointers must refer backward within the message; expanded names and traversal
are bounded. The resolver follows at most eight CNAME links in the answer
section independent of record order, ignores unrelated addresses, and returns
the minimum TTL along the selected chain. Malformed matched replies are counted
in `lookup.malformed` and ignored while the query waits or retries.

Statuses distinguish success, NXDOMAIN, no matching A address, server error,
truncation, timeout, network failure, cancellation, configuration/link changes
and matched ICMP errors; constants live in
[`net_dns.h`](../../runtime/include/net_dns.h). This first version uses classic
512-octet UDP DNS. It has no EDNS, TCP fallback, AAAA, DNSSEC or cache. A
truncated reply fails explicitly; a CNAME-only answer without its terminal A
record returns `NET_DNS_NO_ADDRESS` rather than starting another lookup. The
configured recursive server normally supplies both. ARP uses a small expiring
table with bounded retries and cooldown recovery. Configuration/link changes
clear it; [the lifecycle guide](reliability.md) documents timers and counters.
IPv4 reassembly, multicast and DNS-over-TCP fallback remain separate work.

## TCP streams

See the [TCP client guide](tcp.md) for the API, partial reads/writes, EOF,
half-close, timeouts and connection ownership. Each caller-owned `NetTcp` has
512-octet TX/RX buffers; a context can register four connections. Drive the
network poll and then each active TCP connection. TCP does not receive NIC
frames independently.

`net_tcp_connect` enables TCP dispatch for its context. `net_tcp_enable` enables
closed-port reset handling without opening a connection. Applications that use
neither function do not link the TCP implementation: base IPv4, ARP and ICMP
code call an optional internal driver rather than directly referencing TCP.

## Passive TCP and HTTP server

`net_tcp_listen`, `net_tcp_listener_poll`, `net_tcp_accept` and
`net_tcp_release` expose caller-owned bounded pools over the same stream core.
The context supports two listeners and four live connections. Accepted pointers
remain claimed through failure/close until explicit release. An optional
caller-owned `NetTcpWaitTable` preserves 32 compact old tuples while releasing
large TIME_WAIT buffers; no ordinary ping/DNS application allocates that table.
See [TCP ownership](tcp.md#listen-and-accept) for backpressure and teardown.

`<net_http_server.h>` supplies a standalone incremental request decoder,
response serializer and polling session. The application chooses routes and
immutable HTML; the library manages split requests, partial writes, framing,
errors and deadlines. Drive network, listener/TCP, then HTTP session progress.
The session borrows its TCB and leaves pool release to the application. See the
[server guide](http-server.md) and runnable `http-server.c` example.

## HTTP GET

See the [HTTP client guide](http.md) for `NetHttp`, partial binary body reads,
guest DNS, response status/framing, explicit limits and total deadlines. Its
standalone decoder can be used without TCP. Network/DHCP polling still comes
first, followed by HTTP polling and body reads. Body EOF does not release an
active TCP close/TIME_WAIT: keep polling or explicitly cancel before freeing
the object. No host sockets or HTTP implementation replace the guest stack.

## Linking

Ripple builds link the application object and `crt0.pobj` with
`runtime/libruntime.par`. Archive members are extracted only to satisfy symbol
dependencies. Compiler metadata allows unreachable functions, initializers and
global reservations to be removed from executable links. Initializer writes
retain their owning data symbol, so retained arrays, pointers and callbacks
relocate together. Legacy or opaque fixed-layout objects remain conservative.
A link without an entry point preserves exports. Merely linking the runtime
does not pull every translation unit's global storage into the program.

For native compatibility builds, `scripts/build-net-ping.sh native` compiles
the same protocol C sources to native IR alongside the application's IR and
links them with `rcc link-native`; no host networking library replaces them.

See [Testing](testing.md) for regression commands and the independent packet
fixtures. The protocol implementation remains guest C on every execution host.

Protocol references: [UDP (RFC 768)](https://www.rfc-editor.org/rfc/rfc768.html),
[DNS format (RFC 1035)](https://www.rfc-editor.org/rfc/rfc1035.html), and
[DNS reply matching (RFC 5452)](https://www.rfc-editor.org/rfc/rfc5452.html).
