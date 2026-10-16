# Neighbor and operation lifecycle

The shared guest stack now handles neighbor expiration, bounded ARP retries,
link transitions, queued-send cancellation and matched ICMP errors. These use
ordinary guest C and the existing DEVICE_* NIC; no protocol work moves to the
host. The [TCP client](tcp.md) uses the same lifecycle and validated errors.

## Drive time and observe the link

Call `net_poll(context, now_ms)`, or `net_dhcp_poll` while DHCP owns the context,
throughout the application's lifetime. Supply wrapping 32-bit milliseconds
and service regularly, with less than a full clock wrap between calls. NIC
queries do not advance time. Live monitors and DNS/DHCP launchers grant paced
ticks; applications using a different clock supply it explicitly.

`net_open` can initialize a disconnected NIC. Static `net_init` can likewise
install configuration while disconnected, without announcing an address.
Sends/polls return `NET_LINK_DOWN`; this is distinct from `NET_IO_ERROR`, which
reports a malformed device response, failed mailbox call or backend operation
that cannot be treated as an ordinary link transition.

The stack refreshes NIC information at most once per second while up and once
per 250 milliseconds while down. TX/RX reporting LINK_DOWN detects loss sooner,
including while a pending frame prevents normal receive progress. Detection
clears ARP, recent-send history, pending TX and stale echo/error events. UDP
bindings survive. `network.link` records current state and `network.link_epoch`
changes on loss, restoration or a changed NIC MAC. Events LINK_DOWN/LINK_UP are
transient; observe the epoch for durable detection even if a packet event
replaces a link event in the same poll.

Static configuration remains installed across an outage. Restoration refreshes
the MAC, announces that static address and resolves neighbors again. DHCP
instead removes its lease immediately, enters WAIT_LINK and starts fresh
acquisition after restoration. Offline waiting does not consume its acquisition
budget. It does not reuse a lease on a potentially different network.

This recovers guest state when the backend reports restoration. It does not
restart a failed host helper or add reconnection to an external transport.
Fatal backend errors remain visible to the application.

## ARP cache and failed neighbors

The cache still has four entries. Learned mappings expire after 60 seconds;
expiration removes readiness and the next send resolves the next hop again.
A validated ARP request/reply directed to the guest can refresh a known peer's
MAC and lifetime. No heap, per-peer background worker or unsolicited learning
of arbitrary neighbors is used.

With a caller retrying a send and polling normally, an unanswered lookup has:

| Time from first accepted probe | Action |
|-------------------------------|--------|
| 0 seconds | First ARP request |
| 1 second | Second request |
| 3 seconds | Third request |
| 7 seconds | Mark failed; sends report `NET_UNREACHABLE` |
| Five seconds after observed failure | A new send may begin another lookup |

These are minimum intervals, not deadlines that force background traffic.
Each retry requires another application send attempt. Delayed polling delays
progress; it does not produce a burst of missed probes. A late valid reply can
recover a failed entry before cooldown ends. Successful learning resets retry
state, and full caches reuse slots within the same fixed bound.

Local TX backpressure does not count as a missing remote ARP reply. A queued
probe's response timer starts when the host actually accepts that frame.
`net_arp_invalidate(net, next_hop)` explicitly removes a neighbor and drops a
pending frame using its old MAC or ARP target. Passing null invalidates all
neighbors and discards any pending frame. Already accepted host frames cannot
be recalled. Normal configuration/link changes use shared invalidation too.

## Pending sends and operations

There is one pending frame per context. `NET_OK` from a send means a copy was
accepted by the context or host; `NET_AGAIN` means retry the original operation
after polling. Neither promises remote delivery. RX waits while the pending
frame cannot be flushed, but link checks and protocol timers continue.

Changing IP/mask/router/DNS advances `network.generation`; identical renewal
configuration is a no-op. Link transitions advance `link_epoch` separately,
allowing DHCP to distinguish an outage from explicit application replacement
of its configuration. Operations tied to an old configuration or link must
finish or restart explicitly. The library does not replay application payloads
after an outage, which could duplicate an operation.

