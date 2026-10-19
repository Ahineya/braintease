# Wasm browser bridge, version 1

Implemented as W3 of the [Wasm roadmap](wasm-browser.md). This is the low-level
DOM/event transport, usable without macros. The [typed `Html<Msg>` renderer and public HTML macro](browser-ui.md) build on
this transport.
[Browser effects](browser-effects.md) extend it with timer/fetch batches, owned
futures and a mounted RFL executor supporting ordinary async/await.
There is no JavaScript model, virtual DOM, reference-counting heap or guest
network stack in this bridge.

## Try it

From the repository root:

```sh
python3 rcc-wasm/examples/browser/run.py
# Open http://127.0.0.1:8000/rcc-wasm/examples/browser/
# Or use the stateful C counter instead:
python3 rcc-wasm/examples/browser/run.py --c
```

The launcher builds the selected release compiler, compiles the example, and
serves local files. `--build-only` stops after compilation; `--port 8001` chooses
another local port. The generated `build/app.wasm` is ignored by Git. It serves
the repository on loopback only; stop it with Ctrl-C.

The [RFL example](../../rfl/examples/browser.rfl) uses ordinary
[`std/browser`](../../rfl/std/browser.rfl) patch methods and exported C hooks.
The [C counter](../../rcc-wasm/examples/browser-counter.c) uses
[`browser.h`](../../rcc-wasm/browser.h); its persistent count lives in Wasm.
An initial render is one commit. Several clicks before the next frame become
one incoming batch and one render. The Dispose button releases the application.
Add `?test` to the demo URL for its automatic real-browser lifecycle check.

To compile by hand:

```sh
target/release/rfl compile rfl/examples/browser.rfl --target wasm -o /tmp/app.wasm
target/release/rcc compile rcc-wasm/examples/browser-counter.c --target wasm -I rcc-wasm -o /tmp/counter.wasm
```

Host integration uses a static ES module:

```js
import { mountBrowser } from "./rcc-wasm/browser/bridge.mjs";
const app = await mountBrowser("./app.wasm", {
  root: document.querySelector("#app"), // empty, exclusively owned mount
  onOutput(text) { output.textContent += text; },
  onError(error) { console.error(error); },
});
console.log(app.stats);
// On unmount:
app.dispose();
```

Sources may be a URL, Response, Wasm bytes, or precompiled WebAssembly.Module.
`onOutput` receives decoded Unicode chunks, including UTF-8 sequences split
across output buffers. Callbacks must not synchronously invoke the guest.
`onError` receives failures of scheduled turns or event capture; startup and
explicit `drain`/`dispose` failures throw. A failure discards host registrations
and DOM ownership; a trapped guest is never resumed. Normal disposal runs the
guest finalizer once before host cleanup. Repeated disposal is harmless. Final counters are cached and the adapter releases
its instance reference, so retaining diagnostics does not pin the C16 memory.

## Opt-in and public entry points

A reachable `ripple_browser_mount()` call opts a compilation into the browser
profile. Call it once from zero-argument `main`. Provide both hooks, with these
C16 signatures; in RFL use `export extern "C" fn`:

```c
void ripple_browser_on_events(const unsigned short *data, unsigned long cells);
void ripple_browser_on_dispose(void);
```

The backend retains both hooks as link roots even when main never calls them.
Ordinary Wasm applications gain no browser exports/imports. Missing or incorrectly
typed hooks are compilation errors. Browser main returns void, i16 or i32;
nonzero startup status is an error. A module mounts once; instantiate a new module
instance to mount again.

These generated **public wrappers use i32**, not the private i64 carrier ABI:

| Export | Signature / meaning |
| --- | --- |
| `__ripple_browser_abi` | `() -> i32`, returns 1 |
| `__ripple_start` | `() -> i32`, calls main |
| `__ripple_event_buffer` | `() -> i32`, stable inbox **byte** address |
| `__ripple_event_capacity` | `() -> i32`, 4096 C16 cells |
| `__ripple_events` | `(cells: i32) -> void`, validates and dispatches the inbox |
| `__ripple_dispose` | `() -> void`, runs guest disposal once |
| `__ripple_heap_metrics` | `() -> i32`, byte address of three u32 allocator counters |

