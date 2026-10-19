# Build and run networking examples

Run commands below from the repository root. This guide describes existing
launchers, including [TCP streams/listeners](tcp.md), the [HTTP GET client](http.md)
and the [guest HTTP server](http-server.md).

## Build prerequisites

The launchers compile the C application and protocol sources, but expect the
release compiler, selected execution runtime and assembler/linker to exist.
For native and RVM examples:

```sh
cargo build --release -p rcc-driver -p rcc-native-runtime -p rvm
cargo build --release --manifest-path src/ripple-asm/Cargo.toml --bin rasm --bin rlink
```

RBF additionally needs `target/release/rbf` and
`src/bf-macro-expander/target/release/bfm`; those builds are only needed when
explicitly selecting RBF. Networking needs neither Doom nor rOS to be built.

## Serve a page from the guest

```sh
scripts/run-net-http-server.sh
scripts/run-net-http-server.sh rvm --jit
```

The guest acquires DHCP, starts its TCP listener and prints its actual HTTP URL.
Open that URL in your browser; no host HTTP service or manual network setup is
required. Ctrl-C stops it. `--port 8081` changes the guest port, `--build-only`
only compiles, and `--test` runs automatic curl/error/reuse tests then exits.
See the [server guide](http-server.md) for its bounded API, pool ownership,
TIME_WAIT preservation and supported methods/framing.

## Fetch HTTP

```sh
scripts/run-net-http.sh
scripts/run-net-http.sh rvm --jit
```

The launcher acquires a guest DHCP lease and fetches/prints a 13,033-octet HTML
page from a temporary ordinary host HTTP service. Host-only networking and
service cleanup are automatic; capture is disabled. `--port 8081` selects
another temporary port, and `--build-only` only compiles. See the
[HTTP guide](http.md) for an explicitly supplied external cleartext endpoint,
guest DNS, build-time host/path configuration and supported framing/limits.
The guest [HTTP server](http-server.md) uses the same TCP/HTTP utility layers.

## Try guest TCP

```sh
scripts/run-net-tcp.sh
scripts/run-net-tcp.sh rvm --jit
```

This starts a temporary ordinary host echo service, acquires a guest DHCP lease
and checks a 4,097-octet binary stream and orderly connection close. Host-only
vmnet starts automatically with capture disabled. See the [TCP guide](tcp.md)
for external peers, port selection, buffer ownership and supported limits.

## Acquire configuration through DHCP

```sh
scripts/run-net-dhcp.sh
scripts/run-net-dhcp.sh rvm syma-lang.org --jit
scripts/run-net-dhcp.sh --monitor
```

These run the guest DHCP client without a compiled-in IP, mask, router or DNS
server. The first two use shared-mode vmnet, print the lease, then perform guest
DNS and three pings. The monitor uses host-only vmnet, prints its leased address
and keeps renewing while answering/logging host pings. Ping that printed IP to
generate traffic; no host setup commands or static IP selection are required.
Capture remains disabled. Builds are isolated under
`build/network/dhcp-ping/<mode>/` and `build/network/dhcp-monitor/<mode>/`.

Acquisition includes a randomized initial delay and ARP conflict probes, so
allow several seconds before the bound line. A missing DHCP server produces
an explicit acquisition failure after the configured 60-second budget, with no
static fallback. See [DHCP](dhcp.md) for the API, lease lifecycle, options and
limitations. The following older launchers retain explicit static configuration.

## Try guest DNS and ping

From the repository root, with the release toolchain built:

```sh
scripts/run-net-dns.sh
scripts/run-net-dns.sh rvm syma-lang.org --jit
```

The launcher builds `c-test/examples/network/dns-ping.c`, then runs it. On macOS
it explicitly selects shared-mode vmnet/NAT with capture disabled. Networking
starts when guest C discovers NIC 0; only the native vmnet helper requires sudo.
The guest sends a DNS request to the configured server, validates the response,
prints the returned IPv4 address and TTL, and sends three ICMP echo requests.
No host DNS lookup is used. A successful run prints:

```text
DNS query name=syma-lang.org local=192.168.64.250 server=1.1.1.1
DNS answer name=syma-lang.org address=<guest-resolved IPv4> ttl=<seconds>
PING reply seq=1 octets=17
PING reply seq=2 octets=17
PING reply seq=3 octets=17
PING OK received=3 responded=0 rejected=0 drops=0
```

`--ip 192.168.64.249` avoids a conflict if another guest owns `.250`.
`--dns a.b.c.d` selects another DNS server; the default is `1.1.1.1`.
`--socket /actual/nic.sock` reuses an existing Ethernet backend instead of
starting vmnet. `--build-only` produces an executable without starting any
network. RBF remains available explicitly with `rbf --jit`, but is excluded
from routine tests because of its cost. No host TAP backend is supplied for
Linux yet; the local packet fixture works without vmnet.

The native output is `build/network/dns/native/dns-ping`. It retains the normal
runtime policy when launched directly: host-only networking by default on
macOS. For Internet access and paced timeout ticks, run it with:

