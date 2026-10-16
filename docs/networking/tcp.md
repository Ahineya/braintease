# Guest TCP streams and listeners

`<net_tcp.h>` implements bounded IPv4 TCP streams and passive listeners in the
shared guest C stack. It constructs/checks segments, manages sequence numbers, buffers
streams and performs retransmission itself. RVM/RBF/native carry Ethernet frames;
no execution host substitutes sockets for guest TCP. No instruction or DEVICE_*
command was added. The [HTTP server](http-server.md) uses the passive listener API.

## Run the echo demo

On macOS, with the release toolchain built:

```sh
scripts/run-net-tcp.sh
scripts/run-net-tcp.sh rvm --jit
scripts/run-net-tcp.sh rbf --jit  # explicit, slower Brainfuck run
```

The launcher builds the guest, starts a temporary ordinary host TCP echo service,
and runs guest DHCP followed by a 4,097-octet binary exchange. vmnet starts on
the first NIC query, with host-only attachment and packet capture disabled.
Only its helper needs authorization. No second-terminal setup or manual guest
IP selection is needed. The host service binds to `192.168.64.1`, not all host
interfaces, and is removed after the run. Output ends with:

```text
TCP local=<leased IP> peer=192.168.64.1 port=7777 octets=4097
TCP OK written=4097 acknowledged=4097 received=4097 retransmits=<count> probes=<count>
```

The peer is explicit: host-only vmnet can advertise the guest itself as its
router, so the default gateway does not identify the host echo service.
`--port 7778` selects another unprivileged temporary server port. `--build-only`
only compiles; outputs live under `build/network/tcp-echo/<mode>/`.

To use a separately supplied echo server or Ethernet peer:

```sh
scripts/run-net-tcp.sh rvm --peer 192.168.64.1 --port 7777 --no-server
scripts/run-net-tcp.sh native --static --socket /actual/nic.sock --no-server
```

`--peer` and `--socket` also disable the temporary server. `--static` uses the
older demo's explicit NetConfig; otherwise DHCP remains active. Automatic host
echo requires DHCP and host-only execution. `--shared` is an explicit
NAT selection for external services. The demo expects an exact echo and peer
EOF; it does not send HTTP or resolve its peer through DNS yet.

RBF is opt-in and uses the same automatic echo service. The host deadline is
1800 seconds for RBF, versus 60 for native/RVM; `--timeout SECONDS` overrides
it. RBF prints periodic execution progress and saves a runtime report beside
its log. It executes the actual Brainfuck CPU and guest stack, so expect much
longer startup and transfer times. It is excluded from the default test matrix.
A bounded RBF JIT check confirmed DHCP, TCP establishment and ongoing traffic;
the full 4,097-octet exchange exceeded its 10-minute check, so a complete RBF
echo pass is not yet recorded.

## Ownership and API

Initialize the network through static configuration or DHCP first. Caller-owned
connection objects must be zeroed before first use and remain at a stable
address while active. The context holds at most four registered connections;
each connection owns a 512-octet TX buffer and a 512-octet RX ring, plus bounded
state. No heap or scheduler is required.

`net_tcp_connect` enables TCP dispatch on the context. `net_tcp_enable` can
enable closed-port resets without connecting. The base stack references an
optional internal driver, so programs using neither entry point do not pull
TCP code into their executable.

```c
#include <net_tcp.h>

static NetTcp stream;
static NetTcpConfig endpoint = {{192,168,64,1}, 0, 7777, 0, 60000UL};

int connect_to_echo(NetContext *network, unsigned long now_ms,
                    unsigned long unpredictable_initial_sequence) {
    endpoint.initial_sequence = unpredictable_initial_sequence;
    return net_tcp_connect(network, &stream, &endpoint, now_ms);
}
```

Local port zero chooses a free ephemeral port, 49152..65535. Explicit local
ports are exclusive within a context, including TIME_WAIT. Remote ports are
1..65535. `NET_NO_PORT` reports full connection slots or a local port in use.
Reusing an active object or one with unread orderly-close data reports NET_BUSY.

