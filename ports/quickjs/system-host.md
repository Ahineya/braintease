# JavaScript system host

The experimental terminal host runs on both native Fuse-X and RVM/Riplix.
It uses the same QuickJS engine and optional POSIX capability layer on each.
The ordinary host reference builds against the pristine upstream engine.
This is a small system runtime, not a Node-compatible implementation.

## Try the utility

Build with `python3 ports/quickjs/build.py --target both`; this creates
`build/quickjs-cli/native/js.fxe` and `build/quickjs-cli/ripple/js.rxe` without
changing installed disk images. `--compiler` selects RCC and `--build` selects
another build directory. `--target host` builds `js-host` with `--cc`/`CC`.

The development milestone is also available in `fuse-x-shared`: the native
executable is `js`, with the utility and sample files in `js-system`. The
matching editor is `rift`; `htget` is also staged for installations with an
older HTTP worker. Copy these executables to `/bin` inside the guest.
Inside Fuse-X:

```sh
/mnt/host/js /mnt/host/js-system/system-info.mjs /mnt/host/js-system
/mnt/host/js /mnt/host/js-system/system-info.mjs /mnt/host/js-system /mnt/host/js-system/sample.json /tmp/js-report.json
```

The utility lists directory entries, reads UTF-8 text, parses JSON, obtains
file metadata, launches the configured program with literal arguments, captures
its output, and optionally writes a JSON report. Its source is
[examples/system-info.mjs](examples/system-info.mjs); the sample JSON/text live
in [examples/system-sample](examples/system-sample/). Copy these alongside the
utility on either target. Program selection in this JSON has ordinary trusted
local-script privileges.

## JavaScript API

```js
import {
    readDir, readTextFile, writeTextFile, stat,
    cwd, getenv, run, sleep, decodeUTF8, monotonicNow
} from 'fuse:system';

const [names, text] = await Promise.all([
    readDir('/etc'), readTextFile('/etc/example.json')
]);
const config = JSON.parse(text);
const child = run('/bin/echo', ['a literal argument', config.message]);
await sleep(10); // the child and I/O continue while this timer is pending
const result = await child;
console.log(result.code, result.stdout);
```

| API | Result / contract |
| --- | --- |
| `readDir(path)` | Promise of unsorted entry-name strings, excluding `.` and `..`. |
| `readTextFile(path)` | Promise of a UTF-8-decoded string, including embedded NUL. Regular files only. |
| `writeTextFile(path, text)` | Promise of `undefined`; create/truncate a regular file, encode UTF-8, preserve NUL. Not an atomic replacement or durability/fsync API. |
| `stat(path)` | Promise of `{ size, mode, isDirectory, isFile }`; follows guest symlinks. Size is logical file octets, as reported by the guest libc. |
| `cwd()` | Current directory string. |
| `getenv(name)` | Environment string or `undefined`. |
| `run(program, args = [], options = {})` | Promise of `{ code, signal, stdout, stderr }`. A normal exit has numeric `code` and null `signal`; a signal exit has null `code` and a numeric signal. Nonzero exit codes resolve normally. |
| `encodeUTF8(text)` | ArrayBuffer of logical UTF-8 octets, capped at 1 MiB. |
| `decodeUTF8(arrayBuffer)` | UTF-8 text with replacement for malformed octets; embedded NUL is preserved. |
| `monotonicNow()` | Monotonic milliseconds for local deadlines; not Unix time. |
| `sleep(milliseconds)` | Promise settled after a monotonic deadline. |
| `setTimeout(callback, milliseconds = 0, ...args)` | Global one-shot timer; returns a numeric ID. |
| `clearTimeout(id)` | Global cancellation; unknown IDs do nothing. |