```sh
RCC_NATIVE_NET=shared RCC_NATIVE_PACED=1 build/network/dns/native/dns-ping
```

The launcher already supplies those settings. Native and VM code still use
the same guest memory layout and device ABI.

## Where the addresses and hostname come from

The Ethernet backend supplies the guest MAC through `NET_INFO`. The DHCP
launcher obtains IPv4, mask, gateway and DNS through guest DHCP packets. The
static launchers instead supply `NetConfig`: their defaults are `192.168.64.250/24`, gateway
`192.168.64.1` and DNS `1.1.1.1`. The host-only/shared backend selection does
not itself configure the guest IP stack. [DHCP](dhcp.md) runs in guest C.

The DNS/ping launcher's positional hostname is a **build-time** setting. For
`scripts/run-net-dns.sh rvm example.org`, it compiles the C demo with
`DNS_NAME="example.org"`; the default is `syma-lang.org`. Guest code passes
that string to `net_dns_start`, sends its own UDP query, and pings the returned
A address. The launcher does not resolve the name and this demo does not read
it from guest command-line arguments. Runtime application configuration is
[separate, deferred work](roadmap.md#separate-and-later-work).

## Automatic runtime attachment

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

If an attachment that previously worked loses its worker/helper, a later
`NET_INFO` first reports link-down, then schedules background reconnection.
Retries back off to a thirty-second maximum interval. Recovery discards queued
packets from the old attachment and preserves error counters. The guest's link
lifecycle invalidates existing connections; applications must open new ones.
On macOS recovery uses only noninteractive `sudo -n`: expired authorization is
reported in the host log, never requested through an active guest console.
External socket helpers can also reconnect after they become available again.

For an already built monitor, simply run:

```sh
RCC_NATIVE_PACED=1 build/network/monitor/native/monitor
target/release/rvm build/network/monitor/rvm/monitor.bin --bank-size 4096 --memory 32768 --clock paced --jit
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
sharing implicitly. The guest still configures its own IPv4 address, through DHCP or explicit
static configuration. The static demo uses `192.168.64.250/24` with host/gateway
`192.168.64.1`.

RVM and RBF also accept `--net POLICY --net-eager` to prepare the link before
guest execution or terminal input starts. This keeps sudo authorization from
competing with an interactive guest's keyboard reader. Native programs expose
the same early attachment through `RCC_NATIVE_NET_EAGER=1`.
The [Fuse-X launcher](../../fuse-x/README.md#networking-and-remote-shells) uses
early attachment for interactive consoles and defaults to shared/NAT internet access
on macOS (offline elsewhere).
The Riplix launcher uses
this for host/shared policies, and defaults to shared/NAT with internet access. Use
`--net off` for an offline Riplix launch. See
[Riplix networking and remote terminals](../../ports/fuzix/README.md#networking-and-remote-terminals)
for guest `ping`, `ifconfig`, `telnet` and `telnetd` setup.

Orderly exit detaches and stops the owned helper. If a guest process dies without
running destructors, the managed helper stops after its five-second heartbeat
timeout, removes its interface/socket and cleans its private files after dropping
to the invoking user's privileges. External helpers retain their manual lifetime.
The C test runner explicitly disables automatic networking for offline tests.
Library-created device states and restored snapshots are also offline until the
embedding host configures them. RBF uses the same policy, with automatic networking
disabled for checkpoint runs; attached live NIC state remains non-checkpointable.

## Interactive ping monitor (macOS)

From the repository root, run:

```sh
scripts/run-net-monitor.sh
```

This builds [`monitor.c`](../../c-test/examples/network/monitor.c) with RCC's native
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

Monitors now advance the supplied clock through paced ticks, allowing neighbor
expiry and periodic link checks. Static monitors keep their configured IP across
an outage, log the transition and resolve neighbors again after restoration.
DHCP monitors remove the lease and acquire a fresh one. See
[reliability and lifecycle](reliability.md) for operation cancellation and limits.

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

## Bounded protocol tests and manual attachment

The C API is [`<vm_network.h>`](../../runtime/include/vm_network.h). Like other
device helpers, it returns mailbox status first and updates output objects only
on success. Applications must then check the operation outcome. The equivalent
raw command/outcome constants are exported by `rfl/std/mmio.rfl`.

[`ping.c`](../../c-test/examples/network/ping.c) uses the guest
[`<net.h>` library](library.md), which constructs and validates ARP, IPv4
and ICMP packets in guest C, including checksums, odd-length payloads and
zero/255 octets. It polls independently of ticks and waits for accepted TX work
to be handed to the backend before exiting. It expects one outgoing ping reply
and one incoming echo request by default. Its compile-time configuration can
select other IPv4 addresses or counts.

With the release compiler, assembler/linker and selected execution runtimes
built:

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

[`vmnet.c`](../../tools/network/vmnet.c) is a small native macOS command-line
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

