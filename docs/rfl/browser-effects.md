# Browser timers and fetch

Status: W4a transport and W4b mounted executor implemented. Updated 2026-10-18.

The [browser bridge](browser-bridge.md) now transports timer/fetch starts,
cancellations and completions in batches. Actual browser APIs run in JavaScript;
operation identities, results, allocation, futures and wake registrations live
in Wasm. This extends the browser profile without changing the Ripple ISA or
adding services to ordinary compute modules.

## Try the transport

```sh
python3 rcc-wasm/examples/browser/run.py --effects
# Open http://127.0.0.1:8000/rcc-wasm/examples/browser/
```

The [C example](../../rcc-wasm/examples/browser-effects.c) starts a one-second
timer and fetches a local Unicode text file in one outgoing batch. It prints
completions, updates the page and offers cancellation. Disposal cancels remaining
host operations and frees guest response storage. No external network is needed.
The counter demo's `?test` mode applies to the counter/RFL click examples, not
this asynchronous demo.

C applications call `ripple_browser_effects()` once after
`ripple_browser_mount()`, then implement `void ripple_browser_on_effects(void)`
in addition to the ordinary event/disposal hooks. The compiler keeps the hook
reachable and checks its signature. Missing hooks fail compilation.

```c
/* Starts are staged, not an import per operation. Zero means slot/ID exhaustion. */
unsigned long timer = ripple_effect_timer(1000);
unsigned long request = ripple_effect_get(url_cells, url_octets, 4096);
/* In on_effects: inspect state; copy successful response bytes; then release. */
unsigned state = ripple_effect_state(request);
/* In cancellation/disposal: idempotent, including already completed requests. */
ripple_effect_release(request);
ripple_effect_release(timer);
```

See [browser.h](../../rcc-wasm/browser.h) for the actual declarations. State is
0 pending, 1 successful, 2 fetch/network failure, 3 body limit exceeded, or
4 unavailable browser service, 5 retired/unknown, or 6 storage quota exceeded. Successful fetches retain their HTTP status, including 4xx/5xx;
those statuses are not network failures. Response bytes remain guest-owned until
release; freeing an already delivered result needs no host call or acknowledgement.
`ripple_effect_copy` requires sufficient destination capacity. An empty
body requires no destination storage.

## RFL futures

[`std/browser_effects`](../../rfl/std/browser_effects.rfl) provides:

* `browser_effects()` creates the mount's shared wake registry and opts into the ABI.
* `sleep(effects, milliseconds)` returns an owned, lazy timer future.
* `get(effects, url, body_limit)` returns an owned, lazy GET future producing
  `BrowserResponse { status, body: Vec<u8> }`.
* `effects.wake_completed()` wakes registered tasks whose operations settled.
  A completion hook calls it before giving its task scope one bounded turn.

These implement the existing `Future`/`Poll`/`Context` contract. They register
copied generation-checked wakers, never borrow a movable future/context across
callbacks, and never self-wake in a busy polling loop. Starting happens on first
poll. Dropping or cancelling unfinished work unregisters its wake and stages host
cancellation. A never-polled future causes no host operation. Results are copied
into owned RFL vectors; no response inbox pointer escapes.

Errors distinguish invalid arguments, capacity, network failure, body overflow
and cancellation. Body bytes are binary; no implicit text decoding or JSON parse
occurs. Request validation limits are checked before a request is constructed.

## Mounted applications and ordinary async/await

[`std/browser_app`](../../rfl/std/browser_app.rfl) supplies the lifecycle hooks,
an owned application root and a persistent task scope. Returning from `main`
keeps the application alive until the bridge disposes it. The application owns
its scope; JavaScript holds no RFL model, future, task queue or reference counts.

```rfl
import { mount_app, BrowserApp } from "std/browser_app";
import { BrowserEvent } from "std/browser";
import { Future, sleep } from "std/future";

async fn hello() {
    sleep(75).unwrap().await;
    println("Hello from an RFL future!");
}

fn main() {
    let app = mount_app(8, fn(events: Vec<BrowserEvent>, app: BrowserApp) => {}).unwrap();
    app.spawn(hello()).unwrap();
}
```

Import `Future` for `.await`. Use `std/future.sleep` for ordinary clock-based
sleeps; multiple deadlines share one host scheduling timer. The lower-level
`std/browser_effects.sleep(app.effects(), ms)` also remains available for an
explicit browser timer operation. Fetch uses
`get(app.effects(), url, body_limit).unwrap().await` and returns the same owned
binary response described above.