`run` uses `execvp`: names without `/` search the inherited `PATH`; paths are
executed directly. Arguments are never expanded by a shell. Standard input is
`/dev/null`; stdout and stderr are captured separately. The child inherits the
working directory and environment and runs in a new process group. Use an
explicit `/bin/sh` plus `['-c', source]` only when shell evaluation is intended.
There is no streaming/interactive child handle yet. `options.timeoutMs` defaults
to 30,000 ms; timeout kills the owned process group, closes pipes, reaps the
immediate child and rejects. Descendants that deliberately leave the group are
not managed by this API. `options.signal` can be an object with an `aborted`
boolean (including `AbortController.signal` from `fuse:http`); cancellation uses
the same group cleanup and rejects with `name: "AbortError"`. It is checked at
reactor steps, with child polling bounded to 10 ms plus guest scheduling/work.
`options.maxOutputBytes` sets the stdout cap to an integer in 1..1,048,576
(default 65,536); stderr remains capped at 65,536. `options.binary: true` returns
stdout as an `ArrayBuffer` of logical octets, preserving arbitrary binary bytes.
The default returns decoded UTF-8 strings as before.

Argument/type/bound errors throw synchronously. OS failures reject the Promise
with an `Error` containing numeric `errno`, `syscall`, and, where applicable,
`path`. An exec failure rejects; a real program exiting 127 resolves with
`code: 127`. Missing environment variables are not errors. Text conversion
uses QuickJS's length-aware UTF-8 conversion; malformed input is replaced using
its normal decoder policy. Paths/arguments containing NUL are rejected rather
than truncated. Byte-oriented file operations and undecodable filename round-tripping remain
future work; captured child output already has the binary option above.

## HTTP(S) JSON milestone

```js
import { fetch, AbortController } from 'fuse:http';
const controller = new AbortController();
const timeout = setTimeout(() => controller.abort(), 10000);
try {
    const response = await fetch('https://httpbin.org/json', {
        headers: { Accept: 'application/json' },
        signal: controller.signal,
        timeoutMs: 15000
    });
    if (!response.ok) throw new Error('HTTP ' + response.status);
    console.log(JSON.stringify(await response.json()));
} finally { clearTimeout(timeout); }
```

[examples/fetch-json.mjs](examples/fetch-json.mjs) adds a recurring progress timer,
UTF-8 JSON caching and a separate viewer program. Its optional arguments are URL,
cache file and viewer (default `https://httpbin.org/json`, `/tmp/js-fetch-cache.json`,
`/bin/cat`). The viewer is captured, so this example expects a noninteractive
program. Inside Fuse-X, after copying the shared `js` to `/bin/js`:

```sh
rift /mnt/host/js-system/fetch-json.mjs
# F5 runs it; Enter returns to Rift afterward.
js --max-time-ms 180000 /mnt/host/js-system/fetch-json.mjs
```

`fuse:http` is an embedded optional source module, available through import; it
does not install browser globals. Its API is a bounded Fetch subset:

- `fetch(url, options)` performs GET and returns a Promise of `Response` after
  the complete body and a successful worker exit. HTTP error statuses resolve;
  network, framing, certificate, worker and limit errors reject.
- `Response`: read-only `status`, `statusText`, `ok`, `headers`, `url`, `redirected`;
  `bodyUsed`; asynchronous `text()`, `json()`, `arrayBuffer()`. Consume once.
  Body bytes retain octet identity until explicit UTF-8 decoding.
- `Headers` accepts object entries, pair arrays or another `Headers`; provides
  `get`, `has`, `entries` and iteration. Lookups ignore case. It is read-only.
- `AbortController` supplies `signal` and `abort()`. `AbortSignal` supplies
  `aborted` and `throwIfAborted()`. These do not implement EventTarget or abort
  reasons. Aborting kills/reaps the owned worker group and rejects `AbortError`.
- `timeoutMs` defaults to 30,000 and covers all redirect hops. Deadline expiry
  rejects `TimeoutError`; the CLI's whole-evaluation budget also applies.
- `maxBodyBytes` defaults to and cannot exceed 262,144. Response headers are
  bounded to 16 KiB. Request headers are limited to four ASCII fields of at
  most 1,024 octets each, also subject to `run`'s combined argument limit.
  Framing/Host/connection/encoding headers belong to the worker.
- `redirect` is `follow` (default, at most five hops), `manual` or `error`.
  Relative locations and query-only locations work. HTTPS-to-HTTP downgrade
  is rejected; credentials (`Authorization`, `Cookie`, `Proxy-Authorization`)
  are stripped on origin changes.
