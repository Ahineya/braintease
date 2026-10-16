# Ethernet device contract

This is the implemented v1 device and backend contract. See the
[overview](README.md) for status, [running guide](running.md) for launchers,
and [guest library](library.md) for protocol APIs. Planned protocols in the
[roadmap](roadmap.md) continue to use this same device interface.

The existing [device mailbox](../../rvm/mmio.md#device-command-abi-version-1)
exposes a virtual Ethernet card to RVM, RBF and the native runtime. Guest C code
implements network protocols. A shared host device moves complete frames
between guest memory, bounded queues and a selectable network backend.

## Scope and fixed decisions

- Use `DEVICE_*` at bank-0 addresses 27–31. Networking needs no new instruction
  or dedicated MMIO registers, and works independently of display mode.
- Expose Ethernet frames. The guest owns ARP, IP, ICMP, UDP and eventually TCP;
  the device does not offer host socket operations or secretly implement the
  guest's protocol stack.
- Store **one eight-bit octet per 16-bit guest cell**. Packing two octets into
  a cell is outside v1: bit operations are particularly expensive in RBF's
  [Brainfuck CPU](../../src/ripple-asm/src/cpu_footer.bfm).
- Start with polling and synchronous copies at mailbox submission boundaries.
  Interrupts, guest-memory rings and asynchronous DMA are outside v1.
- Use freestanding C examples for the first protocol implementation. rOS is a
  future consumer; its current structure is not a requirement or a dependable
  foundation for this work. Multitasking and daemons are separate future work.
- The first live acceptance milestone is successful ping in **both directions
  between the guest and the actual host OS network stack**.

## Sharing the mailbox with other devices

Networking uses the same five registers at addresses 27–31 as display, input and
clock commands. Allocate distinct command IDs in the existing namespace; do not
add separate networking MMIO pins or reinterpret existing command IDs.

The mailbox carries one submission at a time. Each caller must complete the
whole transaction: set descriptor bank/offset, submit the command, wait for its
completion if necessary, and read status and outputs before another caller
uses the mailbox. Separate descriptors may be allocated for each device, but
the pointer and status registers are shared.

For example, a guest may submit `NET_TX`, read its result, submit `INPUT_READ`,
read that result, and submit `PRESENT`. The accepted network frame remains in
the host-owned TX queue while the guest uses the mailbox for other devices.
Network commands return after queue/copy work; they do not hold the mailbox
while waiting for transmission on the wire or a remote reply. A pending
`NEXT_TICK` suspends guest execution as usual; host network queues can still
progress during that wait.

Device state stays separate: NIC commands do not consume keyboard events,
advance time or publish frames. Background network activity updates NIC queues,
link state and statistics only. In particular, a late TX failure must not
overwrite `DEVICE_STATUS` or an old descriptor after another device has used
the mailbox; the guest discovers it through a subsequent NIC query.

A freestanding program with one foreground caller can sequence these operations
directly. Future multitasking requires serialization of the **entire mailbox
transaction**, not just the write to `DEVICE_COMMAND`. Interrupt handlers must
not interleave submissions with an interrupted caller; use an interrupt-safe
submission policy, such as deferring their requests. Current C helpers do not
provide a scheduler lock or make the mailbox reentrant.

## Device and backend boundary

Transmit follows this path:

```text
guest protocol stack -> NET_TX -> host TX queue -> backend -> network
```

Receive follows the reverse path through the RX queue and `NET_RX`. Background
host I/O owns host buffers only. It must not access guest RAM or execute guest
instructions. The mailbox adapter validates and copies guest memory while the
guest executes the command. On RBF, guest instructions continue to run in BF;
the host provides the network device, just as it provides other external I/O.

The guest-visible NIC contract is shared across all three hosts. Backends may
connect it to a deterministic virtual peer, another VM or the host's network
stack. A live attachment must support Ethernet-level guest traffic, including
ARP. Host-specific setup belongs in the backend and launcher, not in guest C.

The frame buffer contains destination/source MAC addresses, EtherType and
payload. The guest does not supply a physical Ethernet preamble or FCS; the
backend handles any adaptation its transport requires. Frame limits and MTU
must be discoverable rather than inferred from the host implementation.

## Commands and wire format

| Command | ID | Length | Inputs and outputs |
|---------|----|--------|--------------------|
| `NET_INFO` | 0x0100 | 18 | interface selection; MAC, MTU, maximum frame size, link state and queue limits out |
| `NET_TX` | 0x0101 | 8 | interface, buffer bank/offset, frame length; queue outcome out |
| `NET_RX` | 0x0102 | 9 | interface, buffer bank/offset, capacity; length and receive outcome out |
| `NET_STATS` | 0x0103 | 27 | interface selection; queue occupancy and frame counters out |

All descriptors use the mailbox's version/length header and banked guest-memory
pointers. `CAPS` advertises `VM_CAP_NETWORK = 0x0020`. Unsupported hosts
must be distinguishable from a supported NIC with its link down. An interface
selector leaves room for multiple NICs; v1 only needs one.

All offsets below count 16-bit descriptor cells. Cells 0 and 1 are ABI version
1 and the exact descriptor length, and cell 2 selects interface 0. A different
interface, bad length/version, invalid flags or invalid memory range produces
the existing mailbox error status, without modifying payloads or consuming
frames. Other device commands and their IDs are unchanged.

| Descriptor | Remaining cells |
|------------|-----------------|
| `NET_INFO` | 3 link (out), 4 MTU (out), 5 maximum frame (out), 6–11 MAC octets (out), 12 RX frame limit (out), 13 TX frame limit (out), 14–15 RX octet limit (out), 16–17 TX octet limit (out) |
| `NET_TX` | 3 buffer bank, 4 buffer offset, 5 frame length, 6 outcome (out), 7 flags=0 |
| `NET_RX` | 3 buffer bank, 4 buffer offset, 5 buffer capacity, 6 frame length (out), 7 outcome (out), 8 flags=0 |
| `NET_STATS` | 3 RX queued frames (out), 4 TX queued frames (out), 5–6 RX queued octets (out), 7–8 TX queued octets (out), 9–26 nine 32-bit frame counters (out) |

Multiword outputs are low cell first. The counters, in order, are RX received,
RX delivered, RX dropped, TX accepted, TX handed to the backend, TX failed,
TX would-block, RX oversized and backend errors. They saturate at `0xffffffff`.
RX received includes subsequently dropped frames; oversized drops are also
included in RX dropped. Queue occupancy excludes a frame already held by the
background sender, which holds at most one additional frame.

Mailbox success is status 0. On success, inspect the separate operation outcome:

| Outcome | Value | Meaning |
|---------|-------|---------|
| `VM_NET_OK` | 0 | TX accepted or RX frame copied |
| `VM_NET_EMPTY` | 1 | no RX frame on an active link |
| `VM_NET_WOULD_BLOCK` | 2 | TX queue cannot accept this frame yet |
| `VM_NET_BUFFER_TOO_SMALL` | 3 | RX length reports required capacity; frame retained |
| `VM_NET_LINK_DOWN` | 4 | no active backend; RX length is zero |

RX can drain already queued frames after link loss. TX/RX buffers must not
overlap their own descriptor. V1 uses MTU 1500, frame lengths 14–1514 octets,
64 queued frames per direction and 65,536 queued octets per direction. VLAN
extensions and jumbo frames are outside this initial format. With no attached
backend, NIC 0 is present with its link down; this differs from an older host
that does not advertise networking.

Descriptor lengths and pointers retain the existing cell units. Frame lengths,
capacities and MTU use **octets**; one octet occupies one cell in v1. RX writes
values in 0–255 with zero upper bits. TX rejects cells greater than 255 rather
than silently truncating them. Zero octets are data, never terminators.

Protocol headers require explicit serialization in network byte order. Ripple
C's 16-bit byte and structure layout must not determine Ethernet/IP wire layout.

## Queue and memory contract

1. RX and TX are separate FIFO queues, bounded by both frame count and total
   octets. Publish their limits. Preserve frame boundaries and enqueue order.
2. Host reception and transmission progress independently of guest speed,
   display activity and `NEXT_TICK`. NIC queries and transfers never advance
   the guest's logical clock.
3. Successful TX means the host copied and accepted the entire frame. The guest
   may immediately reuse or free its source buffer. Acceptance is not remote
   delivery or a TCP acknowledgement; later backend failures are observable in
   link state and statistics.
4. A full TX queue reports a retryable `WOULD_BLOCK` outcome without accepting
   any part of the frame. The guest decides when to retry.
5. An empty RX queue returns immediately with an explicit empty outcome. No
   implicit wait or tick request occurs.
6. An undersized RX buffer reports the required frame length and leaves the
   frame queued. It must not truncate or partially copy the frame. The final
   descriptor returns the required length and `VM_NET_BUFFER_TOO_SMALL` under
   mailbox status 0, so C helpers expose the result normally.
7. Validate descriptor fields, complete buffer ranges and TX octet values before
   changing queues or writing RX payloads. Invalid requests cannot consume a
   pending RX frame. Buffers may cross banks using the existing memory rules.
8. When RX limits are reached, drop the newly arriving frame and increment drop
   counters, retaining queued frames. Oversized frames and backend failures also
   need explicit accounting. Ordinary packet loss does not terminate the VM.
9. Statistics reads do not consume frames or clear counters. Frame counters are
   32-bit saturating values.

For example, if a 98-octet frame is queued and the guest offers 64 cells, RX
reports that 98 are needed and keeps the frame. Retrying with 128 cells copies
98 values, reports length 98, and removes that frame. The remaining 30 cells
are untouched. A subsequent RX returns empty unless another frame has arrived.

## Live time, simulated time and replay

Queues absorb bursts; they cannot make an arbitrarily slow guest keep up with
a real network. A remote peer's timeouts continue to run while RBF computes.
Changing or pausing guest logical time does not pause the host or a VPS.

Maintain two test environments behind the same NIC contract:

- A deterministic peer with controlled simulated time for protocol regression
  tests, including slow RBF execution. Record frame order and delivery points;
  repeated empty polls must not advance time or release future scheduled frames.
- A live backend for the real host and external network. Record wall-clock
  round-trip times and configure test deadlines explicitly for each execution
  mode. Longer deadlines help testing but do not guarantee a remote peer waits.

Virtual-peer tests must not substitute for the bidirectional host ping milestone.
Host-side routing/NAT may carry live traffic, but the guest still constructs and
validates its own protocol packets.

Disconnected/controlled NIC checkpoints preserve queues, order and statistics.
Cloning device state makes an independent snapshot. Checkpoints with an attached
live backend are explicitly rejected; `rbf --net` conflicts with save/load-state
options. A live connection cannot be recreated merely by restoring queues.
Exact replay requires a controlled backend and recorded input.

A VM reset clears NIC queues and counters while preserving its configured MAC
and link/backend attachment. An already handed-off frame cannot be recalled;
its late completion does not count against the new reset generation.

## Local backend transport

The common transport uses Unix datagrams. The initial request is five octets
`RNET` followed by 1; the response adds six MAC octets and a big-endian 16-bit
MTU, totaling 13 octets. Later datagrams of 14+ octets are complete Ethernet
frames. Five-octet messages `RNET`+2 are heartbeat request/reply, +3 reports a
backend failure and +4 detaches a client. An unresponsive helper marks the link
down after five wall-clock seconds and records a backend error. Transport
workers own no guest descriptor pointers, so they cannot overwrite mailbox
status during other commands. No live Linux TAP helper is supplied yet; Linux
can use the local fixture or another backend speaking this transport.

## Legacy MMIO and later work

Networking and future audio can use the mailbox without reusing addresses
10–15. Migrate examples and runtime keyboard helpers toward descriptor-based
input separately, including TEXT40 behavior. Retain compatibility reads or leave
deprecated addresses reserved until an explicit memory-map revision defines
reuse; old binaries must not silently reinterpret key reads as other devices.

Future protocol and application work is tracked in the [roadmap](roadmap.md).

## References

- [Existing MMIO/device contract](../../rvm/mmio.md#device-command-abi-version-1)
- [Linux TUN/TAP documentation](https://docs.kernel.org/networking/tuntap.html):
  the distinction between IP-packet and Ethernet-frame host interfaces.
- [QEMU network backend documentation](https://www.qemu.org/docs/master/system/devices/net.html):
  separation of guest NICs from host network attachment.
