# RFL/RCC WebAssembly and browser direction

Status: W1/W2/W3/W4a/W4b/U1/U3 implemented; larger async UI applications are next. Updated 2026-10-18.
The [Wasm backend](../../rcc-wasm/README.md) compiles RCC/RFL applications with
a C16 runtime, typed floats and buffered output. The
[browser bridge](browser-bridge.md) adds typed lifecycle wrappers, DOM batches
and queued events. [Browser effects](browser-effects.md) add batched timers/fetch
and owned RFL futures; the mounted executor now supports ordinary async/await.
The [typed UI and public HTML macro](browser-ui.md) now connect the renderer,
model/messages and owned async commands.

## Decisions

1. Compile shared Ripple IR directly into WebAssembly. Both RCC and RFL benefit;
   no Ripple interpreter or instruction decoder is embedded in the application.
2. Add a Rust backend using `wasm-encoder` and `wasmparser` validation. Cranelift
   remains the native backend; it does not emit Wasm. LLVM is not required.
3. Preserve the C16 compatibility data layout for bring-up. Separate code
   target, data ABI and host environment so a later flat/byte-oriented ABI does
   not require another frontend architecture rewrite.
4. Minimize Wasm–JavaScript crossings and redundant data conversion from the
   first browser milestone. Computation, allocation, ownership, view generation,
   diffing, and task bookkeeping belong in Wasm.
5. Use a small explicit JavaScript host bridge for actual browser operations.
   DOM patches and incoming events use versioned batches in shared linear memory.
6. Start the UI library with typed views, messages and Elm-style state updates.
   Components are ordinary RFL functions with typed props. JSX is macro syntax
   over library operations; the backend has no HTML-specific instructions.
7. Add general function-like procedural macros first. User-defined macros,
   hygiene, diagnostics and editor support are part of that feature. A built-in
   HTML parser alone is insufficient. Declarative macros, derives and attribute
   macros can follow independently.
8. Keep existing ownership, cancellation and typed-effect semantics. Adapt the
   executor to the browser event loop; do not spin `block_on` on the page thread.
9. Keep the Ripple ISA, RVM and BF execution contract unchanged.

## Current foundations and gaps

The shared IR already has typed scalar operations, floating intrinsics, memory,
calls, basic blocks and phis. RFL already supplies ADTs, constructor generics,
traits, closures, managed collections, move-only resources, futures, effects and
comprehensions. Neither additional HKT machinery nor a full new type system is
a prerequisite for a first UI.

Relevant implementation points:

| Location | Current behavior / required change |
| --- | --- |
| `rcc-ir/src/target.rs` | Ripple/native/Wasm retain `RIPPLE16`; Wasm has native floats but no implicit host-math service. |
| `rcc-ir/src/ir/` | Mostly reusable operations, but pointers/layout still carry bank assumptions. |
| `rcc-native/src/units.rs` | Native wrappers now use `rcc-ir/src/link.rs`; shared linking accepts explicit roots, while native still starts at `main`. |
| `rcc-backend/src/optimize/` | Shared optimization is housed in the Ripple backend. Give target-neutral passes a reusable entry point; keep bank/ISA legalization target-specific. |
| `rcc-native-runtime/` | Useful compatibility semantics; desktop/device/process dependencies must not become browser dependencies. |
| `rfl/src/macros/`, lexer/parser/modules | General imported procedural macros, lossless groups and hygiene are independently implemented; see [macro documentation](macros.md). |
| `rfl/std/future.rfl`, `task.rfl`, `scope.rfl`, `effect*.rfl` | Execution/ownership foundations to adapt, rather than replace. |

Explicitly separate IR link roots from a process entry named `main`. Browser
exports, callback dispatchers, initialization and disposal must remain reachable
even when no conventional main function references them. Import/export metadata
must distinguish browser bindings from the private RCC/RFL ABI and host libc.

## Backend contract

### Layout and calls

The first ABI retains 16-bit compatibility cells, existing aggregate layouts and
logical bank/offset pointers. Ordinary RAM maps approximately as:

```text
byte_offset = 2 * (bank * 64000 + cell)
```

Validate logical addresses and complete access ranges; Wasm's physical memory
bounds alone do not establish logical pointer validity. Null, code tokens and
MMIO need their own policies. Unsupported devices fail explicitly. Existing
compatibility stack, heap and pointer limits remain documented limits; retaining
the layout does not imply unlimited Wasm allocation.

Wasm memory pages and Ripple banks are different units. Heap growth, maximum
memory and out-of-memory behavior must be explicit. Initialization should not
inflate the download with zero-filled memory images. Code uses Wasm functions
and indices rather than inheriting a VM code-bank boundary.