- URLs use the worker's ASCII DNS/IPv4 subset: HTTP(S), optional port, no
  userinfo or IPv6 literal. Percent-encode non-ASCII URL components. There is
  no URL constructor, POST/upload, streaming body, compression, cookie jar,
  connection pool or Node/npm compatibility in this milestone.

The module executes **guest `/bin/htget --response`** with literal arguments.
That existing library-backed worker performs guest DNS/TCP, HTTP framing,
chunk decoding and verified TLS with the installed `/etc/ssl/cert.pem`. It is
also NetSurf's transport boundary. No host HTTP proxy or substitute TLS engine
is used. JavaScript observes its nonblocking pipes through `select`, so timers
remain responsive even during DNS, CA parsing or cryptographic work. A worker
failure after partial output rejects the whole fetch. This buffering policy
intentionally avoids presenting an incomplete response as successful.

`JS_HTTP_HELPER` may select another trusted compatible executable (one pathname,
no flags); the host-reference protocol test uses this to select its fixture.
Normal guests use `/bin/htget`. Older htget builds lacking `--response` must be
updated; the browser builds already use this protocol. Each request currently
pays process/loader and TLS trust preparation costs. This gives asynchronous
client behavior without claiming an in-process nonblocking TLS facade.

## TCP and a small HTTP server

```js
import { createApp, json, text } from 'fuse:http-server';
import { sleep } from 'fuse:system';
const app = createApp();
app.use(async (request, next) => {
    console.log(request.method, request.url);
    return next();
});
app.get('/hello/:name', request => json({ hello: request.params.name }));
app.post('/echo', async request => json(await request.json(), { status: 201 }));
app.get('/slow', async () => { await sleep(1000); return text('done'); });
const server = app.listen({ port: 8080 });
console.log(server.address);
await server.closed;
```

Run with `js --server file.server.mjs`. Server mode removes the default
whole-evaluation wall-clock and Promise-job ceilings, while keeping the memory
limit and applying `--max-polls` to each JS turn (initial evaluation, pending job
or reactor callback). Promise jobs still yield to the reactor every 32 jobs.
Explicit `--max-time-ms` and `--max-jobs` **after** `--server` impose finite
whole-process budgets. Synchronous runaway code still hits its CPU budget.
Ctrl-C cancels work, closes listeners/connections and exits with status 130.
Ordinary script execution retains its previous limits.

Rift adds `--server` when F5 runs a `.server.mjs` or `.server.js` file. Other
JavaScript files retain normal script mode. Run
`rift /mnt/host/js-system/hello.server.mjs`, then F5; Ctrl-C stops the server
and Enter returns to the editor. `[run] javascript` still overrides the default
command. The example serves `/`, `/hello/:name`, `/api/status`, `/api/slow` and
JSON POST `/api/echo`; its optional first argument selects the port.

The server module exports:

- `serve(handler, options)` and `createApp().listen(options)`, returning a server
  with `address: { hostname, port }`, `closed: Promise`, and `close({force:false})`.
  Bind defaults to `0.0.0.0`, port zero chooses an available port.
- `close()` stops accepts and allows in-flight responses to complete. Call it
  without awaiting inside a handler; await `server.closed` separately to join
  shutdown. `close({force:true})` closes active streams as well, waking pending
  reads/writes. Accepted sockets that race with shutdown are closed.
- `text(stringOrArrayBuffer, {status,headers})` and
  `json(value, {status,headers})` build bounded replies. Framing headers are
  owned by the server; Content-Length counts UTF-8 octets. HEAD suppresses the
  body while retaining the corresponding length; 204/304 require empty bodies.
- Requests expose `method`, `url` (origin-form target), `path`, read-only
  `headers`, `query.get/getAll/has/entries`, `params`, `signal`, `bodyUsed`, and
  one-shot asynchronous `text/json/arrayBuffer` methods. JSON parse errors
  produce HTTP 400. The request signal is aborted on deadline or completion;
  application code must cooperate with cancellation of its own asynchronous work.