Connect returning NET_OK means the attempt started. Drive `net_poll` or
`net_dhcp_poll` first, then `net_tcp_poll` for **every active connection**, using
the same caller-supplied wrapping millisecond clock. Poll does not receive NIC
frames on its own. Watch state ESTABLISHED/CLOSE_WAIT for writable streams;
SYN_SENT is still opening. Check the returned status and `stream.error` for
failure. Closed/failed objects release their table slot automatically.

```c
unsigned short written, received;
unsigned char output[3] = {0, 255, 7}, input[193];

/* Call only after driving network and TCP progress. Retry any unsent suffix. */
int status = net_tcp_write(&stream, output, 3, &written);
/* A partial write owns a copy of exactly written octets. */
status = net_tcp_read(&stream, input, sizeof(input), &received);
```

Write accepts as much as fits and reports that count. A full buffer returns
NET_AGAIN/zero; opening also returns NET_AGAIN. Buffer acceptance is distinct
from wire transmission and acknowledgement: `bytes_written`, `bytes_acked`,
`bytes_received` and `bytes_read` expose those boundaries. Callers can reuse
the accepted portion of their input immediately. The library never retains
an application payload pointer.

Read returns copied octets, NET_AGAIN when empty, or NET_EOF once peer FIN has
arrived and buffered data has been consumed. Packet boundaries are invisible.
EOF leaves the local writing half usable until `net_tcp_shutdown`. Shutdown
drains/acknowledges accepted writes before sending FIN; it does not discard
unread data. Continue polling through the closing states. Sending additional
data after local shutdown returns NET_CLOSED.

The active closer keeps its slot/object in TIME_WAIT for 120 seconds, restarting
that timer for a matching retransmitted FIN. The passive closer ends after its
own FIN is acknowledged. Closing states are otherwise bounded by the progress
timeout. `net_tcp_abort` explicitly cancels, discards buffers, releases the slot
and sends RST if it can do so immediately. Aborting TIME_WAIT gives up its
tuple-reuse guard; do not free/move a still-active object instead of cancelling it.

## Listen and accept

`net_tcp_listen` registers an IPv4 local port and a caller-owned TCB pool, with
capacity 1..4. At most two listeners and four live connections share a context.
Pending handshakes, established connections and full-TCB TIME_WAIT all count
toward that connection limit; pool memory is reserved by its caller, not a heap.
Listening itself does not consume a connection slot. Active connects cannot
claim a listener port, and listeners cannot bind an active-open local port.
Accepted children share their listener's port and are distinguished by full
peer/local tuples.

```c
static NetTcpListener listener;
static NetTcp clients[2];
static NetTcpWaitTable closing;
extern unsigned long make_sequence(void *, const unsigned char *,
                                   unsigned short, unsigned long);
static NetTcpListenConfig settings = {8080, 15000UL, make_sequence, 0};

int listen_to_http(NetContext *network) {
    int status = net_tcp_wait_attach(network, &closing);
    if (status) return status;
    return net_tcp_listen(network, &listener, clients, 2, &settings);
}
```

Zero the listener and pool before first use, and keep them at stable addresses.
The pool belongs exclusively to that listener until it is stopped and all
accepted objects are released; do not pass its objects to active connect or
another context during that lifetime.
The synchronous ISN callback receives user data, peer IPv4/port and current
milliseconds for each new passive open. Supply unpredictable sequences for
untrusted networks; do not recursively poll/mutate the context in that callback.
Repeated SYNs reuse the original ISN and do not restart the handshake deadline.
SYN-ACK is retransmitted through the same reliable-flight machinery as active
TCP. Final ACKs must acknowledge an actually transmitted SYN-ACK; invalid ACKs
cannot establish a connection. Final-ACK data/FIN is delivered through the normal
ordered stream path. TCP Fast Open/SYN data is not supported and such new opens
are dropped without allocating a slot.

After network/DHCP poll, call `net_tcp_listener_poll` to drive **all** pool
members, including unaccepted handshakes, accepted streams and close/TIME_WAIT.
Then `net_tcp_accept(listener, &connection)` returns NET_OK and a borrowed TCB
when its handshake completes, or NET_AGAIN with a null output. A peer may
already have queued data or half-closed. Existing read/write/shutdown APIs apply.
Pool/global-table exhaustion drops new SYNs and increments `listener.refused`;
it does not overwrite an existing connection or allocate additional storage.
Unaccepted state has an absolute `timeout_ms` lifetime, including a completed
connection that the application never accepts. Duplicate traffic cannot keep a
half-open slot forever.