Keep the private RCC/RFL calling convention consistent across both frontends.
A compatibility scalar-carrier convention is acceptable for bring-up; it must
not become the public JavaScript API. Public wrappers use explicit typed values,
handles and buffer ranges. Ordinary event/DOM calls must not require one JS
BigInt per private scalar carrier. Indirect calls require signature validation,
with a function table/token mapping separate from data pointers.

RFL's resumable frames and closure ABI can remain initially, including the new
bounded direct-call paths. This does not require a new async transformation.

### Control flow and numeric semantics

Emit structured blocks/loops for ordinary CFGs. Use a per-function dispatcher
fallback for irreducible graphs when necessary for C; avoid routing every
ordinary loop through it. Preserve parallel phi assignments, including cyclic
copies and critical edges. Backend output is compiled Wasm, not VM bytecode.

Preserve integer widths, sign extension, wrapping, shift policies, division and
remainder behavior. Guard operations where raw Wasm would trap differently.
Preserve IEEE comparisons, reinterpretation versus conversion, NaNs and the
existing saturating float-to-integer contract. Do not lower typed floats to
Ripple software helpers before the Wasm backend sees them.

Wasm arithmetic can use `f32`/`f64` without JS calls. Transcendentals need a
separate policy: start with in-Wasm portable math, then improve its algorithms or
link an in-Wasm math library. Do not enable `host_math` merely because JavaScript
has `Math.sin`, accidentally making each hot sine call cross the boundary.
Optional host imports must be explicit and measured. Native-float capability and
availability of target-provided trigonometry are distinct properties.

Generate/link a small Wasm-resident runtime. Port reusable algorithms and ABI
semantics, not the whole desktop runtime. Reject unsupported IR, externals and
inline Ripple assembly explicitly, with locations where available.

## Wasm–JavaScript boundary: performance contract

JavaScript performs actual browser operations; it does not host the RFL heap,
reference counts, application model, VDOM diff or scheduler. Generated modules
use static ES-module glue without requiring generated-source evaluation.

### Outgoing DOM work

Wasm constructs a patch buffer containing node creation/removal, text and
property updates, child ordering and listener metadata. JavaScript applies a
whole batch. Do not cross the boundary per node, attribute, character or diff
comparison. An unchanged render emits no DOM commit.

Use stable node IDs and a string table/pool within a batch. Retain mounted-node
handles between renders. Do not serialize the entire model/tree to JSON on
each update, rebuild unchanged subtrees, or read DOM properties back to discover
application state. Keyed children preserve identity, focus and selection.
Static templates/interning and event delegation are natural further reductions.

The bridge ABI describes buffer units, version, lengths, encoding, ownership,
maximum batch size and failure behavior. Prefer compact metadata and pooled
strings; choose dense-byte staging versus a C16-aware shared-memory view using
measurements. A compatibility String stores one UTF-8 octet per 16-bit cell:
treating its storage as a dense `Uint8Array` is incorrect. Centralize conversion
and avoid repeated copying. Shared linear memory avoids message serialization
but does not make decoding, conversion or per-operation work free.

Refresh JS views after memory growth and specify which ranges are borrowed only
for a call. Never retain a raw pointer to storage that RFL can release or reuse.
Oversized patches can use bounded chunks; chunking is not permission to fall
back to one host call per node. Text is inserted as text; raw HTML, if later
exposed, is a distinct explicit operation.

### Incoming events and asynchronous work

Delegate events where suitable, extract only the required payload, and deliver
queued events in batches. Preserve ordering for semantic events such as clicks,
keys and text composition. Coalescing must be defined per event kind; dropping
arbitrary input is not an optimization. Encode synchronous browser policies
such as `preventDefault` in listener metadata where possible so they do not need
an extra Wasm roundtrip.

Avoid synchronous application re-entry while a model/environment borrow or DOM
commit is active. A JS callback normally queues completion and requests a drain.
Use bounded work per drain, animation-frame batching for rendering, and scheduling
that eventually permits browser input and painting rather than an endless
microtask chain. Timers/Promises wake existing RFL futures. This budget does not
preempt an arbitrary synchronous infinite loop in user code.

Browser HTTP uses fetch and its browser policies. Our guest raw Ethernet/TCP
stack is not the browser transport. Request start, cancellation and completion
are explicit operations, with stale completions ignored using retired/generation
checked identities. Similar lifetime rules apply to listeners and DOM handles.

### Counters and acceptance checks

Instrument host crossings, batch sizes, command counts, bytes decoded/copied,
live handles/listeners, outstanding requests and Wasm allocations. Separately
measure module size, compilation/startup, first render and steady-state updates.

The initial regression expectations are:

* Pure kernels, allocation and diffing execute with zero JS service calls after
  setup, excluding an explicit fault/output operation.
* A normal render flush uses one patch commit, or a documented bounded number
  of size-limited chunks; unchanged views commit nothing.