- The router provides chainable `get`, `post`, `route(METHOD, pattern, handler)`
  and `use(async (request, next) => reply)`. Patterns match complete paths with
  named `:parameters`. Route parameters and query components decode UTF-8;
  repeated query values are retained. Middleware returns `next()` or a reply;
  each middleware invocation may call `next` once. There are at most 64 routes
  and 32 middleware stages. Dispatch yields a Promise job between stages so
  RVM does not accumulate a native stack frame for every middleware hop.
- Unmatched routes return 404; a matched path with another method returns 405
  with Allow. HEAD can use a GET route. Handler exceptions produce 500 and are
  logged; an already-started response is closed instead of writing a second
  status line into its body.

The first server intentionally handles **one request per connection**, with
Connection: close. It accepts HTTP/1.0 and HTTP/1.1 origin-form requests,
requires one nonempty Host in HTTP/1.1, rejects folded/control-character headers,
duplicate Content-Length, ambiguous framing and unsupported Expect. Headers are
limited to 16 KiB, 64 fields and 1,024 octets per line. Content-Length request
bodies and buffered responses are limited to 64 KiB. Request chunking returns
501; there is no keep-alive, upgrade/WebSocket, streaming HTTP body or inbound
TLS profile yet. Existing `fuse:http` HTTPS clients continue using verified TLS.

`timeoutMs` defaults to 30,000 for the request read, handler and response write;
`maxBodyBytes` can lower the 64 KiB request cap. A stalled I/O operation closes
its stream. A timed-out asynchronous handler receives an aborted signal and,
when the connection remains writable, a 408 response. Synchronous JS is governed
by the host CPU guard. Arbitrary handler work is not preempted or rolled back.
`maxConnections` defaults to two and may be set to four, matching the guest
accept pool. Outstanding JS allocations still share `--memory`; reducing
concurrency or increasing that limit may be needed for large application data.

### Raw TCP

`fuse:net` is a reusable layer below HTTP:

```js
import { listen, connect } from 'fuse:net';
const listener = listen({ hostname: '0.0.0.0', port: 8080 });
try {
    const connection = await listener.accept({ timeoutMs: 30000 });
    try {
        const bytes = await connection.read(2048);
        if (bytes !== null) await connection.write(bytes);
    } finally { connection.close(); }
} finally { listener.close(); }
```

`connect({hostname,port,timeoutMs,signal})` asynchronously connects to a **numeric
IPv4** address. DNS remains in the separate HTTP/TLS worker for outbound fetch.
Listeners expose `address`, `accept(options)` and idempotent `close()`;
connections expose `address`, `peer`, `read(maxBytes=2048, options)`,
`write(stringOrArrayBufferOrView, options)` and `close()`.
Read returns an ArrayBuffer up to the requested size (at most 16 KiB), or null
at EOF. Write copies at most 64 KiB and settles only after all octets are accepted
by the socket, honoring partial writes and TCP backpressure; it does not promise
peer receipt. There may be one pending read and one pending write per connection,
or one accept per listener. Extra overlapping operations throw EBUSY instead of
building an unbounded queue. Options accept monotonic `timeoutMs` and a signal
object with `aborted`; accept cancellation leaves the listener usable, while
read/write/connect cancellation or failure closes the affected connection.

Handles belong to the context, are explicitly closed, and use nonreused IDs so
stale pending operations cannot act on reused descriptors. Collection of a JS
wrapper does not itself close a socket: use finally and close. Context teardown
and cancellation close all owned sockets. The ceiling is 32 handles and 32
pending operations shared with the system APIs; OS socket/descriptor limits may
be lower. An idle raw socket alone does not keep the event loop alive; queued
accept/read/write work does. No raw file descriptor escapes through this API.
This optional capability must not be inherited by browser scripts implicitly.

This is the first routing layer toward an Express-like runtime, not Node or
Express package compatibility. Next steps are bounded HTTP streaming and
backpressure APIs, keep-alive/request lifecycle work, and then richer routing
and middleware services.

## Event loop and limits

This host performs real asynchronous child execution and readiness-driven pipe
I/O. It sleeps in the existing kernel `select()` implementation, using
nonblocking descriptors, monotonic timer deadlines, and `waitpid(WNOHANG)`.
It reads both child streams while the child is running, so output exceeding
pipe capacity does not deadlock it. A close-on-exec error pipe distinguishes
failed exec from a successful program returning 127. The launcher explicitly
clears close-on-exec on redirected standard descriptors: current guest kernels
copy that flag through `dup2`, unlike POSIX.

