# Guest DHCP client

`<net_dhcp.h>` adds guest DHCP to `libruntime.par`, using the same Ethernet,
IPv4 and UDP stack as static networking. The host only carries frames; it does
not hand the guest a decoded lease. No new DEVICE_* command or instruction is
needed. Static `NetConfig` applications remain supported.

## Run it

From the repository root with the release toolchain built:

```sh
scripts/run-net-dhcp.sh
scripts/run-net-dhcp.sh rvm syma-lang.org --jit
scripts/run-net-dhcp.sh --monitor
```

The first two acquire a lease, use its DNS server to resolve the compiled-in
hostname, then validate three ping replies. They explicitly select shared-mode
macOS vmnet/NAT with capture disabled. The monitor uses host-only vmnet, prints
its leased configuration and stays alive answering/logging pings while renewing
its lease. Only the helper needs sudo; the guest runs as your normal user.

Example output (addresses and lease duration come from the server):

```text
DHCP acquiring
DHCP bound address=<leased IPv4> mask=<mask> gateway=<router> dns=<DNS server> lease=<seconds>
DNS query name=syma-lang.org local=<leased IPv4> server=<leased DNS server>
DNS answer name=syma-lang.org address=<guest-resolved IPv4> ttl=<seconds>
PING reply seq=1 octets=17
PING reply seq=2 octets=17
PING reply seq=3 octets=17
```

Ping the monitor's printed address from the host to generate incoming traffic.
You do not need to select an unused static IP or configure the host interface
manually. `--socket PATH` reuses a supplied Ethernet peer; `--host-only` overrides
attachment mode; `--build-only` compiles without starting a network. RBF remains
an explicit launcher option, with no automatic heavy BF acceptance run.

Builds live under `build/network/dhcp-ping/<mode>/` or
`build/network/dhcp-monitor/<mode>/`. The launchers set `NET_PRIVATE_RUNTIME=1`
to compile protocol dependencies inside that output directory, avoiding writes
to another task's shared runtime build products. The same C modules are included
in the ordinary runtime archive by its source rules. The hostname is still a
build-time `DNS_NAME` setting; DHCP supplies network configuration, not the
application's desired domain name.

## API and lifetime

```c
#include <net_dhcp.h>

static NetContext network;
static NetDhcp lease; /* zero before first use; never move while active */

int initialize(unsigned long now_ms) {
    int result = net_open(&network, 0, 12345);
    if (result != NET_OK) return result;
    return net_dhcp_start(&network, &lease, now_ms, 60000UL);
}

int service(unsigned long now_ms) {
    int result = net_dhcp_poll(&lease, now_ms);
    if (lease.state == NET_DHCP_FAILED) return lease.error;
    /* Once BOUND, network.config contains the active configuration.
       Continue servicing it throughout DNS/ping/application activity. */
    return result;
}
```

`net_open` discovers and initializes an unconfigured NIC without announcing an
address. Ordinary application sends remain unavailable until configuration is
applied. The fixed seed in the example is illustrative; the application supplies
entropy for transaction IDs and retransmission jitter. The PRNG and DHCP itself
do not authenticate a server on an untrusted network.

`net_dhcp_start` reserves UDP port 68 and starts acquisition. `timeout_ms` is
1..600000 and bounds an acquisition, including conflict checks. DHCP waits a
random 1..10 seconds before initial discovery, then uses bounded exponential
retransmission delays. Set a sensible overall deadline; a very short deadline
can expire before the first discovery. The first acceptable OFFER is selected;
there is no hidden static fallback when acquisition fails.

Call **`net_dhcp_poll` in place of `net_poll`** for the client's entire lifetime,
including after the first BOUND event. It advances lease timers before receiving
one frame, then drives DHCP progress. It returns a `NET_*` polling result;
inspect `lease.state` and `lease.error` separately for terminal failure.
`NET_REJECTED` traffic is normally nonfatal. Treat `NET_DHCP_FAILED` as terminal.

Use caller-supplied monotonic wrapping 32-bit milliseconds, as with DNS. Live
launchers pace ticks (`rvm --clock paced`, `RCC_NATIVE_PACED=1`). Lease durations,
T1/T2 and age are kept in seconds with a millisecond remainder, rather than
multiplying a long lease into an overflowing millisecond deadline. Call regularly,
with less than a full millisecond-clock wrap between service calls.

