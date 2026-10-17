# RFL networking library

`std/net` and `std/http` wrap the existing guest C networking stack. The same
ARP, DHCP, IPv4, UDP/DNS, TCP and HTTP code runs on native and Ripple. Native
execution still exchanges Ethernet frames through DEVICE_*; it does not replace
guest protocols with host DNS or HTTP APIs. The ISA and octet-per-cell layout
are unchanged.

This first layer provides owned networks/reactors, DHCP, IPv4 name lookup,
buffered HTTP GET, and a small HTML server. Direct RFL UDP/TCP streams,
streaming bodies and richer HTTP headers/methods remain follow-ups.

## Fetch a page

[http-get.rfl](../../rfl/examples/http-get.rfl) is a complete application:

```sh
target/release/rfl run rfl/examples/http-get.rfl --target native
# Or run the same source in RVM:
target/release/rfl run rfl/examples/http-get.rfl
```

The example obtains a DHCP lease, prints its IP, and requests `/` from
`192.168.64.1:8080`. Point the request at your HTTP service. To obtain a native
executable, use:

```sh
target/release/rfl compile rfl/examples/http-get.rfl --target native -o build/http-get
build/http-get
```

The runtime initializes networking when the guest queries its NIC, following the
same [backend selection](running.md) as C programs. No `--visual` or explicit
socket setting is required for ordinary automatic vmnet networking. Host-only
networking reaches the host; Internet destinations require a shared/NAT backend
and a working guest DNS configuration. DHCP is guest code, not a host-supplied IP.

The central API, inside an async function with a live network reactor, is:

```rust
import {Request} from "std/http";
import {Future} from "std/future";

let response = client.send(
    Request::get("example.com", "/")
        .with_port(80)
        .with_timeout(30000)
        .with_limit(65536)
).await?;
println(response.status);
println(response.text());
```

`Request::get(host, path)` accepts a hostname or dotted IPv4 address and an
origin-form path starting with `/`. These are separate strings, not a URL; do
not include `http://` in the host. Defaults are port 80, a 30-second total
transaction timeout and a 65,536-octet body limit. Host/path limits are 253/255
ASCII octets; the C parser performs the remaining syntax validation. Timeouts
must be 1..600,000 ms; maximum body size is 1,048,576 octets.

`HttpClient.send` returns a lazy, move-only `Async<Result<Response,NetError>>`.
Creating it retains its inputs but starts no DNS query or connection. Await it,
spawn it in a task scope, or drive it with `block_on`. The outer result from
`block_on` describes executor failure; its inner result describes the HTTP
operation. Import `Future` when using `.await`.

`Response` has `status:u16` and `body:Vec<u8>`. `is_success()` means 200..299;
a 404 or redirect is an ordinary response, not a transport error. `text()`
converts octets using RFL's existing String semantics, with balanced
concatenation rather than repeated copying of the entire body. It is not an
HTTP charset decoder. Binary bodies remain available as `Vec<u8>`.

The client sends HTTP/1.0 with Host, Connection: close and Accept-Encoding:
identity. Content-Length and connection-close framing are supported. It does
not follow redirects, decode chunked/compressed bodies, expose response headers,
reuse persistent connections or provide HTTPS. Unsupported/malformed framing,
size violations and truncation are explicit errors. See the shared
[HTTP contract](http.md) for parser and transport details.

## Configure and drive a network

```rust
import {dhcp_network} from "std/net";
import {Reactor,block_on} from "std/executor";
import {http_client} from "std/http";

let network = dhcp_network(0, 12345).unwrap();
let mut reactor = network.reactor().unwrap();
let mut acquire = network.acquire(30000);
let config = block_on(&mut acquire, &mut reactor).unwrap().unwrap();
println(config.address.text());
let client = http_client(&network, 305419896).unwrap();
```

Interface and seed are explicit. The client/listener also take an application
sequence seed, matching the C stack's policy. **The example constants and
subsequent deterministic mixing are not cryptographic entropy.** This is the
bounded development stack described in [TCP](tcp.md), not a hardened Internet
transport. A stronger entropy/sequence policy remains shared C-stack work.

`NetworkConfig` contains `interface`, `seed`, `address`, `mask`, `gateway` and
`dns`. The last four use `Ipv4`; construct one with `ipv4(a,b,c,d)`. Use
`static_network(config)` instead of DHCP for a deliberately assigned address.
`network.configuration()` returns the current configuration, or an error while
unconfigured. `network.resolve(name, timeout_ms)` returns a lazy future yielding
`Result<Ipv4,NetError>` using guest UDP/DNS. DNS timeouts apply per attempt
(1..60,000 ms), with two retries; HTTP additionally has its total transaction
limit. Dropping unfinished DHCP stops that acquisition. Once acquired, the
reactor continues DHCP polling for renewal/rebinding until stopped.