Promise jobs run in batches of at most 32, alternating with one round-robin OS
work unit. File data moves in 2,048-octet chunks and directory scans consume at
most 16 entries per work unit. Timers run no earlier than their deadline;
actual latency includes kernel clock resolution, JS work and scheduling.
Due timers are ordered by deadline, then by registration order for equal
deadlines; round-robin slot reuse cannot make a later sleep overtake them.
Bounded microtask batches are a deliberate fairness policy of this host,
not a claim to reproduce browser/Node event-loop ordering exactly.

Regular file operations and directories are deferred and cooperative; they do
not run in worker threads or a kernel asynchronous disk-I/O engine. Individual
`open`, `stat`, `read`, `write` and `readdir` calls can still block on storage.
File/module source loading before evaluation is synchronous. The readiness
loop is reusable, but raw sockets use the same readiness loop, as described above.

Hard limits per context:

- 32 pending operations/timers, also constrained by the guest's descriptor limit.
- 1,048,576 logical octets per text file; 4,096 returned directory entries.
- By default 65,536 captured octets per child stream. Stdout can opt into a
  cap up to 1,048,576; overflow kills/reaps and rejects.
- 511 UTF-8 octets per path; 32 program arguments totaling at most 4,096 octets
  (excluding the executable name).
- Timer delays and child timeouts between 0 and 2,147,483,647 milliseconds.

Queued buffers and retained JS handles use the engine allocator and count
against `--memory`; libc directory objects, source-loading buffers, OS objects
and kernel process copies have additional bounded or OS-governed costs.
Default CLI budgets remain 1,048,576 engine cells, 10,000 interrupt polls and
1,024 Promise jobs per evaluation. A new `--max-time-ms` budget defaults to
60,000 ms and bounds the entire evaluation/event loop. Time budgets are checked
between OS work units and at engine interrupts; they cannot interrupt a blocked
kernel syscall. Exhaustion cancels pending host work and kills/reaps owned
children. A failed timer callback fails the evaluation; an unhandled timer
Promise rejection is reported. The host drains work before returning to the
REPL prompt, so an evaluation containing a timer waits for it.
Result conversion for `-p` and the REPL happens before draining work, and the
saved text is printed afterward. Timers or Promise rejections created by a
custom result `toString()` therefore participate in the same evaluation.
During evaluation, SIGINT/SIGTERM request orderly cancellation and exit with
128 plus the signal number. Signal handlers only set a flag; descriptor closing,
process-group termination and child reaping happen in ordinary host code.

## Reusing the C layer

[system.h](system.h) and [system.c](system.c) are separate from the engine and
CLI, and use only the QuickJS public API and the existing POSIX SDK. Compile
`system.c` with the generated engine headers for C16, or upstream headers for
a normal host. Generate `qjs-http.inc` with `embed.prepare_http(build_dir)` and
add that directory to the C include path. Forward optional module-loader
requests (`fuse:http`, `fuse:net`, `fuse:http-server`) to `qjs_system_module(ctx, name)` as demonstrated by `cli.c`. No upstream engine files or `quickjs-libc.c` are modified.

1. Create a QuickJS runtime/context and apply the embedding application's
   memory, stack and interrupt limits.
2. Call `qjs_system_new(ctx)` to install `fuse:system` and global timers. It
   owns the context's opaque pointer; an existing opaque owner is rejected.
   On initialization failure, report the pending exception and discard the
   context; module/global registration may be partially complete.
3. Evaluate code with the normal QuickJS API. Alternate bounded pending-job
   batches with `qjs_system_step(system, max_wait_ms)`. Zero wait polls; nonzero
   waits in `select`, capped by the next timer, child-exit polling and 1 second.
4. Supply rejection tracking, exception reporting and whole-evaluation budgets
   in the embedding application, as demonstrated by `cli.c`.
5. Cancel with `qjs_system_cancel` when needed. Cancellation discards host work;
   its outstanding Promises remain unsettled rather than running JS during
   teardown. Call `qjs_system_free` before
   destroying the context/runtime, and do not evaluate more code afterward.