States are STOPPED, SELECTING, REQUESTING, PROBING, BOUND, RENEWING, REBINDING,
BACKOFF, WAIT_LINK and FAILED. During renewal/rebinding, an unexpired installed address
remains usable even though the state is no longer BOUND. Counters expose initial
acquisitions, renewals, expirations and malformed matched replies.

On link loss, the client removes its lease, clears pending neighbor/TX state and
enters WAIT_LINK. Keep polling: offline time does not consume the acquisition
budget. Restoration or a NIC MAC change starts fresh acquisition, not reuse of
the old address. See [link lifecycle](reliability.md#drive-time-and-observe-the-link).

`net_dhcp_stop` cancels locally, removes the address and releases the UDP binding.
It does not send DHCPRELEASE; the server retains its lease until expiry. No heap,
thread, OS scheduler or interrupt handler is required.

## Lease and conflict behavior

The client validates BOOTP framing/cookie, transaction ID, client MAC, server
identity, message state, option bounds and lease/timer values. It supports subnet,
router and DNS options, lease time, T1/T2 and option overload in the BOOTP file
and server-name fields. Unknown options are skipped with checked lengths.

Initial configuration requires a usable unicast address, contiguous nonzero
mask, server ID and positive lease; subnet network/broadcast addresses are
rejected. The first router/DNS address is used if supplied. Missing router or
DNS remains explicit zero configuration, not a hardcoded replacement. Parameters
omitted from ACK may retain those from the selected offer or current lease.
Server-provided timers must satisfy `0 < T1 < T2 < lease`; absent timers use
one-half and approximately seven-eighths of the lease. An all-ones lease is
infinite and does not enter timed renewal/expiry.

Before using a new address, the client sends three ARP probes from `0.0.0.0`,
with randomized spacing, and waits after the last probe before applying the
configuration. It then announces the address and schedules a second announcement.
A conflicting ARP sender or simultaneous probe triggers DHCPDECLINE, removes the
candidate and waits at least ten seconds before another acquisition. Repeated
conflicts are bounded: three conflicts terminate the client rather than looping
forever. A conflict after binding also relinquishes the address; this version
does not attempt continued address defense.

At T1, DHCPREQUEST is unicast to the selected server with the installed `ciaddr`.
At T2, requests switch to broadcast and another server can supply the renewal.
Renewal retransmissions use half the remaining phase time with a 60-second
minimum. The state still advances at T2 or expiry even when the next retry is
later. Lease accounting conservatively starts at the first request in the
transaction, including time waiting for ACK and probing.

On expiry the address is removed **before processing another incoming frame**
and acquisition restarts. A NAK also removes the address and starts acquisition
after backoff. An ordinary unchanged renewal leaves unrelated active DNS work
intact; a changed configuration invalidates it.

## Configuration changes and DNS

`net_configure` validates/applies static or leased configuration. Changed IP,
mask, router or DNS clears ARP, discards the context's pending TX frame, clears
stale echo events and increments `NetContext.generation`. UDP bindings survive.
Already accepted host-queue frames cannot be recalled. `net_unconfigure` removes
the address with the same invalidation behavior.

DNS captures that generation at start. If it changes, an active lookup completes
with `NET_DNS_CONFIG_CHANGED` and releases its binding instead of accepting an
answer from the old network. Other long-lived operations must observe generation
changes too; future TCP state must not silently keep an old local address.
Explicit application reconfiguration stops an active DHCP client with
`NET_CONFIG_CHANGED`, preserving the replacement configuration.

## Limits and validation

This is one IPv4 DHCP client per context, with no saved-lease/INIT-REBOOT flow,
DHCPINFORM or DHCPRELEASE operation. Critical duplicate options are rejected;
RFC3396 option concatenation is not implemented. Invalid supplied timers are
rejected, and short leases whose default T1/T2 cannot form valid ordered timers
are not accepted. There is no DHCP server implementation.

See [Testing](testing.md#dhcp) for fake-clock unit tests, independent packet
fixtures, two simultaneous clients and live host tests. Shared-mode vmnet has
also supplied a real lease used by guest DNS and three successful VPS pings.
RBF execution of the new DHCP client is not part of that recorded milestone.

Protocol references: [DHCP, RFC 2131](https://www.rfc-editor.org/rfc/rfc2131.html),
[options, RFC 2132](https://www.rfc-editor.org/rfc/rfc2132.html), and
[address conflict detection, RFC 5227](https://www.rfc-editor.org/rfc/rfc5227.html).
