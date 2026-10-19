# RFL routing, browser storage and TodoMVC

Implemented 2026-10-18. The [typed UI](browser-ui.md) now has a working single-page
[TodoMVC application](../../rfl/apps/todomvc/main.rfl), reusable hash routing,
owned location subscriptions, bounded local storage, double-click/blur/focus
handlers, and deferred focus. Models, route matching, persistence encoding and
diffing remain in Wasm. The host applies patches and performs browser operations.

## Run it

```sh
python3 rcc-wasm/examples/browser/run.py --todo
# Open http://127.0.0.1:8000/rcc-wasm/examples/todomvc/
```

`--build-only` produces `rcc-wasm/examples/todomvc/build/app.wasm`; `--port 8019`
changes the loopback server port. The counter and TodoMVC have separate outputs.
Refreshing a URL ending in `#/active` or `#/completed` loads the same application
and restores its saved todos. The normal browser profile needs no custom
application JavaScript or C hooks.

Append `?test=todo` before the hash for the optional real-DOM regression. It uses
an isolated localStorage key, drives real controls/history, mounts a fresh Wasm
instance to check persistence, and disposes its instances. Its temporary key is
removed; the normal user's todo list is not changed. A successful run prints
`REAL TODOMVC BROWSER TEST PASSED` under Runtime diagnostics. That test finishes
with an empty, disposed app; open the ordinary URL to interact manually.

## Typed hash routes

[`std/router`](../../rfl/std/router.rfl) provides:

| API | Behavior |
| --- | --- |
| `hash_path(hash)` | Validates a hash and returns its path; empty hash means `/`. |
| `parse_route(hash, parse)` | Calls `fn(String)->Option<Route>` inside Wasm; returns `InvalidHash` or `UnknownRoute` explicitly. |
| `route_href(route, path)` | Calls `fn(Route)->String` and returns a validated `#/...` anchor href. |
| `watch_routes(effects, parse, notify)` | Owned, lazy future delivering initial and changed `Result<Route, RouteError>` snapshots. |
| `navigate(effects, hash, replace)` | Owned async push/replace navigation with an explicit result. |

The first grammar is deliberately small: `#/` followed by ASCII letters, digits,
`/`, `_`, `~`, `.`, or `-`, at most 2048 UTF-8 octets. Empty browser hash is also
accepted as root. There is no query parsing, percent decoding, secondary fragment,
external URL or path-history router. Escapes and query syntax are rejected rather
than interpreted inconsistently. Route parsers decide which valid paths exist.

```rfl
type Filter = All | Active | Completed;
fn parse(path: String) -> Option<Filter> {
    if (path == "/") { Some(All) }
    else if (path == "/active") { Some(Active) }
    else if (path == "/completed") { Some(Completed) }
    else { None }
}
fn path(filter: Filter) -> String {
    match filter { All => "/", Active => "/active", Completed => "/completed" }
}
// Inside a view, with route_href imported:
html!{<a href={route_href(Active, path).unwrap()}>Active</a>}
```

These are ordinary anchors. The bridge does not intercept their clicks, so
browser back/forward, bookmarks, modified clicks and opening another tab retain
native behavior. Only validated hash hrefs on HTML anchors are accepted; arbitrary
URL attributes and scripts remain outside the bridge schema.

A location subscription installs one host `hashchange` listener on first poll.
It publishes an initial snapshot, then changed hashes; repeated notifications of
the same current hash are ignored. Changes waiting for delivery coalesce to the
latest snapshot, rather than building an unbounded navigation queue. Programmatic
push/replace explicitly publishes through the same subscription and deduplicates
any later browser notification. Idle subscriptions schedule no polling timer.

A subscription is a normal owned future: poll it in an application task, or wrap
its await in an async command. Its callback receives owned data after host ingress
and may enqueue a UI message. Callbacks never synchronously enter Wasm. Cancel its
task or drop its owner to remove the host listener; disposal cancels all of them.
The low-level `std/browser_effects.watch_location` supplies raw hashes, and the
subscription future itself returns a typed error if the host service fails.
Unknown/malformed route results go to the typed callback without closing it.
TodoMVC also repairs an oversized initial/changed hash to `#/`, then owns a fresh
subscription so later routes continue working.

## Startup and persistence commands

`std/ui_command.program_with_init(model, init, update, view, capacity)` constructs
initial commands once with mounted `Ui<Msg>` capabilities. They start in the first
bounded turn. Use this for location subscriptions and initial storage loads;
no synthetic DOM event or application lifecycle FFI is required. Ordinary
`program` remains available when an application needs no initial commands.

[`std/browser_storage`](../../rfl/std/browser_storage.rfl) provides async
`load(effects, key)`, `store(effects, key, value)` and `remove(effects, key)`.
Import `Future` for `.await`. `load` returns `Result<Option<String>, BrowserEffectError>`:
`None` is a missing key, while `Some("")` is an existing empty value. Keys are
1..256 UTF-8 octets without NUL; values are at most 16384 UTF-8 octets. Invalid
arguments, unavailable storage, quota exhaustion and oversized reads are explicit
errors. Data is an opaque UTF-8 string to the host; there is no host JSON/model
serialization. UTF-8 conversion and browser calls are bounded.

Storage operations use the same owned command/completion channel as timers and
fetch. Commands execute in batch order. The actual localStorage calls are
synchronous browser operations, with their results queued for a later guest turn;
there is no reentry during a save. Cancellation cannot undo a write already
performed by the browser, but it retires its completion and prevents later guest
callbacks. Applications should serialize dependent saves, not rely on cancellation
to roll back durable writes.