A new UDP binding forgets that port's earlier unbound transmissions, so it
cannot inherit a previous operation's errors. `net_udp_unbind` forgets the
port's recent transmissions and discards its
pending UDP frame. DNS completion/cancellation uses this path; a retry also
forgets the previous attempt before issuing a new transaction. Accepted host
frames are still outside guest cancellation's reach.

DNS reports `NET_DNS_CONFIG_CHANGED` for configuration changes and
`NET_DNS_LINK_DOWN` for an outage/MAC change, even if the link has already
returned by the time the application services the lookup. It releases its
binding in either case. Start a new query after usable configuration returns.

## ICMP error matching

The stack handles destination-unreachable (type 3), time-exceeded (11) and
parameter-problem (12) messages. It validates the outer checksum and source,
then the quoted IPv4 header, checksum, source address, fragment flags and the
first eight transport octets. Quotes can be shorter than the original full
datagram, as ICMP requires. Our emitted IPv4 headers currently have no options.

An error must match a recently accepted unicast transmission: local address,
destination, protocol, IPv4 ID, total length and all eight quoted transport
octets. For UDP those include both ports, length and checksum. Routers may
change the quoted TTL/header checksum. A router's reporting IP need not equal
the remote application's IP. Matching consumes the record, so duplicates do
not notify twice. Unmatched, evicted, expired or cancelled records cannot
cancel another operation.

History is limited to eight accepted packets and 30 seconds; expired records
are pruned during normal polling. Packets merely waiting for local TX space,
DHCP broadcasts and ICMP replies/errors are not eligible. This is bounded
correlation, not cryptographic authentication. A busy application may receive
no notification for an older evicted packet and must still maintain deadlines.

A matched error sets `NET_EVENT_ICMP_ERROR` and populates `network.error` with
reporter, original destination/protocol/ID, type/code, UDP/TCP ports or echo
identifier/sequence, and the advertised next-hop MTU for type 3/code 4. To receive
UDP-specific notifications, attach an optional handler to an existing binding:

```c
static void failed(NetContext *net, const NetError *error, void *user) {
    /* Synchronous callback: copy fields if needed after the next poll.
       The user pointer is the existing UDP binding's user pointer. */
}

int enable_errors(NetContext *net) {
    return net_udp_on_error(net, 9000, failed);
}
```

Do not recursively poll the context from callbacks. Passing a null handler
disables it; unbinding removes it. Echo applications inspect the context event.
TCP consumes the same validated quote information. Protocol/port-unreachable
terminates a stream; fragmentation-needed and parameter problems report a path
error. Network/host-unreachable and TTL errors remain soft diagnostics while
retransmission and progress deadlines continue. See [TCP lifecycle](tcp.md#timers-windows-and-lifecycle).

DNS installs its own handler. Type 3 finishes with `NET_DNS_UNREACHABLE`;
types 11/12 use `NET_DNS_PATH_ERROR`. `NetDns.icmp_type`, `icmp_code` and `mtu`
retain diagnostics. Fragmentation-needed is reported, not silently retried
with truncated payloads. No automatic path-MTU adaptation, IP reassembly, ICMP
redirect/source-quench handling or general outbound ICMP error generation is
included in this milestone.

## Counters and validation

`NetContext.counters` contains saturating 32-bit counters for ARP queries,
expired mappings, failed lookups, learned updates, local TX backpressure,
discarded pending frames, observed link transitions, matched ICMP errors and
unmatched quotes. These complement the host queue statistics from NET_STATS;
they neither reset counters nor allocate per-packet log records. One MAC change
while up counts as a new up epoch, without inventing an observed down event.

See [Testing](testing.md#stack-reliability) for fake-clock lifecycle coverage,
independent malformed/stale ICMP packet fixtures and the live host checks.
Protocol references: [ICMP, RFC 792](https://www.rfc-editor.org/rfc/rfc792.html)
and [IPv4 host requirements, RFC 1122](https://www.rfc-editor.org/rfc/rfc1122.html).