Accepted objects remain claimed until **explicitly** released, including after
failure or orderly close. Call `net_tcp_release(listener, connection)` when the
application is done. Active states return NET_BUSY; abort explicitly only when
cancelling. Release discards any remaining unread data and returns the object
to its pool. Never retain/use that pointer after release: a new connection can
reuse its address on the next received SYN. Starting/stopping a listener does
not silently transfer ownership of an accepted object.

`net_tcp_unlisten` stops new opens and aborts unaccepted work. Accepted streams
remain alive; keep polling/release them. The listener and pool must stay stable
until all accepted objects are released. Configuration/link changes stop the
listener with NET_CONFIG_CHANGED/NET_LINK_DOWN and terminate affected streams.
There is no automatic relisten on a different lease. Explicit rebind after
recovery is an application decision.

### Compact TIME_WAIT storage

`net_tcp_wait_attach(network, &table)` optionally attaches a zeroed caller-owned
`NetTcpWaitTable`. Without one, the full TCB remains active for normal TIME_WAIT.
With one, release can preserve an accepted TIME_WAIT tuple/sequence/timer in one
of 32 compact records, detach the full connection, and immediately reuse its
large buffers. Duplicate FINs still get their last ACK and restart the record's
120-second timer. Delayed SYN/data/RST cannot create the same old tuple, while
other tuples can reuse the local listening port. Outgoing active opens also
respect protected local ports.

This storage belongs to the context independently of any listener, so it must
outlive retained records even after unlisten. Empty/expired storage can be
replaced or detached with `net_tcp_wait_attach(network, 0)`; live records return
NET_BUSY. A full compact table leaves the full TCB in TIME_WAIT and release
returns NET_BUSY: records are never evicted to make a request look successful.
Keep polling and retry release, or deliberately abort if giving up tuple
protection is acceptable. Changed configuration/link epochs invalidate old
records. Ordinary UDP/DNS/ping clients allocate no table; the receive/port paths
return immediately when no table is attached.

## Timers, windows and lifecycle

`timeout_ms`, 1..600000, bounds handshake or idle progress. New received bytes,
newly acknowledged bytes and handshake completion count as progress; duplicate
ACKs/probes do not. Even an otherwise idle established stream eventually times
out under this policy. There is no implicit keepalive. Use paced ticks for a
live peer, or an explicitly advanced fake clock in tests.

The sender allows **one segment in flight**, capped at the peer MSS, our
512-octet limit and the peer's window. Retransmission starts at a one-second
minimum, estimates RTT/variance from unambiguous ACKs, skips retransmitted RTT
samples, and exponentially backs off to 60 seconds. Seven retransmissions or
the configured progress deadline terminate remote silence. Local ARP/TX
backpressure does not start a wire retransmission timer before host acceptance.

Zero-window data probes contain one actual queued octet. A window update can
accept that octet or request its retransmission; neither loses nor invents a
stream byte. A pending FIN waits for sequence-space window and uses bounded
ACK probes while closed. Partial ACKs retain the unacknowledged suffix, and
ACKs cannot advance past data actually accepted for transmission.

The receiver validates tuple, header/option bounds and pseudoheader checksum,
then uses wrapping 32-bit sequence arithmetic. It discards out-of-order data
and cumulatively ACKs the current position; duplicate prefixes are trimmed.
Receive-window excess is clipped and retransmitted later by the peer. Reading
frees space and schedules a window update. There is no unbounded reorder queue.

Configuration or link/MAC epoch changes terminate affected streams with
NET_CONFIG_CHANGED or NET_LINK_DOWN, discard queued state and release slots.
They do not replay writes on a new lease. Exact-sequence RST terminates with
NET_RESET; an in-window inexact RST elicits a rate-limited challenge ACK.
Closed-port resets are bounded too and never answer another RST.