The adapter caches inbox/counter addresses at setup. It refreshes memory views
per access; it never retains borrowed guest string/patch pointers. Node's existing
console runner does not supply DOM imports; serve browser modules with this bridge.
The optional mounted-executor profile adds a clock argument to active wrappers
and a `__ripple_turn(now)` entry; see [its exact ABI](browser-effects.md#wire-abi-1).

## Outgoing patch wire format

`ripple.dom_commit(byte_address: i32, cells: i32) -> void` borrows a complete
batch for the duration of the import. **Addresses are bytes; every size and
offset inside the batch counts 16-bit little-endian cells.** A UTF-8 octet
occupies one entire cell. C16 RAM pointers are translated/checked by the compiler
wrapper before entering JavaScript.

An eight-cell header:

| Cell | Value |
| --- | --- |
| 0 | `0x5244` |
| 1 | Version 1 |
| 2 | Total cells, including header |
| 3 | Command count, at most 1024 |
| 4 | Command offset, always 8 |
| 5 | String-pool offset, `8 + commands * 8` |
| 6 | String-pool cells |
| 7 | Zero, reserved |

Each command has eight cells: `[opcode, a, b, c, d, 0, 0, 0]`. Unused operands
are zero. The pool consists of `[octet_count, octet0, ...]` records. A string ID
is the record's cell offset relative to the pool, not an arbitrary interior
offset. Strings are decoded once per batch with strict UTF-8 validation.
Guest code can pool a static tag/property name once and reuse its ID throughout
the batch. IDs expire when the batch closes.

| Opcode | Operands a, b, c, d |
| --- | --- |
| 1 element | node ID, tag string ID, namespace (HTML=0 / SVG=1), 0 |
| 2 text | node ID, text string ID, 0, 0 |
| 3 set text | existing text node ID, text string ID, 0, 0 |
| 4 attribute | element ID, name string ID, value string ID, 0 |
| 5 property | element ID, property (value=1 / checked=2 / disabled=3), string ID or boolean 0/1, 0 |
| 6 insert/move | parent ID, child ID, before ID (0 appends), 0 |
| 7 remove subtree | node ID, 0, 0, 0 |
| 8 listen/replace | element ID, event kind, listener ID, policy flags |
| 9 unlisten | element ID, event kind, 0, 0 |
| 10 remove attribute | element ID, name string ID, 0, 0 |
| 11 deferred focus | element ID, 0, 0, 0 |

Node 0 is the mount root and can only be an insertion parent/anchor context.
Node and listener IDs are 1–65535, monotonically increasing for an entire mount;
retired IDs cannot be reused. Exhaustion fails explicitly. Subtree removal
retires all descendant nodes/listeners. Moving an existing node preserves its DOM
identity. Controlled values are assigned only when different, preserving an
unchanged input's selection. Text updates require text nodes and cannot orphan
registered descendants by replacing an element's whole contents.

Version 1 intentionally supports a finite set of ordinary HTML/SVG elements,
attributes and properties (listed in `browser/protocol.mjs`). Script/custom
elements, event-handler attributes, raw HTML and arbitrary URL attributes are absent.
The supported URL exception is a validated literal ASCII hash href on an HTML
anchor. Deferred focus requests wait until the node is inserted, including across
patch chunks; see [routing/editing](browser-routing.md).
Unknown commands/tags/properties fail explicitly. This is a small initial UI
surface, not a complete DOM API.

The Wasm builder has 1024 command slots and a 16384-cell pool; the host rejects
batches above 32768 cells. Exhaustion traps with fault 8. Larger renders must
explicitly flush bounded chunks, preserving IDs across batches. An empty guest
batch commits nothing. Dropping an RFL Patch aborts uncommitted work; nested
batches fail. A valid batch is entirely preflighted before DOM mutation,
including references, cycles, string ranges and reserved bits. Validation copies
only touched metadata, not the entire mounted tree. Host DOM exceptions after
preflight terminate/dispose the instance; there is no partial-state recovery.

## Incoming events

Event kinds are click=1, input=2, change=3, keydown=4, keyup=5,
compositionstart=6, compositionend=7, dblclick=8, blur=9 and focus=10. Each node has at most one binding per
kind. Replacing a listener retires its old identity. Synchronous policies are
preventDefault=1 and stopPropagation=2; callbacks apply those policies before
queueing. Payloads snapshot event data when received.

The inbox header is `[0x5245, 1, total_cells, event_count]`. Each record is:

```text
[record_cells, listener_id, node_id, event_kind, modifiers,
 key_octets, value_octets, checked, ...key_UTF8_cells, ...value_UTF8_cells]
```

Modifiers are Shift=1, Ctrl=2, Alt=4, Meta=8, composing=16. Key is the browser's
`event.key`. Input/change/composition/blur/focus include the bound element's current value;
checked is 0 or 1. Other events carry an empty value. Strings are owned octet
text after RFL `decode_events`; its temporary inbox pointers must not escape.
Both the Wasm dispatcher and RFL decoder validate record lengths/kinds/flags.

Callbacks only enqueue: no synchronous guest reentry during a DOM commit or
application borrow. One animation-frame drain writes at most 64 events into the
4096-cell inbox and makes **one typed dispatch call**. Remaining work gets another
animation frame, allowing input and painting between turns. Event order is
preserved; version 1 performs no coalescing. Queued bindings retired before a
drain are discarded. All records already delivered in one batch are a snapshot;
the guest dispatcher decides whether subsequent messages still apply after an
earlier message changes its model. No host reentry per event is required.

One event must fit the inbox. Queue limits are 1024 records / 524288 cells;
overflow or oversized payload fails explicitly and cleans up, rather than
silently losing input. These are bring-up limits, not streaming paste support.
The bounded adapter does not preempt an infinite synchronous guest computation.

## Measurements and tests

`app.stats` reports host import calls, guest entries, commits, command/patch
counts, UTF-8 octets decoded, event batches/events/cells copied, stale events,
queue/batch peaks, live nodes/listeners and disposal state. Reads require no
guest call. Three browser-profile Wasm allocator counters report successful allocations, live
blocks and live reserved payload cells (including alignment, excluding headers).
They count the bundled allocator, not user replacements; cumulative u32 counters
wrap. No allocation crosses into JavaScript.

Normal unmount must return handles/listeners/queues and owned allocations to
baseline. RFL retains one reusable synchronous call-frame arena per instance;
its warm baseline is not an outstanding application owner. On a guest trap, host ownership is cleaned up and the entire instance
is retired: guest stack/resource unwinding is not promised. Separate counters
distinguish import calls from public guest entries and from setup/diagnostic work.
The demo's counter display is host diagnostics, not application state readback.

```sh
cargo test --release --jobs 2 -p rcc-wasm
cargo test --release --jobs 2 -p rfl --test wasm_browser --test wasm --test wasm_target
```

Node tests execute real compiled Wasm using a small DOM fixture and exercise
wire corruption, UTF-8, stable identities, retirement, crossing budgets,
event batching/overflow and cleanup. The demo's `?test` check additionally uses
the actual browser DOM/event loop. See [browser effects](browser-effects.md) for
timer/fetch transport and mounted executor integration. Typed diffing and JSX are supplied by [the UI library](browser-ui.md).

Verification on 2026-10-18: both the RFL example and RCC counter passed the
real-browser `?test` lifecycle check. Each delivered three clicks in one batch,
performed one update commit, preserved text-node identity, and disposed all DOM
nodes/listeners. The RFL warm frame-arena baseline was one 520-cell block; C
retained no allocations. The bridge releases the instance after final counters.