* `mount_app(capacity, on_events)` accepts 1..64 root tasks. The callback gets
  owned event snapshots and a copyable `BrowserApp` capability.
* `app.spawn(future)` accepts any `Future`, lazily queues it and returns a
  `BrowserTask`. Root outputs are discarded; update state or send messages
  inside the future. Capacity counts both queued and active roots.
* `task.cancel()` requests cancellation before the next scheduling turn;
  `task.is_finished()` observes collected completion/cancellation. Dropping a
  handle leaves the task owned by the application. Disposal cancels every root.
* Tasks may enqueue other tasks without reentering the scope borrow. Rejected
  futures and cancelled tasks run cleanup outside the ingress queue borrow.
* Application capabilities retain the ingress/effects state, rather than the
  scope, so a task can capture its application without retaining its own scope.

Every start/event/completion entry samples the monotonic clock and polls at most
one bounded scope turn (at most the configured task capacity). A self-wake or
newly enqueued task requests another browser task, allowing input and painting;
there is no endless Promise/microtask chain. No runnable work or deadline means
no scheduled wake and no idle polling. This is cooperative: a single poll must
return, so arbitrary synchronous loops cannot be preempted.

The root is a compiler-owned typed RFL callback. It retains normal managed
storage across entries, publishes replacement roots before releasing old ones,
and clears the root after closing the scope on disposal. JavaScript and C never
inspect RFL callable/frame layouts. Normal disposal returns guest allocations
to the single warm synchronous frame arena (currently 520 cells); the bridge
then drops the instance and its entire memory image. Traps discard the instance
without promising guest stack unwinding. Do not use synchronous `block_on` for
browser work: it prevents browser timers and Promises from running.

Try the [ordinary RFL async demo](../../rfl/examples/browser-async.rfl):

```sh
python3 rcc-wasm/examples/browser/run.py --async
# Open http://127.0.0.1:8000/rcc-wasm/examples/browser/
# Append ?test=async for the real-browser lifecycle regression.
```

It fetches the local Unicode message, awaits a clock timer, and supports reload
and cancellation through queued DOM events, with no application-written FFI
hooks. The [typed UI library](browser-ui.md) adds reconciliation, JSX and Model/Msg/commands
on top of these futures.

## Host lifecycle and limits

There are 64 guest operation slots. IDs are nonzero u32 values that increase
throughout a mount; slots may be reused, IDs never are. At u32 exhaustion,
allocation fails rather than introducing ABA. Guest release immediately retires
an identity and frees its body. The host revokes a registration before clearing
a timer or aborting fetch; queued and late completions for it are discarded.

Starts/releases are flushed once at the end of public start, DOM-event or
completion/executor entries, if commands or a changed scheduling hint need delivery. Disposal runs guest
cleanup, frees all remaining guest response storage, and aborts host work without
an unnecessary final command roundtrip. Outgoing capacity is 256 commands or
32768 cells per guest turn; exhaustion faults explicitly. Large sets of URLs
must be split across turns rather than growing buffers without bound.

Callbacks only enqueue. One scheduled `setTimeout(..., 0)` task coalesces available
completions into at most 64 records / 32768 cells and makes one typed Wasm entry.
Additional records get another task. This yields to the browser and also works
when animation frames are suspended. DOM events keep their existing RAF batching.
The host callback queue is bounded by the 64 outstanding registrations; bodies
are bounded independently. There is no unbounded completion or Promise loop.

Timer intervals are 0..2147483647 ms. Browser scheduling/throttling determines
actual delivery time; a timer does not guarantee an exact wall-clock wake.
Fetch is currently GET only, with URL length 1..2048 UTF-8 octets (no NUL) and
body limit 1..16384 octets. It uses browser fetch with same-origin credentials,
normal CORS/redirect rules and AbortController. Response bodies are read as a
stream and rejected as soon as they exceed the caller's limit; no unlimited
`arrayBuffer()` or trust in Content-Length. Opaque/unreadable responses and fetch
rejections become network failures. Custom methods/headers, request bodies,
streaming response APIs, WebSocket and guest raw TCP are outside this first ABI.