Exactly one `NetworkReactor` drives a Network's shared context. It checks real
host monotonic time, polls at most four incoming frames per service call, then
services bounded HTTP/DNS/listener registries. Idle waits are bounded to 1 ms;
operation futures register timer wakes instead of assuming NIC interrupts.
No background worker progresses guest protocols: keep driving this reactor,
even when application tasks are waiting or only graceful closes remain.
`NetworkReactor.stop()` and reactor destruction stop new work and cancel active
protocol operations. A stopped context cannot be restarted by obtaining a new
reactor. Independent network contexts on the same NIC do not demultiplex each
other's frames: use `Network.clone()` to share the existing one instead.

`Network`, `HttpClient`, `HttpServer`, `IncomingRequest` and operation futures
are move-only. `Network.clone()` explicitly creates another lease to the same
C storage; it does not open another NIC. Operations retain that storage, so
cancellation after driver/outer-network destruction cannot access freed memory.
C objects live in stable heap storage, never inside moving RFL/COW values.
Keep the `state`, `guard`, `session` and other implementation fields private to
library usage; they are not a supported raw-handle API.

Abandoning a pending request aborts only its operation and releases its buffers
and port. Abandoning an incoming request aborts its unanswered connection.
Cancellation cannot undo bytes already transmitted. Successful client reads
return when the HTTP body ends, while the reactor finishes TCP close in the
background. TIME_WAIT tuples move into the existing compact table before live
slots are reused. Before a one-shot application drops its reactor, optionally
await `network.drain_closing(timeout_ms)`; this waits for already closing
connections (including automatic server error responses), not for the
120-second TIME_WAIT quarantine to expire. An expired
wait returns an error without silently treating incomplete close as success.

There are four HTTP client job slots, four DNS job slots and one RFL HTTP
listener backed by four server sessions per context. They share the C stack's
four TCP connections/four UDP bindings and 32 compact TIME_WAIT entries.
DNS/DHCP/HTTP can therefore exhaust a shared underlying limit before their
individual wrapper limit; failures remain explicit. Body buffering allocates
guest heap memory. A server response also owns a stable C copy while sending.
The registry allocates its server pool only when listening is requested.

`NetError` distinguishes `BadArgument`, `NoMemory`, `NetworkFailure(code)`,
`DnsFailure(code)` and `ReactorUnavailable`. `.message()` gives a short readable
description; numeric codes retain the shared C headers' detail. Allocation and
capacity failures do not produce empty successful responses.

## Serve HTML

[http-server.rfl](../../rfl/examples/http-server.rfl) obtains a lease and prints
its actual `http://<ip>:8080/` address:

```sh
target/release/rfl run rfl/examples/http-server.rfl --target native
# Or use RVM:
target/release/rfl run rfl/examples/http-server.rfl
```

Open the printed address in a browser on the host.

Inside its async service function:

```rust
let request = server.accept().await?;
println(request.method + " " + request.path);
move(request).respond(Response::html(200, "<h1>Hello from RFL!</h1>\n")).await?;
```

Create the server with `http_server(&network, port, sequence_seed)`. Accept is
lazy and yields an owned `IncomingRequest` containing copied `method`, `path`
and `host` strings. Respond consumes that request, owns the response across
suspension, and completes after graceful TCP close reaches CLOSED/TIME_WAIT.
It does not merely report that the body was queued. Poll/copy work is bounded
in 256-octet chunks; cancellation during a copy cannot transmit uninitialized
body storage. Dropping the listener prevents new accepts; already claimed
requests keep their leases. Old accept futures fail after listener replacement
instead of accepting from a new listener generation.

`Response::html(status,text)` produces an exact-length body. Supported status
codes are the C server's 200, 400, 404, 405, 408, 413, 414, 417, 431, 501 and 505.
The example returns 404 for an unknown path and 405 for a non-GET method. The
shared server bounds request/header parsing and phase timeouts, rejects
unsupported bodies/transfer framing, and closes after each response. It does
not implement uploads, pipelining, keep-alive, arbitrary response headers or TLS.
See [HTTP server](http-server.md) for those exact bounds.

## Validation and follow-ups

Run the independent packet fixtures (no vmnet, host DNS or Internet required):

```sh
python3 scripts/test-rfl-network.py
python3 scripts/test-rfl-network.py --mode rvm --jit --skip-build
MAKEFLAGS=-j4 ./rct test_rfl_net_ownership test_net_tcp_retire --backend rvm --timeout 90
```

The Python runner builds the checked-in RFL fixtures; `--skip-build` reuses them.
It verifies guest DNS, all 10,230 response octets/checksum and graceful client
close. The server serves six connections, exceeding the live pool, against loss,
reordering, zero windows and a corrupt packet. DHCP coverage checks acquisition,
three ARP probes and the leased DNS address before the HTTP request. The C
bridge tests cover capacity/reuse, refcount overflow, unfinished DHCP cleanup,
shutdown with surviving owners, and stale listener epochs. The separate TCP
retirement test covers compact-table backpressure and duplicate FINs. Together
they pass 50 assertions on native and RVM.

These first wrappers intentionally use the working owned async machinery.
Generated VM programs remain large; compiler reachability/frame-layout and
allocator work should be measured before calling this a lightweight embedded
HTTP library. Streaming bodies, direct typed UDP/TCP, configurable listener
policies, event-aware waits and richer HTTP build on this ownership boundary.
Routine RBF network runs remain opt-in; no new RBF result is claimed here.