TodoMVC waits for the initial read before enabling editing. Missing data starts
an empty list without writing it back. Malformed/version-mismatched/unavailable
storage leaves the stored value untouched, shows an error, and offers Retry or an
explicit Use an empty list action. Persistence begins only after a committed todo
change or that explicit reset. Input drafts, edit drafts, route changes and
rendering do not write storage.

The model tracks a revision and allows one save at a time. Changes made while a
save is active remain in memory; its acknowledgement starts a save of the latest
snapshot. Older snapshots cannot finish after newer ones. A failed save leaves
changes visible, displays an error and offers Retry saving; it does not spin in
an automatic retry loop. Closing the app may leave an unacknowledged latest edit
unsaved, so the UI reports Saving versus Saved locally explicitly.

The [codec](../../rfl/apps/todomvc/codec.rfl) uses `RFLTODO1`, a record count and
length-prefixed UTF-8 titles. It checks version, sizes, numeric overflow, identities,
duplicate IDs, trimmed nonempty titles and trailing data in RFL. Limits are 128
todos, 256 UTF-8 octets per title and a 16 KiB complete snapshot. A full-size set of
titles may exceed the total snapshot limit; this produces a visible save error,
leaving the old saved data and current in-memory edits intact.

## Editing and focus

`html!` and the ordinary builders support `on_double_click`, `on_blur`, `on_focus`
and `autofocus={bool}`. Blur/focus payloads include the current control value.
Focus is requested when `autofocus` changes from false/absent to true, then applied
after insertion. Pending focus survives chunk boundaries in a large render.
Supported targets are HTML input, textarea, select, button and anchor elements.
Inputs supporting `select()` select their text on this initial focus request.
Unchanged renders do not repeat focus or reset selection.

TodoMVC supports trimmed Enter-to-add, individual/toggle-all completion, deletion,
clear completed, remaining counts and route-selected filters. Stable IDs key each
row. Double-click enters editing; Enter or blur commits, Escape cancels, and an
empty committed title deletes the row. Commits verify the current editing identity,
so Enter followed by blur cannot apply a second change. Keyboard actions ignore
IME composition. Browser input maxlength limits large pastes; the guest also
checks the stricter UTF-8 byte limit. Main/footer controls disappear when empty.

## Additive wire extensions

These operations use the existing `ripple.effect_commit` import and effect inbox;
there is no new Wasm/JS import. Keep the guest runtime and static host adapter
versions together. Older command kinds and retired state 5 keep their meanings.

| Effect kind | Argument | Payload | Successful result |
| --- | --- | --- | --- |
| 4 location subscription | 0 | empty | status 0, current hash bytes; registration remains live |
| 5 hash navigation | 0 push / 1 replace | validated hash | status 0, empty body |
| 6 storage read | body limit 1..16384 | key | status 0 missing / 1 present, bounded value bytes |
| 7 storage write | key byte count | key bytes followed by value bytes | status 0, empty body |
| 8 storage remove | 0 | key | status 0, empty body |

Kind 3 cancels a subscription too. A location completion keeps its ID registered;
`ripple_effect_consume(id)` frees the copied guest snapshot and rearms it locally,
without a host acknowledgement. Drop/release cancels even a settled subscription.
Guest state is 0 pending, 1 success, 2 network failure, 3 body limit, 4 unavailable
service, 5 retired/unknown, 6 quota exceeded. Wire errors are 0 success, 1 network,
2 body limit, 3 unavailable service and 4 quota. Existing 64-operation, 256-command
and 32768-cell bounds still apply. Each active location subscription uses one slot.
The host preflights a whole batch before any browser side effect.

DOM opcode 11 requests focus: `[11, node_id, 0, 0, 0, 0, 0, 0]`. Event kinds 8, 9,
10 are double-click, blur and focus. Event framing, queued dispatch and listener
retirement are unchanged. Diagnostics add location subscription counts, storage
reads/writes and programmatic navigation counts without reading guest model data.

## Verification

The implementation passed 24 RFL browser integration tests and all 26 Wasm
backend tests. The compiled TodoMVC suite covers five end-to-end scenarios using
the deterministic DOM/platform harness; five additional host-service tests cover
wire bounds, UTF-8, storage failures and subscription cancellation. Actual desktop
browser automation was unavailable in this session; the optional `?test=todo`
real-browser check is provided separately and is not included in those results.

```sh
cargo test --release --jobs 2 -p rfl --test wasm_browser --test operator_hints --test places -- --test-threads=1
cargo test --release --jobs 2 -p rcc-wasm -- --test-threads=1
node --test rcc-wasm/browser/platform.test.mjs
python3 rcc-wasm/examples/browser/run.py --todo --build-only
RIPPLE_TODO_WASM=rcc-wasm/examples/todomvc/build/app.wasm node --test rcc-wasm/browser/todomvc.test.mjs
```

Tests execute the actual compiled TodoMVC Wasm and cover edits, IME, duplicate
commit prevention, stable retained rows, initial routes/history, reload persistence,
ordered saves, no-op rendering, quota/unavailable storage, malformed data, explicit
reset/retry, and cancellation/disposal. Codec and typed-route tests also run on
Native and RVM. A separate large-tree Wasm case checks focus across patch chunks.
The optional real-browser test covers native anchors, focus and fresh instances.
Component-local resource APIs, request/search cancellation and performance baselines
remain separate [roadmap](browser-apps-roadmap.md) work.
