# Guest HTTP server

The guest serves a hardcoded HTML page over its own TCP listener. An ordinary
browser or curl connects to the guest IP; the host backend transports Ethernet
and does not run a replacement HTTP server. The implementation is shared guest
C, with the existing DEVICE_* contract, ISA and one octet per cell.

## Run and open the page

On macOS, with the [release toolchain](running.md#build-prerequisites) built:

```sh
scripts/run-net-http-server.sh
scripts/run-net-http-server.sh rvm --jit
```

Host-only networking starts automatically on NIC discovery, with capture off.
The guest obtains its own DHCP lease and prints the URL:

```text
HTTP SERVER DHCP acquiring
HTTP SERVER ready http://<leased IPv4>:8080/
```

Open that actual URL in your browser. Reloading opens another guest connection.
No address selection, helper command or second-terminal setup is required.
Ctrl-C stops the server; the runtime-owned helper cleans itself up too. If you
prefer curl, use `curl --noproxy '*' http://<leased-IP>:8080/`.

`--port 8081` chooses another guest port, `--build-only` compiles without starting
networking, and `--static` uses the demo's explicit NetConfig instead of DHCP.
`--socket /actual/nic.sock` selects an external Ethernet peer and leaves its
lifetime alone. Ports below 1024 are allowed: this is a guest TCP listener, not
a privileged host socket. Builds are isolated under
`build/network/http-server/<mode>/` and use 16384-cell banks.

For automatic host validation followed by guest/helper cleanup:

```sh
scripts/run-net-http-server.sh --test
scripts/run-net-http-server.sh rvm --jit --test
```

These use curl plus ordinary host sockets for valid, malformed, split, stalled
and repeated requests. Tests require the default guest DHCP/host-only setup.
The normal server keeps running; `--test` is the bounded automated alternative.
`rbf --jit` is an explicit Brainfuck build/run option, excluded from automatic
HTTP tests and not built or validated for this milestone.

## Behavior and limits

The example in `c-test/examples/network/http-server.c` supplies routing and
immutable HTML in `http-page.h`. `GET /` returns the 984-octet page, an unknown
path returns 404, and an unsupported method without a request body returns 405
with `Allow: GET`. The request target is compared exactly: queries are not
stripped or decoded by this small router.

The library in `<net_http_server.h>` parses HTTP/1.0 and HTTP/1.1 request lines
and headers incrementally. A line, CRLF or header delimiter can cross any read.
HTTP/1.1 needs exactly one syntactically valid Host field; duplicate/empty Host,
folded fields, whitespace before colon, controls and invalid decimal lengths
are rejected. HTTP/1.0 permits an absent Host, but validates one if supplied.
The parser accepts ASCII DNS/IPv4-style authorities and an optional numeric
port, not bracketed IPv6. It accepts an origin-form target starting `/`, not an
absolute URL or a fragment. It preserves the method and target for application
routing and never interpolates them into response headers or HTML.

| Input/condition | Response or behavior |
|-----------------|----------------------|
| Valid request | Application chooses a response after READY |
| Malformed syntax/Host/length, duplicate Content-Length, premature request EOF | 400 |
| Request deadline, including a slow trickle or waiting for the application | 408 |
| Nonzero request Content-Length | 413; no request bodies in this version |
| Request line over 255 cells excluding CRLF | 414 |
| Expect header | 417; no interim 100 response |
| Header line over 255 cells, more than 64 fields or 4096 total header octets | 431 |
| Transfer-Encoding | 501; no chunked request decoding |
| Well-formed unsupported HTTP version | 505 |
| TCP reset, link/configuration loss, stalled transmission/close | Session fails and releases through the application |

Every response is HTTP/1.0 with an exact unsigned 32-bit Content-Length,
`Content-Type: text/html; charset=us-ascii` and `Connection: close`. Writes may
be partial. The application body is borrowed immutable storage until completion
or failure; the library never buffers the entire page. The example exceeds
TCP's 512-cell buffer and transmits it in several segments. No request bodies,
uploads, filesystem content, TLS, keep-alive, pipelining or HTTP/2 are provided.
Bytes after the first parsed request are drained/discarded without extending
any deadline; they never become a second request on that connection.

## Listener ownership and connection reuse

See the [TCP listener API](tcp.md#listen-and-accept). The example has a two-TCB
caller-owned pool: one application session is serviced at a time, with room for
a pending handshake/connection. All live pool members share the context's
four-connection ceiling. The listener polls pending, accepted and closing
connections, while the application explicitly releases accepted objects after
close/failure. Half-open and unaccepted work has a bounded lifetime.

An active TCP closer normally retains its full TCB through 120 seconds of
TIME_WAIT. That would exhaust a small HTTP server after a few browser reloads.
`net_tcp_wait_attach` optionally attaches a caller-owned `NetTcpWaitTable` with
32 compact records. `net_tcp_release` can move an accepted TIME_WAIT connection
into this table and immediately return its large buffers to the pool. Delayed
packets cannot open the old tuple, and a duplicate FIN gets its final ACK and
restarts that record's timer. Other tuples can use the freed TCB immediately.

The records live independently of the listener and its pool, so stopping a
listener does not invalidate already preserved tuples. Keep the table stable
while attached. Replacing/detaching a table with live records returns NET_BUSY;
expiration or configuration/link-generation changes clear stale records. A full
table causes release backpressure, never eviction of an unexpired tuple. With
no table attached, release waits for the full TCB's TIME_WAIT to expire. Ordinary
ping/DNS/client applications allocate no compact table and do not scan one on
their receive path.

An explicit abort gives up normal TIME_WAIT protection; the HTTP demo does not
use abort to accelerate successful reloads. Failed/timed-out sessions are
cancelled, then their accepted TCB is released. The finite table and pool are
resource limits, not a claim of resistance to SYN floods or Internet hardening.
The demo's clock/MAC/counter ISNs are demonstration entropy; production callers
provide their own unpredictable sequence callback.

## Session API

```c
#include <net_http_server.h>

static NetHttpServer session;
static const unsigned char html[] = "<html><body>Hello!</body></html>\n";

/* After net_tcp_accept returns a stable pool connection: */
int begin(NetTcp *tcp, unsigned long now_ms) {
    return net_http_server_start(&session, tcp, now_ms, 5000UL);
}

int drive(unsigned long now_ms) {
    int status = net_http_server_poll(&session, now_ms);
    if (session.state == NET_HTTP_SERVE_READY)
        return net_http_server_reply(&session, 200, html, sizeof(html) - 1);
    return status;
}
```

Start with a zeroed session and an established/CLOSE_WAIT TCP stream. Drive
`net_poll` or `net_dhcp_poll`, then `net_tcp_listener_poll` (or individual TCP
polls), then each HTTP session. HTTP server poll does not independently drive
TCP or receive raw NIC frames. A completed/failed session object can be reused
for a new connection, including a new generation at the same pool address.
Do not reuse it while a request/response is active.

READY exposes `request.method`, `request.target`, `request.host` and version
10/11. `net_http_server_reply` accepts the documented response statuses and an
immutable body; an application may choose its own 200/404/405 routing. Parser
errors and request deadlines produce a small fixed error page automatically
where the transport still permits it. `session.status` is the response status;
`session.error` records a transport/session failure, not a 404 or 405.

The supplied 1..600000 ms timeout applies separately to request/application
waiting, response submission and connection shutdown, using wrapping 32-bit
milliseconds. Progress does not extend those absolute phase deadlines. Configure
TCP's progress timeout **longer** than the HTTP request timeout if the server
should have time to send a 408. The example uses 5 seconds per HTTP phase and
15 seconds for TCP/pending handshakes. This is distinct from TIME_WAIT retention.

NET_AGAIN means pending work, NET_OK in READY means the application can reply,
and NET_EOF means closed or TIME_WAIT. Once HTTP reports EOF, call
`net_tcp_release`; retry NET_BUSY while continuing listener polling. A failure
aborts its borrowed stream; the application still calls release. Session code
does not free another layer's pool object. Explicit
`net_http_server_cancel` likewise leaves pool release to its owner.

`net_http_request_init/feed/eof` also exposes the standalone request parser:
preserve unconsumed input and stop at the first request boundary. Its
`request.header.state` is DONE on success or PARSE_FAILED with a sticky error;
`request.status` supplies the recommended error response. Shared CRLF/field,
ASCII and decimal helpers are used by the client and server. Request parsing,
response serialization and session progress are separate small modules.

[Tests and recorded validation](testing.md#tcp-listeners-and-http-server) cover
parser splits, binary/partial writes, real host clients, loss/reordering,
backpressure, malformed input, deadlines and resource reuse. RBF was not built
or timed for this milestone. Protocol references: [TCP, RFC 9293](https://www.rfc-editor.org/rfc/rfc9293.html)
and [HTTP message framing, RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html).