* A large list does not cause one Wasm–JS crossing per element/property.
* Event delivery is batched per drain where semantics permit it.
* Unmount/disposal returns listener/handle/request counts to their baseline.

These are crossing/work-count checks, not machine-dependent timing assertions.
Network I/O and other actual host services naturally require separate calls.

## UI and effects

The library direction is Model / Msg / update / view, with update producing a
new model and commands whose effect results become messages. A typed `Html<Msg>`
or equivalent tree is independent of the DOM patch bridge. Ordinary constructors
must be usable and testable before JSX is available.

Illustrative future syntax:

```rfl
fn view(model: &Model) -> Html<Msg> {
    html! {
        <div class="counter">
            <button on_click={Decrement}>{"−"}</button>
            <span>{model.count}</span>
            <button on_click={Increment}>{"+"}</button>
        </div>
    }
}
```

Typed props, event payloads, child conversions, optional/list children and SVG
must have ordinary library/type-checking contracts. JSX itself does not bypass
ownership or make code pure. A callback cannot retain the borrowed model from a
render; it can own a snapshot or send a message carrying an ID.

Component identity, keyed reconciliation, controlled input, IME composition,
listener replacement and unmount cleanup need explicit tests. Each mounted
component/application owns a task scope; disposal cancels its work before
releasing state. Shared handles are not an async mutex. Existing Effect and
OwnedEffect resource policies remain in force. React hooks, direct React
interoperability, SSR/hydration and fine-grained reactivity are later design
choices, not prerequisites for this first framework.

## Parallel ownership and integration

| Track | Owner | Primary scope |
| --- | --- | --- |
| Wasm backend | Current backend agent | `rcc-wasm/`, target/layout and shared linking/optimization changes, RCC/RFL target integration, portable runtime and browser bridge/executor |
| Macro system | Agent receiving the separate brief | RFL macro tokens, parsing, expansion, hygiene, macro packages/runner, diagnostics, formatter/LSP/editor support and macro examples/tests |
| UI library | Subsequent integration | Ordinary typed tree/update/effect APIs, renderer and public `html!` package |

The macro track does not wait for Wasm. It uses a versioned executor abstraction
and can bootstrap user-written RFL macro packages through the existing native
compiler in a bounded child process. A future Wasm compiler-host profile uses
that same macro protocol. It is distinct from the browser profile and exposes
no DOM services. See the brief for trust, determinism and limits.

Shared touchpoints are workspace manifests/lockfile, `rfl/src/lib.rs`, module
resolution and target-aware cfg parsing. Make focused additive edits and retain
other work. The backend owner handles new target variants and exhaustive target
matches; the macro owner consumes a caller-target context without independently
inventing a Wasm target. Macro expansion is target-independent after its
declared cfg/context inputs have been accounted for.

## Milestones and completion evidence

| Stage | Deliverable | Evidence |
| --- | --- | --- |
| W1 — implemented | Target/ABI/import/export contract, shared link roots, minimal `rcc-wasm` | Validated integer modules executed by Node; nested loops, cyclic phis, direct recursion, multi-unit private symbols, irreducible fallback and explicit errors |
| W2 — implemented | Memory, globals, runtime, indirect calls, typed floats, RCC/RFL compile/run integration | Real Wasm execution, native/RVM differential C, managed values/closures, C imports, portable math, faults and buffered output |
| W3 — implemented | Small browser ES-module bridge, typed wrappers, ordinary RFL/C APIs and instrumentation | Unicode streaming, 500 nodes in one commit, queued click/input/key delivery, stale binding rejection and complete disposal |
| W4a — implemented | Batched browser host effects and owned RFL futures | Timer/fetch overlap, cancellation and late replies, streaming body bounds, C demo and real-Wasm regressions |
| W4b — implemented | Owned mounted RFL application root and browser executor | Persistent task scopes, bounded polling, clock/deadline hints, ordinary async/await demo and disposal checks |
| M1 — independently implemented | General macro system | User-written imported macros, hygiene, diagnostics, cache/limits, editor protocol tests and JSX-shaped example |
| U1 — implemented | Typed UI plus public JSX macro integration | Counter, controlled input and keyed list with crossing budgets and lifecycle checks |
| U2 | Async application | Search/request example with cancellation, typed errors, component cleanup and repeatable performance measurements |
| U3 — implemented | SPA capabilities and classic TodoMVC | Hash router, bounded browser storage, editing/focus/keyboard behavior, persistent todos and back/forward tests |