Matched ICMP protocol/port-unreachable errors terminate with NET_UNREACHABLE;
fragmentation-needed and parameter problems report NET_PATH_ERROR, retaining
`icmp_type`, `icmp_code`, and advertised `mtu`. Network/host-unreachable and TTL
errors are soft diagnostics: normal retransmission/deadline policy continues.
Old, malformed or unrelated quotes cannot cancel a reused connection.

## Compiler costs found in the echo demo

The 2026-10-16 IR/assembly investigation found two missed optimizations, now
implemented in the shared compiler without changing guest source or ISA:

| Small operation, 8192-cell banks | Before emitted instructions | After |
| --- | ---: | ---: |
| `(offset * 37UL + 11UL) % 256UL` helper | 111 | 13 |
| Read two header octets as a word | 89 | 70 |
| Read a field at offset 3155 through an unknown pointer | 53 | 39 |

The byte-pattern helper previously computed the entire 32-bit multiplication,
including six word multiplies and many spills, before masking to eight bits.
The new low-word pass narrows single-use modular arithmetic and then explicitly
zero-extends the result to its original type. This preserves wide call/return
ABIs and any independently observed high words. Division, right shifts, shared
wide expressions and volatile memory accesses remain conservative.

Constant pointer arithmetic also allocated a second address/bank pair despite
already owning private copies. Reusing those copies reduces register pressure
and callee-save traffic. Carry, borrow, bank crossing and live source pointers
remain tested. The static TCP fixture's linked code shrank from 84,931 to 81,455
instructions with identical 5,427-cell data reservations.

A checked 32-iteration BF [benchmark](../../rbf/benchmarks/jit/README.md#masked-arithmetic-compiler-result-2026-10-16)
reduces executed guest instructions from 3,098 to 1,411 and BF dispatches from
50.9 million to 22.1 million. These are workload-specific counts, not a measured
speedup for the complete live DHCP/TCP run. The long live BF exchange was not
repeated during the compiler investigation. The new C regressions pass on
native, RVM interpreter and JIT; all 346 backend tests pass. The nine TCP packet
cases pass on native and RVM JIT, including a fully rebuilt Ripple guest stack.

Remaining candidates include preserving resolved bank provenance across pointer
operations, keeping more loop-carried state in registers, and bank-aware memory
copy/fill loops. Even the improved two-octet read still expands to dozens of VM
instructions, each executed by the BF CPU. A dynamic profile is needed before
assigning percentages to those costs. Static DIV/MOD instructions can belong to
cold bank-normalization fallbacks; power-of-two packet arithmetic already uses
shifts/masks. `--emit-ir`/`--save-ir` expose frontend IR before backend passes,
so compare the resulting assembly too when checking an optimization.

## Supported subset and security

This is a small bounded implementation, not a claim of complete TCP conformance. It
supports MSS and bounded parsing/skipping of other options, but does not offer
window scaling, SACK, timestamps or ECN. There is no Nagle buffering, fast
retransmit, growing congestion window, simultaneous open,
IPv6, IP reassembly, path-MTU adaptation or TLS. Its fixed one-segment cap is
deliberately conservative and limits throughput. Urgent-data semantics are
unsupported and fail explicitly with NET_UNSUPPORTED.

Initial sequence selection is an explicit caller responsibility. Supply a
fresh unpredictable value for untrusted peers, and suitable entropy for the
context's ephemeral-port seed. The local demo uses MAC/clock-derived values;
the packet fixture can force a wraparound ISN. Neither is an RFC 6528 keyed
ISN generator, and this milestone does not claim Internet-hardening. Normal
TIME_WAIT and tuple exclusivity do not make predictable ISNs secure.

See [TCP testing](testing.md#tcp) for fake-NIC tests, independent loss/reorder
fixtures, exact binary-stream checks and ordinary host-kernel interoperability.
References: [TCP, RFC 9293](https://www.rfc-editor.org/rfc/rfc9293.html),
[RTO, RFC 6298](https://www.rfc-editor.org/rfc/rfc6298.html),
[congestion control, RFC 5681](https://www.rfc-editor.org/rfc/rfc5681.html).