OS operations hold their resolver/rejector, callback, arguments and result
values until completion or cancellation. The library closes descriptors and
reaps owned immediate children during disposal. This is the first reusable
system-capability layer, not yet a packaged RFL ownership wrapper or stable
installed embedding SDK. Browser contexts must omit this module unless the
embedding application intentionally grants its filesystem/process privileges.
Outbound HTTP uses the process capability; raw TCP and inbound HTTP use the
shared socket reactor. Environment mutation, broader process options, installed
embedding SDK integration and RFL bindings remain separate work.

## Validation

```sh
python3 ports/quickjs/build.py --target host
python3 ports/quickjs/tests/test_cli.py --target host --binary build/quickjs-cli/js-host
python3 ports/quickjs/tests/test_system.py --target host --binary build/quickjs-cli/js-host
python3 ports/quickjs/tests/test_system.py --target native --binary build/quickjs-cli/native/js.fxe
python3 ports/quickjs/tests/test_system.py --target ripple --binary build/quickjs-cli/ripple/js.rxe
```

`tests/test_server.py --target host|native|ripple --binary PATH` exercises real
TCP binary echo and partial writes, timer fairness, accept timeout/cancellation,
stale-handle protection, read cancellation, Unicode routing and JSON, 24 async
middleware hops, protocol rejection, simultaneous clients, deadlines, responses
beyond TCP buffer capacity, disconnect recovery, and graceful/forced shutdown.
`tests/test_server_lifecycle.py` verifies that SIGINT and whole-evaluation budget
cancellation release listeners so a fresh process can rebind the same port.
Guest tests use private Ethernet fixtures and disposable roots and require clean
filesystem shutdown. CLI cases cover server mode's CPU guard, optional finite
budgets and operation beyond the normal Promise-job cap. Rift's live test also
checks F5 server-mode selection, Ctrl-C and restoration of editor tabs.

For HTTP, run `tests/test_http.py --target native|ripple --binary PATH --htget PATH`
with current target binaries. It creates its own OpenSSL CA/server and exercises
real guest DNS/TCP/TLS, Unicode JSON beyond 64 KiB, timers during transfer, caching,
viewer launch, redirect policy, expired/wrong-host certificates, truncation,
limits, timeout and cancellation. `tests/test_http_protocol.py --binary HOST_JS`
checks binary/NUL/UTF-8 decoding, relative/query redirects, loop bounds and
credential stripping against a host-only worker protocol fixture. The real TLS
tests do not use that fixture. The public JSON/cache/viewer example also passed
on both Fuse-X and RVM against `https://httpbin.org/json` using guest TLS. `tests/test_http_internet.py` is an opt-in public
JSON check using the installed Mozilla root bundle and a blind TCP relay (no
TLS decryption or certificate replacement in the relay). `rfl/tools/test_rift_js.py` separately drives F5,
F8, highlighting, error reporting and terminal restoration on both guests.

The system runner builds a small child fixture, creates disposable guest roots,
and tests real timers, concurrent children, signal/nonzero exits, literal Unicode
arguments, missing/non-executable programs, pipe output beyond kernel capacity,
output limits, child timeout, cancellation, Promise fairness, UTF-8 filenames,
Unicode/NUL/bank-sized text, JSON, metadata and repeated descriptor cleanup.
Separate CLI cases check timer liveness, awaited timers, callback exceptions,
unhandled timer rejections, loop budgets and killing a running child on budget
exhaustion. Guest shutdown must pass filesystem checks. Reports record the
executable checksum. No test modifies the user's installed disk images.
Timeout and Ctrl-C regressions also record the child's PID and use a fresh
program to verify that the child no longer exists after the JS host exits.

Low-memory CLI regression testing additionally exposed deferred parser-buffer
allocation failure being misreported as invalid bytecode. The generated adapter
now checks the input DynBuf before variable resolution can replace it, so
truncated output fails as out-of-memory. Upstream source files remain pristine.

The legacy image installer currently encodes non-ASCII manifest filenames as
UTF-16 cells. Fixtures therefore create UTF-8 names through `writeTextFile`
before listing and reopening them; sample distribution filenames are ASCII.
