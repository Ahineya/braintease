# Guest HTTP client

`<net_http.h>` supplies a bounded, polling HTTP GET client and a transport-independent
response decoder. DNS, TCP and HTTP all execute in guest C on native and RVM;
the host backend only carries Ethernet frames. It uses the existing DEVICE_*
contract and ISA, with one wire octet per cell and no heap allocation.

## Run it

On macOS, with the [release toolchain](running.md#build-prerequisites) built:

```sh
scripts/run-net-http.sh
scripts/run-net-http.sh rvm --jit
```

The launcher builds the guest, creates an ordinary temporary host HTTP service,
acquires a guest DHCP lease and prints a 13,033-octet HTML response. This exceeds
the 8192-cell guest bank and every TCP/application buffer. The service binds only
to `192.168.64.1:8080`. Host-only vmnet starts automatically on guest NIC discovery;
only its helper needs authorization, and packet capture stays disabled. The
service and owned network helper are removed after the run. No other terminal
or network setup command is needed.

Output includes the guest's acquired address, URL, response status/framing,
the streamed body, and a summary:

```text
HTTP local=<leased IP> GET http://192.168.64.1:8080/
HTTP response status=200 length=13033
HTTP BODY
<!doctype html>
...
HTTP END
HTTP OK status=200 received=13033 checksum=1097199
```

`--port 8081` chooses another temporary service port. `--build-only` produces
`build/network/http-get/<mode>/http-get` or `http-get.bin` without networking.
The launcher isolates runtime builds so concurrent work cannot change its bank
settings. `--timeout SECONDS` is the wall-time bound (default 60); the guest
request budget is half that, capped at 30 seconds, leaving time for DHCP/cleanup.

For an **explicitly supplied cleartext HTTP endpoint**:

```sh
scripts/run-net-http.sh rvm --jit --shared --host your-http-host.example --port 80 --path /hello
```

Replace that example hostname with the actual configured service. `--host`,
`--path`, `--shared` and `--socket /actual/nic.sock` disable the temporary server;
`--no-server` does so explicitly. `--static` uses the demo's compiled NetConfig
instead of DHCP and requires `--no-server`. Hostname, port and path are build-time
settings, not a new runtime argument ABI. The guest resolves names itself.
Host-only vmnet does not advertise a recursive DNS server: use shared attachment
or explicit static DNS configuration for named external endpoints. The launcher
keeps host-only networking as its default.

This version uses plain HTTP. HTTPS URLs, TLS, redirects, chunked responses and
persistent connections are outside its supported subset. A remote redirect is
returned as a status/body; the client does not silently follow it or downgrade
an HTTPS destination. No public VPS HTTP endpoint is assumed or configured by
this milestone. RBF remains compatible at the ISA/library level but was not
built or timed for HTTP; the routine launcher/test matrix is native and RVM.

## Ownership and polling

Initialize a `NetContext` with static configuration or DHCP. Allocate a zeroed,
caller-owned `NetHttp` at a stable address; it owns its DNS query, TCP connection,
request and response state. Do not reset/reinitialize its network while active.
Host and path strings are copied during start and may be reused afterward;
input configuration/strings must not alias the destination object being reset.

```c
#include <net_http.h>

static NetHttp fetch;
static NetHttpConfig request = {
    "192.168.64.1", "/", 8080,
    0,           /* replace with caller-supplied unpredictable TCP ISN */
    30000UL,     /* total request deadline, milliseconds */
    1048576UL    /* maximum body octets; zero allows empty bodies only */
};
static unsigned char body[173];

int begin(NetContext *network, unsigned long now_ms, unsigned long isn) {
    request.initial_sequence = isn;
    return net_http_start(network, &fetch, &request, now_ms);
}
```

`net_http_start` validates the request, then starts DNS or a direct IPv4 TCP
connection. `NET_OK` means started, not fetched. A hostname is at most 253 ASCII
characters with DNS labels at most 63; IPv4 literals bypass DNS. The port is
1..65535, the path is an already escaped ASCII origin-form starting `/`, at most
255 characters, without spaces, controls, backslashes or a fragment. The library
does not parse a full URL or encode a path. Invalid input returns
`NET_BAD_ARGUMENT`; an active query/connection, including TIME_WAIT, returns
`NET_BUSY`. The underlying TCP API retains its [ISN/entropy policy](tcp.md#supported-subset-and-security).

For each iteration:

1. Call `net_poll(network, now_ms)`, or `net_dhcp_poll` if DHCP owns the context.
2. Call `net_http_poll(&fetch, now_ms)`; check its status.
3. Call `net_http_read(&fetch, body, sizeof(body), &count)`. Consume **count even
   when this call also returns EOF or an error**, then check the status. Repeat
   on `NET_OK` until `NET_AGAIN`, EOF or failure; poll again to receive more.

`NET_AGAIN` means pending progress/backpressure. `NET_OK` from read means a
nonempty body prefix. `NET_EOF` means the whole framed body is complete; the
last read can return both bytes and EOF. `fetch.response.status` holds the HTTP
status once response headers are complete (`response.state` BODY or DONE).
A 404 or 301 is a valid HTTP response, not a network error; its body is available.

The total 1..600000 ms deadline includes DNS, handshake, request writes, headers,
body reads and caller backpressure. Slow trickles do not extend it. DNS uses
up to three attempts, at most 3000 ms each, inside the total budget. An IPv4 A
result is connected through the same TCP layer. DNS errors return
`NET_HTTP_DNS_ERROR`, with the detailed `fetch.dns.status` preserved. TCP
reset/link/configuration/timeout errors retain their ordinary `NET_*` statuses.
Use the same wrapping 32-bit clock as the other guest protocols; paced ticks
are required for the example's live-network timing.

Body completion initiates graceful TCP shutdown. **Body EOF is separate from
TCP resource release**: continue network/HTTP polling while `fetch.tcp.active`
is true, including TCP's 120-second TIME_WAIT if this endpoint actively closes.
`cleanup_error` records a later teardown failure without retracting an already
complete body. `net_http_cancel` explicitly cancels DNS/TCP, discards buffered
input, marks the operation cancelled and releases its slot. Cancelling TIME_WAIT
gives up tuple-reuse protection; do not free/move an active object instead.
The one-shot demo waits up to three seconds for close, then explicitly cancels
remaining transport state before draining accepted host TX and exiting.

## Supported response framing and limits

| Item | Behavior |
|------|----------|
| Request | `GET <path> HTTP/1.0`, Host including nondefault port, Connection: close, Accept-Encoding: identity |
| Status | HTTP/1.0 or 1.1, final 200..599; status retained for the application |
| Headers | Strict CRLF, case-insensitive names, trimmed space/tab values, no folding or whitespace before colon |
| Content-Length | Checked unsigned 32-bit decimal, one field only; duplicate/list/conflicting/negative/overflow values fail |
| Close-delimited body | Stream until orderly TCP EOF; a reset is an error |
| Empty body | 204 and 304; 204 with Content-Length is rejected; 304 length describes a representation and is not consumed |
| Unsupported | Informational 1xx/upgrade, any Transfer-Encoding, nonidentity Content-Encoding; explicit NET_UNSUPPORTED |
| Line/header bounds | 255 cells per line excluding CRLF, 4096 total header octets including status/CRLF, 64 fields |
| Body bound | Caller-selected unsigned 32-bit max_body; streamed, never allocated as one buffer |
| Buffering | 256-cell HTTP input, 768-cell request, 256-cell line; underlying TCP has 512-cell TX/RX |

Unknown syntactically valid headers are ignored. This client does not pipeline
or reuse a connection: the decoder stops exactly at the response boundary and
the client discards following bytes on teardown. With a declared length,
premature orderly EOF is `NET_HTTP_TRUNCATED`; malformed status/headers/lengths
are `NET_HTTP_BAD_RESPONSE`; line/header/body bounds are `NET_HTTP_LIMIT`.
A close-delimited response has no independent advertised length against which
to detect a server's accidentally shortened body. That is a limit of this
framing, not grounds to label a reset or deadline as successful EOF.

The conservative duplicate-length rejection is deliberate, including identical
values. Chunk decoding and interim responses can be added as tested extensions;
the client never prints their syntax as if it were a decoded body.

## Decoder without a connection

`net_http_parser_init(parser, max_body)` initializes a caller-owned decoder.
`net_http_parser_feed` accepts an input slice and output buffer and returns both
consumed input and written body counts. Preserve any unconsumed suffix. It can
consume headers with output capacity zero, and stops at a full body output
buffer or the exact message boundary. `NET_EOF` means complete framing;
`NET_AGAIN` means more input or output space is needed. Errors remain sticky.
Call `net_http_parser_eof` only when the input transport actually reaches orderly
EOF; this completes close-delimited bodies or reports truncation.

The implementation is split into `http.c` (DNS/TCP orchestration),
`http_request.c` (request validation/serialization), `http_parse.c` (incremental
framing), `http_fields.c` (shared CRLF/fields), and `http_ascii.c` (shared token,
case-folding and bounded decimal helpers). The [server](http-server.md) uses
these same utilities for request parsing and response serialization. The demo contains only configuration,
network/time driving, body output and exit handling.

See [HTTP tests](testing.md#http) for deterministic parser/stream tests,
independent DNS/TCP packets and ordinary host-service interoperability.
Protocol references: [HTTP/1.0, RFC 1945](https://www.rfc-editor.org/rfc/rfc1945.html)
and [HTTP framing, RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html).
