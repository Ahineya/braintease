# Networking through the device mailbox

Status: v1 Ethernet commands, bounded queues, a local Unix-datagram transport,
macOS vmnet attachment and a freestanding ARP/IPv4/ICMP/UDP/DNS guest library
are implemented. RVM, RBF and the native runtime share the device. Local
bidirectional ping and guest-generated ping to the VPS IPv4 address have been
validated on all three on macOS. Guest-side DNS followed by three validated
pings to `syma-lang.org` has passed on RVM and native. RBF execution of the new
DNS client remains opt-in and was not repeated for this milestone.

See [the guest library guide](net-library.md) for the C API, UDP/DNS examples,
limits and `scripts/run-net-dns.sh`, which builds and runs the complete guest
DNS-to-ping demonstration without a separately managed helper.

The existing [device mailbox](../rvm/mmio.md#device-command-abi-version-1)
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
  [Brainfuck CPU](../src/ripple-asm/src/cpu_footer.bfm).
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

For example, a guest may submit `NET_TX`, read its result, submit `INPUT_POP`,
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

## Building and running

### Automatic runtime attachment

On macOS, ordinary RVM runs and native executables start a private **host-only**
Ethernet connection when the guest first submits a valid `NET_INFO` for NIC 0
(normally through `vm_net_info`). No launcher, socket path or helper build is
required. Programs that do not use the NIC, including those that only query
`CAPS`, do not start a helper or request authorization.

The runtime embeds the native macOS helper at build time and extracts it into
a private temporary directory when needed. Only the helper requests `sudo`
authorization; guest execution retains the invoking user's privileges. The
first NIC discovery may therefore wait for host authorization and startup.
Later NIC commands use the existing synchronous queue/copy contract. Startup
failure is logged once, leaves the link down and increments `backend_errors`;
ordinary polling does not repeatedly prompt or restart the connection.

For an already built monitor, simply run:

```sh
build/network/monitor/native/monitor
target/release/rvm build/network/monitor/rvm/monitor.bin --bank-size 4096 --memory 32768 --jit
```

Host policies can be selected with `rvm ... --net POLICY` or
`RCC_NATIVE_NET=POLICY executable`. `RIPPLE_NET` is a shared environment fallback
when no runtime-specific selection is supplied:

| Policy | Behavior |
|--------|----------|
| `auto` (default) | lazy host-only attachment on macOS; disconnected elsewhere |
| `host` | explicitly request lazy host-only attachment |
| `shared` | explicitly request lazy macOS Internet sharing/NAT |
| `off` | keep NIC 0 disconnected; never start a helper |
| socket path | connect to an existing Ethernet backend; leave that helper running on exit |

Use a qualified path such as `./host` if an external socket is named like a
policy. Automatic attachment disables packet capture and never enables Internet
sharing implicitly. The guest still configures its own IPv4 address; the demo
uses `192.168.64.250/24` with host/gateway `192.168.64.1`.

Orderly exit detaches and stops the owned helper. If a guest process dies without
running destructors, the managed helper stops after its five-second heartbeat
timeout, removes its interface/socket and cleans its private files after dropping
to the invoking user's privileges. External helpers retain their manual lifetime.
The C test runner explicitly disables automatic networking for offline tests.
Library-created device states and restored snapshots are also offline until the
embedding host configures them. RBF uses the same policy, with automatic networking
disabled for checkpoint runs; attached live NIC state remains non-checkpointable.

### Interactive ping monitor (macOS)

From the repository root, run:

```sh
scripts/run-net-monitor.sh
```

This builds [`monitor.c`](../c-test/examples/network/monitor.c) with RCC's native
target and runs the guest as your normal user. The runtime initializes networking
on its first NIC query. Internet sharing and packet capture are disabled. It
prints the guest's static configuration and waits indefinitely:

```text
PING monitor local=192.168.64.250 mask=255.255.255.0
gateway=192.168.64.1 MAC=<assigned by vmnet>
Waiting for pings; Ctrl-C to stop.
```

In a second terminal:

```sh
ping 192.168.64.250
```

The network is ready as soon as the monitor prints its configuration. The
second-terminal `ping` is optional test traffic, not a networking setup step.
The host shows normal ping replies. The guest logs each validated echo request
and the reply it queues, for example:

```text
PING request from=192.168.64.1 id=1234 seq=0 octets=56
PING reply queued
```

`octets` counts the ICMP payload, excluding its header. Queuing a reply means
the host NIC accepted the frame; the host's `ping` output confirms delivery.
Invalid IP/ICMP checksums receive no reply. The IP is compile-time configuration,
not DHCP or the Mac's LAN address. Stop the monitor with Ctrl-C; its private
helper, interface and socket are cleaned up too. Run one monitor at a time with
the default IP.
At startup the guest library sends a gratuitous ARP announcement, so restarting it
with a newly assigned MAC refreshes the host's cached entry for that static IP.

The same guest protocol implementation also runs on the VMs:

```sh
scripts/run-net-monitor.sh rvm --jit
scripts/run-net-monitor.sh rbf --jit
```

The launcher only builds and runs the C application. It expects the release
RCC toolchain and selected runtime to be built already. For RVM/RBF this also
includes the assembler/linker, and for RBF the macro expander. RBF takes longer
to build and start; wait for the guest's `Waiting for pings` line before pinging.
`ping -i 3 -W 10000 192.168.64.250` gives RBF more time on macOS.

To reuse an already running Ethernet helper, append `--socket /actual/nic.sock`.
The launcher then needs no `sudo` and leaves that helper running when stopped.
For builds without launching, use
`NET_DEMO=monitor scripts/build-net-ping.sh rvm build/network/monitor/rvm`
(substitute `native` or `rbf` as needed). `scripts/test-net-smoke.py --monitor`
checks the RVM and native monitor builds with an independent packet peer: ARP discovery,
multiple requests, empty/odd/56-octet payloads, malformed checksums, log output
and continued execution after replies. `--jit` exercises the RVM JIT. RBF remains
available with `--mode rbf`, but is excluded from routine test runs because of
its build and execution cost.

`scripts/test-net-auto.py --jit` additionally checks actual host ping with no
launcher or socket setting, a native executable copied outside the repository,
and helper cleanup after abruptly stopping each guest. It tests RVM and native
only. If another guest already uses the demo IP, build monitors with a different
`-DNET_LOCAL_IP=a,b,c,d` and pass `--build-root DIRECTORY --guest-ip a.b.c.d`.

### Bounded protocol tests and manual attachment

The C API is [`<vm_network.h>`](../runtime/include/vm_network.h). Like other
device helpers, it returns mailbox status first and updates output objects only
on success. Applications must then check the operation outcome. The equivalent
raw command/outcome constants are exported by `rfl/std/mmio.rfl`.

[`ping.c`](../c-test/examples/network/ping.c) uses the guest
[`<net.h>` library](net-library.md), which constructs and validates ARP, IPv4
and ICMP packets in guest C, including checksums, odd-length payloads and
zero/255 octets. It polls independently of ticks and waits for accepted TX work
to be handed to the backend before exiting. It expects one outgoing ping reply
and one incoming echo request by default. Its compile-time configuration can
select other IPv4 addresses or counts.

From the repository root, with the release compiler, assembler, linker and BF
macro expander built:

```sh
cargo build --release -p rvm -p rcc-native-runtime
scripts/build-net-ping.sh rvm
scripts/build-net-ping.sh native
scripts/test-net-smoke.py
scripts/test-net-smoke.py --jit
```

The smoke test uses an independent local packet oracle, with no external network
or administrator requirement. It checks both ARP/ICMP directions and rejection
of bad IP/ICMP checksums, and writes guest logs, JSON results and generated-test
packet captures under `build/network/<mode>/`.
It defaults to RVM and native; build RBF with `scripts/build-net-ping.sh rbf`
and explicitly select `--mode rbf` when testing the BF adapter.

[`vmnet.c`](../tools/network/vmnet.c) is a small native macOS command-line
helper, compiled by the host's Clang and linked against Apple's vmnet framework.
It creates a virtual Ethernet connection and relays raw frames to the runtime;
it does not execute guest code or implement guest ARP/IP/ICMP. Keeping it in a
separate process confines vmnet authorization to the helper. The runtime
manages this process for normal use; the manual
steps below are for reusing a helper across runs or debugging the transport.

For a manual macOS host attachment, start the helper in a separate terminal:

```sh
scripts/build-net-vmnet.sh
NET_DIR="$(mktemp -d /tmp/ripple-net.XXXXXX)"
sudo build/network/ripple-net-vmnet "$NET_DIR/nic.sock" host
```

The helper prints its MAC and network configuration. It uses gateway/host
`192.168.64.1`, mask `255.255.255.0`, DHCP pool through `.128`, and the example
uses static guest address `192.168.64.250`. The helper needs macOS vmnet
authorization; the VM/guest process runs as the normal user. The Unix socket is
owner-only and a sudo-launched helper assigns it to the invoking user. Stop it
with Ctrl-C to remove the virtual interface and socket. Existing socket paths
are rejected rather than unlinked. Optional fourth argument `capture.pcap`
records the helper's Ethernet traffic; capture is disabled by default.

Copy the printed socket path into another terminal and run:

```sh
scripts/test-net-live.py --socket /tmp/ripple-net.XXXXXX/nic.sock --mode rvm
scripts/test-net-live.py --socket /tmp/ripple-net.XXXXXX/nic.sock --mode native
scripts/test-net-live.py --socket /tmp/ripple-net.XXXXXX/nic.sock --mode rbf --jit
```

Replace `XXXXXX` with the actual directory. These are real host `ping` tests;
they save host output and guest logs separately and fail if either direction
does not complete. Runtime attachment is `rvm ... --net SOCKET`,
`rbf ... --net SOCKET`, or `RCC_NATIVE_NET=SOCKET executable`. A headless run can
still use an explicitly requested live NIC. RVM/RBF guest binaries built by the
script use bank size 4096 and eight data banks; native retains its 64,000-cell
compatibility layout. The build script accepts additional compiler `-D` flags.

The common transport uses Unix datagrams. The initial request is five octets
`RNET` followed by 1; the response adds six MAC octets and a big-endian 16-bit
MTU, totaling 13 octets. Later datagrams of 14+ octets are complete Ethernet
frames. Five-octet messages `RNET`+2 are heartbeat request/reply, +3 reports a
backend failure and +4 detaches a client. An unresponsive helper marks the link
down after five wall-clock seconds and records a backend error. Transport
workers own no guest descriptor pointers, so they cannot overwrite mailbox
status during other commands. No live Linux TAP helper is supplied yet; Linux
can use the local fixture or another backend speaking this transport.

## Acceptance milestones

### 1. Device contract and deterministic fixtures

Test the shared device and all three memory adapters with small, bounded cases:

- Binary payloads containing zero and 255, odd lengths, and bank crossings.
- Exact preservation of octet order and frame boundaries.
- TX buffer reuse after acceptance; invalid values and out-of-bounds buffers.
- Empty RX, undersized RX with retry, FIFO bursts and full TX queues.
- RX overflow with correct drop counters and preserved queued frames.
- No guest-time advancement from NIC commands; deterministic scheduled delivery.
- Alternate NIC, input, clock and presentation commands, checking that each
  result and device state remains correct. Deliver incoming frames and complete
  background TX work between submissions; neither may overwrite mailbox status
  or another command's outputs.

Record the input frames, expected output frames, configuration, queue statistics
and execution limits. These fixtures establish device behavior before depending
on host network configuration.

### 2. Bidirectional ping with the host OS

Use a small freestanding guest with static MAC and IPv4 configuration. Implement
Ethernet, ARP, IPv4 and ICMP echo; TCP, DHCP, routing protocols and rOS integration
are not required. Validate lengths, IPv4/ICMP checksums, echo identifier, sequence
and payload. Unsupported or malformed packets must be handled explicitly and
must not corrupt guest memory.

Run both directions against the actual host network stack:

| Direction | Required evidence |
|-----------|-------------------|
| Guest -> host | guest sends an echo request and validates the host's echo reply |
| Host -> guest | the host's normal `ping` sends requests and validates replies produced by guest code |

Exercise ARP discovery in both directions rather than relying solely on warm
neighbor caches. Use bounded request counts, explicit deadlines and payloads
that reveal corruption. Save packet captures, guest logs, checksums, wall-clock
timings and NIC counters. Record RVM, RBF and native results separately; do not
infer one host's success from another. RBF runs use small workloads and generous
ping intervals/timeouts with their actual timings reported.

### 3. Stretch goal: guest ping to syma-lang.org

`syma-lang.org` is the user's Hetzner VPS and the intended external target.
First ping its configured IPv4 address, then resolve the hostname using a
minimal guest DNS client and ping the resulting address. Do not hard-code a
current DNS result into this design document.

This stage adds a guest default gateway, ARP for that gateway, and host forwarding
with a working return route or NAT. The selected backend must carry ICMP; the
VPS/network firewall must allow echo requests and replies. For the hostname
stage, the guest constructs and parses DNS over UDP; host-side name resolution
does not establish guest DNS support.

The hostname stage is implemented by the reusable [guest networking
library](net-library.md) and
[`dns-ping.c`](../c-test/examples/network/dns-ping.c). Run:

```sh
scripts/run-net-dns.sh native syma-lang.org
scripts/run-net-dns.sh rvm syma-lang.org --jit
```

The launcher selects shared-mode vmnet with capture disabled. The guest queries
its configured DNS server over UDP, validates the reply and CNAME chain, and
pings the returned address three times with checksum, source, identifier,
sequence and binary payload validation. Native and RVM runs have both passed
this complete exchange. Use `--ip 192.168.64.249` if the default `.250` is
already in use. DNS and echo failures are reported separately. The helper is
owned and cleaned up by the runtime; no second terminal is needed for setup.

The earlier IPv4-only tool
[`scripts/test-net-vps.py`](../scripts/test-net-vps.py) remains useful for
isolating ICMP/routing failures with an explicit `--ipv4`. Its hostname option
uses **host-side** resolution and therefore does not test guest DNS. It expects
an existing `--socket` backend and saves its logs/results separately under
`build/network/vps/<mode>/`.

External connectivity is a separate integration test, not a prerequisite for
local deterministic tests to pass. The [DNS packet fixture](net-library.md#linking-and-regression-checks)
exercises successful and malformed responses without relying on Internet DNS.

## Legacy MMIO and later work

Networking and future audio can use the mailbox without reusing addresses
10–15. Migrate examples and runtime keyboard helpers toward descriptor-based
input separately, including TEXT40 behavior. Retain compatibility reads or leave
deprecated addresses reserved until an explicit memory-map revision defines
reuse; old binaries must not silently reinterpret key reads as other devices.

Later work includes DNS TCP fallback, DHCP, a Linux TAP helper,
controlled-time packet replay files, additional NICs, optional batching and a
future OS's driver/serialization policy. These additions must preserve the
queue, octet representation and mailbox-sharing requirements above.

## References

- [Existing MMIO/device contract](../rvm/mmio.md#device-command-abi-version-1)
- [Linux TUN/TAP documentation](https://docs.kernel.org/networking/tuntap.html):
  the distinction between IP-packet and Ethernet-frame host interfaces.
- [QEMU network backend documentation](https://www.qemu.org/docs/master/system/devices/net.html):
  separation of guest NICs from host network attachment.