Complete outgoing batches are validated before starting/cancelling operations.
Complete incoming batches are validated before changing guest results, including
per-request body bounds. Malformed kinds/lengths/octet cells/reserved fields,
wrong operation kinds and invalid HTTP status codes fail explicitly. On a trap
or host failure, the bridge retires the instance and releases host resources;
it does not promise to unwind trapped RFL frames.

Counters include effect batches/commands, starts/cancellations, outstanding
operations, queued/late completions, completion batches/records/copied cells,
task wakeups, explicit executor turns, whether a host task is scheduled,
and the existing host crossings, guest entries and allocator counters.

[Routing and browser storage](browser-routing.md) extend the same channel with
owned location subscriptions, hash navigation and bounded string persistence.
They add no host import; that document specifies kinds 4..8 and their result rules.

## Wire ABI 1

The optional import is `ripple.effect_commit(byte_address:i32, cells:i32) -> void`.
Public exports are `__ripple_effect_buffer() -> i32` (32768-cell inbox byte
address) and `__ripple_completions(cells:i32) -> void`. Private i64 carriers never
cross this boundary. Every internal count/offset is a little-endian C16 cell;
each body or URL octet occupies one entire cell. No pointers are retained by JS.

Outgoing header:
`[0x5246, 1, total_cells, command_count, has_hint, hint_low, hint_high, 0]`.
`has_hint` is 0 or 1; absent hints require zero halves. A hint is 0 for runnable
work, 1..2147483647 ms for a future deadline, or 4294967295 for no wake. Only
executor-profile modules may send hints. A hint shares the effect batch; it does
not require a separate import. No command and no new hint means no import.

A reachable `ripple_browser_executor()` opts into the executor profile after
mount/effects. It requires `unsigned long ripple_browser_on_turn(void)`, which
returns that hint. The profile adds `__ripple_turn(now:i32) -> void` and changes
only these active wrappers:

```text
__ripple_start(now:i32) -> i32
__ripple_events(cells:i32, now:i32) -> void
__ripple_completions(cells:i32, now:i32) -> void
```

The host detects the optional `__ripple_turn` export. Clock sampling is piggybacked
on the existing entry, not an additional host call; `ripple_browser_now()` reads
the guest sample. Time wraps at u32 milliseconds. Forward gaps must remain below
2^31 ms (about 24.9 days); backwards/ambiguous samples fault explicitly. W3/W4a
modules keep their original argument signatures and zero scheduling-header words.
Each variable-length command:

```text
[8 + payload_octets, kind, id_low, id_high, arg_low, arg_high,
 payload_octets, 0, ...payload_octet_cells]
```

Kinds: 1 timer (`arg=milliseconds`, no payload), 2 GET (`arg=body_limit`, URL
payload), 3 cancel (`arg=0`, no payload), and platform kinds 4..8 described
in [routing/storage](browser-routing.md#additive-wire-extensions). Both halves of IDs/arguments are u16.

Incoming header: `[0x5247, 1, total_cells, record_count]`. Each record:

```text
[8 + body_octets, id_low, id_high, kind, http_status, body_octets,
 error, 0, ...body_octet_cells]
```

For timer/fetch, kind is 1 or 2. Errors are 0 success, 1 network failure, 2 body limit.
Platform operations additionally use 3 unavailable and 4 quota; see the extension above.
Timer status/body/error must be zero. Failed fetches carry zero status/body;
successful fetch status is 100..599. Retired and duplicate settled identities
cannot overwrite another operation's result.

## Validation

```sh
cargo test --release --jobs 2 -p rcc-wasm
cargo test --release --jobs 2 -p rfl --test wasm_browser
```

Tests execute compiled Wasm, not a simulated guest: overlapping timer/fetch,
HTTP error status versus network failure, streamed body overflow, cancellation
through a queued DOM event, late completions, bounded response batches,
slot exhaustion/reuse, non-reused IDs, corrupt completion records, allocator
cleanup, RFL lazy futures and owned response vectors. Pure JS preflight tests
also assert that an invalid later command cannot partially start a batch.

Mounted-executor cases additionally cover root retention after `main`, releasing
the root while its dispatcher is active, self-wakes, nested task spawning,
clock deadlines, queued cancellation, capacity errors and reentrant cleanup,
repeated fetch/event batches with stable allocation counts, and disposal during
an outstanding request. Host tests check clock wrap, stale callbacks and atomic
wake-hint validation; backend tests reject missing/mistyped turn hooks and invalid
clock samples. The demo is exercised as compiled Wasm as well as in a browser.