U1 is implemented in `std/html`, `std/browser_render`, `std/ui`,
`std/ui_command` and the user-authored `std/html_macro`. Views reconcile in Wasm;
keyed children preserve identity, unchanged views commit nothing, and pure update
functions return owned commands whose results become messages. See the
[API, demo, budgets and limits](browser-ui.md). Next is U2: a larger request/search
application with typed errors, cancellation policies, component-local resource
scopes and repeatable performance measurements. Classic TodoMVC is now implemented as the
single-page application showcase, with `#/`, `#/active` and `#/completed`, deep
links/back/forward, controlled editing and persistent state. The
[browser application roadmap](browser-apps-roadmap.md) defines the supporting
router/storage APIs and acceptance checks; see [the implementation](browser-routing.md). Typed child expressions such as
`{model.count}`, `{name}` and `{rows}` now use static library conversions.
The Node console harness remains
a separate profile without DOM or a browser event loop.

W2 preserves fixed 256 × 64000-cell memory (500 Wasm pages), three logical stack
banks, up to 16 global banks, and a contiguous heap above globals. It implements
checked pointers/function tokens, allocation reuse, sparse data, typed float
operations and an output import that accepts i32 byte addresses and C16 cell
counts. Pure computation/allocation require no imports. See the backend README
for ABI limits, supported runtime functions and fault behavior. Variadics, raw
devices, files/networking and host-reactor adaptation remain outside this slice.

W1 verification on 2026-10-18: `rcc-ir` and `rcc-wasm` suites pass, including
3,795 arithmetic/comparison executions and structured/fallback CFG tests.
The same call/loop/phi fixture returns 55 in Node Wasm, native and RVM; its
standalone Wasm image is 245 bytes with zero imports. A small RCC C → IR →
Wasm call example also returns 42. Existing native object/liveness checks pass.


W2 verification on 2026-10-18: the backend execution suite covers 19 cases,
including the integer matrix, typed float edges, bank crossings, callback tables,
heap reuse, invalid pointers/signatures, double free, stack exhaustion and output
batching. Printing 20,001 octets performs exactly three host writes. Four RFL
integration cases plus Wasm cfg pass; the traits example and a large-angle sine
check also execute. The registered C runtime case prints seven `Y`s in RVM,
native and Wasm. The shared call/loop/phi fixture still agrees on all targets.
The runnable [Wasm example](../../rfl/examples/wasm.rfl) combines managed strings,
closures, comprehensions and in-Wasm floating math without a host reactor.

W3 implementation on 2026-10-18: typed i32 public lifecycle/inbox wrappers,
optional DOM imports, a Wasm patch builder, `std/browser`, and a static ES-module
adapter. A 500-node synthetic render is one import; a 130-click burst is bounded
to 64 records per frame. Tests cover preflight corruption, Unicode split across
output buffers, key/input snapshots, node/listener retirement, overflow, and
normal disposal to zero live DOM/listener counts and the warm allocation baseline. The low-level
RFL example and stateful C counter share the bridge. See the
[protocol and runnable demo](browser-bridge.md) for exact limits. Typed Html/Msg
diffing is supplied by U1; mounted task scopes are covered by W4b below.

W4a adds optional typed effect imports/exports, one outgoing batch per guest turn,
bounded queued completion batches, cancellation/late-reply retirement, streamed
GET body limits, owned RFL futures and a runnable C timer/fetch demo.

W4b adds `std/browser_app`: a compiler-retained typed application root owns the
scope across entries and closes it on disposal. Ordinary async functions can
await browser fetch, use standard clock sleeps, yield, and spawn other tasks.
Copyable application capabilities retain ingress/effects state without retaining
their own scope. One bounded turn follows each active entry; a clock sample rides
on that entry, and deadline/self-wake hints share the outgoing effect batch.
There is no idle polling or per-task host call. The demo has reload/cancel buttons
and no user-written lifecycle FFI. Tests cover root ownership, nested spawning,
capacity and reentrant cleanup, clock wrap, repeated fetches, stale callbacks and
cleanup during in-flight I/O. Try `python3 rcc-wasm/examples/browser/run.py --async`;
see [the executor API and wire profile](browser-effects.md).

## References

* [Native compatibility target](../../rcc-native/README.md)
* [RFL effects](../../rfl/effects.md), [async](../../rfl/async.md),
  [constructor generics](../../rfl/higher-kinded-types.md)
* [Macro implementation brief](macros-agent-brief.md)
* [wasm-encoder](https://docs.rs/wasm-encoder/latest/wasm_encoder/)
  and [wasmparser validation](https://docs.rs/wasmparser/latest/wasmparser/struct.Validator.html)
* [Wasm control instructions](https://webassembly.github.io/spec/core/syntax/instructions.html)
  and [JavaScript interface](https://webassembly.org/getting-started/js-api/)
* [HTML event loops](https://html.spec.whatwg.org/multipage/webappapis.html#event-loops)
  and [Fetch CORS](https://fetch.spec.whatwg.org/#http-cors-protocol)
* [Rust procedural macros](https://doc.rust-lang.org/reference/procedural-macros.html)
  and [Elm architecture](https://guide.elm-lang.org/architecture/)
