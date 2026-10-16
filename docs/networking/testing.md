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
