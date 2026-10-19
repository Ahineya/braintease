# Typed RFL browser applications

Status: U1 implemented, 2026-10-18. RFL views, messages, model updates, keyed
reconciliation and command ownership run inside Wasm. The existing
[browser bridge](browser-bridge.md) applies bounded DOM batches and queues events;
JavaScript does not store the model, virtual tree or message dispatcher.

## Run the example

```sh
python3 rcc-wasm/examples/browser/run.py --ui
# Open http://127.0.0.1:8000/rcc-wasm/examples/browser/
# Append ?test=ui for the real-browser regression.
```

The [example](../../rfl/examples/browser-ui.rfl) has a counter, controlled name
input, keyed list, typed component and an async timer command. Reversing the list
preserves its DOM nodes. Re-selecting the same row emits no DOM commit. The
Dispose button closes message ingress, cancels application tasks and releases
models/callbacks before dropping the Wasm instance.

## Model / Msg / update / view

```rfl
import macro {html} from "std/html_macro";
import {Html} from "std/html";
import {program, Cmd} from "std/ui_command";

type Msg = Increment;
fn update(count: i16, msg: Msg) -> (i16, Vec<Cmd<Msg>>) {
    (count + 1, [])
}
fn view(count: &i16) -> Html<Msg> {
    html!{<button on_click={Increment}>Count: {count}</button>}
}
fn main() { program(0, update, view, 8).unwrap(); }
```

`program(initial, update, view, capacity)` returns `Result<Ui<Msg>, String>`.
The update signature has no host capability argument: it returns the next model
and a vector of owned commands. Rendering borrows the model synchronously.
RFL does not enforce a purity effect on ordinary functions; applications should
keep update/view free of side effects and describe work with commands.

`program_with_init(model, init, update, view, capacity)` additionally constructs
initial owned commands once with mounted capabilities. This powers initial
location subscriptions and storage reads; see [routing and TodoMVC](browser-routing.md).

The lower-level `std/ui.mount_ui` accepts
`fn(Model, Msg, Ui<Msg>) -> Model`, for integrations that need explicit access to
the application capability during updates. `Ui.send(message)` queues a message
and returns `UiClosed` or `UiQueueFull` on rejection. `ui.app()` returns the
mounted `BrowserApp`, including its task/effect capabilities. Senders retain
only their mailbox and application ingress, not the model/renderer or task scope.

A delivered event batch first resolves its handlers to owned message snapshots.
Updates then process those messages in order and invoke view once. Changes to
handlers during that render affect later batches. A callback can own an ID or
snapshot; it cannot retain a render's borrowed model.

## Commands and async results

`std/ui_command` provides:

| API | Meaning |
| --- | --- |
| `perform(async_program)` | Own a lazy `Async<Msg>` and send its result as a message. |
| `perform_with(once fn(Ui<Msg>) -> Async<Msg>)` | Defer construction until mounted capabilities are available. |
| `message(msg)` | Queue a message for the next bounded turn. |

Commands execute in written order after update returns. Immediate messages do
not recursively call update. At most 256 messages are processed per turn; more
queued work requests another browser task. Capacity counts queued and active
application roots and must be 1..64. Command admission/queue exhaustion is an
explicit failure, not a dropped command. Lower-level APIs let applications
handle admission errors themselves.

```rfl
async fn delayed() -> Msg {
    sleep(30).unwrap().await;
    TimerDone
}
// In update, returning (model, commands):
(model, [perform(delayed())])
```

For fetch, build an async function taking `BrowserEffects`, then return
`perform_with(once fn(ui: Ui<Msg>) => load(ui.app().effects()))` from update.
The [compiled regression](../../rfl/tests/fixtures/browser-ui-command-fetch.rfl)
shows the complete path. The future constructs the request, owns its result,
and returns a typed message. Request failures can be variants of that message;
there is no JavaScript model/JSON message conversion. `perform` currently accepts
compiler-owned `Async<Msg>`; wrap another Future in an async function to adapt it.

Returning from main keeps the application alive. Closing the mount first revokes
message/application ingress, then cancels roots, then releases model/callback
storage. Task cleanup observes `UiClosed`. Move-only models, messages and
commands are transferred through queues once; unused/rejected values run their
normal cleanup outside mailbox borrows. Ordinary click messages require `Copy`,
since a reusable click handler must be able to produce its message repeatedly.

## Typed views without macros

`std/html` is the ordinary runtime library:

```rfl
node("button", [on_click(Increment)], [text("Count: "), number(count)])
```

`Html<Msg>` describes text, elements and fragments. `Prop<Msg>` includes supported
attributes, controlled `value`, `checked`/`disabled` booleans, event handlers and
sibling keys. Constructors are `text`, `number` (i64), `node`, `svg`, `attribute`,
`value`, `checked`, `disabled`, `key`, and the event functions below.

* `fragment(Vec<Html<Msg>>)` adds list children without a wrapper element.
* `optional(Option<Html<Msg>>)` adds an optional child.
* `key(String)` identifies an element among its flattened siblings. Keys are
  unique across fragments; duplicate keys are errors. Unkeyed nodes match by
  sibling position. A key matches within its parent, not across unrelated lists.
* `svg(tag, props, children)` selects SVG namespace explicitly. A namespace/tag
  change replaces the node even if its key remains equal.

`on_click(message)` produces a fixed copy/COW message. `on_input(fn(String)->Msg)`
uses the owned text value; `on_change(fn(bool)->Msg)` uses checked state. Double-click, blur, focus, keyboard
and composition helpers take `fn(Event)->Msg`. For complete payloads/policies,
use `on_event(kind, flags, fn(Event)->Msg)`, with the constants and pure owned
`Event` type from `std/html_event`. Its fields are listener/node/kind/modifiers,
key, value and checked. This pure type is separate from the low-level
`std/browser.BrowserEvent`; the renderer copies its payload into typed handlers.

`std/browser_render.renderer()` also exposes direct `render(Html<Msg>)` and
`messages(Vec<BrowserEvent>)` operations for custom application architectures.
A renderer is move-only: copying a mount's node/listener identity counters is
rejected by the ownership checker.
Invalid views return `Err` before emitting patches and leave the mounted view
intact. Both direct ADT constructors and builder-generated fragments normalize
through the same validation path.

## The public html! macro

`import macro {html} from "std/html_macro"` loads a user-authored RFL procedural
macro, not a compiler HTML special case. It emits the constructors above, with
normal typing, ownership and hygienic library resolution.

```rfl
html!{<><input value={model.name} on_input={fn(name:String)=>Rename(name)}/><ul>{rows}</ul><svg><circle r="4"/></svg></>}
```

Supported syntax includes nested/self-closing tags, `<>...</>` fragments, raw
UTF-8 text, hyphenated `data-*`/`aria-*` attributes, literal or `{RFL expression}`
properties, typed child expressions and capitalized component functions.
`{count}` works even when `count` is a borrowed integer; no dereference, cast or
`number(...)` wrapper is required. Strings, Unicode characters, booleans and all
integer widths (including full-range u64) become text nodes. Existing `Html<Msg>`
values pass through; `Vec` and `Option` of views become fragments. Nested optional
view collections also compose. Empty optional views add no DOM node. Dynamic text
is always text, including strings containing `<script>` or `&`.

These conversions are ordinary static trait methods in `std/html`, resolved in
the macro's definition scope. `TextChild.html_child<M>(&self) -> Html<M>` describes
textual content independent of the application's message type. Application-owned
display types can implement it. `ViewChild` has an associated `Message` and
`html_child(&self) -> Html<Self::Message>`; existing views and collections retain
their event message type. A child from another application's message type is a
compile error. Both borrow their input synchronously; references never enter the
view. Generic view helpers can use these normal bounds. Outside the macro,
import the appropriate trait to call `.html_child()` directly.

There is no runtime type switch, compiler HTML intrinsic or JavaScript formatting
call. Explicit `text`, `number`, `fragment` and `optional` builders remain usable.
Floating-point children currently need explicit application formatting; they are
not silently cast to integers. Inline whitespace and mixed raw text are meaningful
and preserved. Whitespace-only child runs containing a newline are layout outside
`pre`/`textarea`; they do not add DOM nodes. Explicit whitespace expressions and
entities remain text. The [HTML-aware formatter](../../rfl/formatting.md#standard-html-macros)
uses this rule to indent trees without adding text nodes.

Properties and child expressions evaluate once, in source order. Conditional,
`match` and block child expressions keep the normal expected view type, so
`{if (show) { text("visible") } else { text("") }}` also works. Quoted values
are RFL string literals, not an HTML parser's attribute strings. Raw text decodes
`amp`, `lt`, `gt`, `quot`, `apos` and decimal/hex numeric Unicode entities; unknown,
unclosed or invalid scalar entities produce macro diagnostics. SVG namespace
is inherited through literal SVG descendants. Unknown literal tags/properties
and mismatched closings are compile errors; dynamic property/tag combinations
are checked by the renderer against the bridge schema before patch emission.

A component is an ordinary function returning `Html<Msg>`, for example
`Badge(label: String, children: Vec<Html<Msg>>)`. Component property expressions
are positional arguments in written order, followed by the children vector;
the property labels do not perform named-argument matching. Function signatures
check their types. Components are pure view functions in this milestone; keyed
returned nodes provide DOM identity. Independent component-local state/task
lifecycles and hooks are not implemented.

## Reconciliation and crossing budgets

The renderer stores descriptions, stable node IDs and callback environments in
Wasm. Sibling keys use an open-addressed hash index; retained positions use an
O(n log n) longest increasing subsequence to minimize moves. Unchanged text,
attributes and ordering emit no commands. Stable event kind/policy reuses its
listener ID while replacing the callback environment in Wasm, avoiding listener
churn on each render. Removal retires descendant nodes/listeners and drops their
callback captures.

An ordinary changed render uses one DOM import. Larger renders stream chunks
of at most 1000 commands and 8192 string-pool cells; strings are interned with a
growing hash index per chunk. A 500-row/1001-node render takes three chunks, not
one host call per node. No-op views make zero DOM imports. Pure diffing and
allocation call no JavaScript service. Event lookup currently walks the mounted
binding snapshot; it can be indexed separately if measured workloads justify it.

Controlled properties know which nodes received edits. Even if update rejects
an edit and returns the same model, the renderer can restore the controlled
value. The bridge assigns it only when the actual value differs, preserving
focus/selection for accepted text. Composition listeners are installed
implicitly for controlled text: value writes wait until composition ends,
including model changes from unrelated events. Controlled select values are
applied after option reconciliation. No DOM value readback into Wasm
is needed.

Limits are 4096 view nodes, 64 nesting levels, 64 properties per element, 256
queued messages, and the bridge's 65535 monotonically increasing node/listener
IDs per mount. Text/value limits are 8190 UTF-8 octets; an attribute name plus
value and two length cells must fit 8192 cells. The finite supported tag/property
schema remains the one documented by the bridge. Validated hash hrefs on HTML
anchors and deferred autofocus are supported; raw HTML, scripts, arbitrary
URL attributes and custom elements are not exposed. ID exhaustion fails
explicitly; create a new mount to reset the ID space.

Normal disposal returns guest allocations to the warm synchronous frame arena
(one 520-cell block), then the host drops the entire instance. Allocator-reserved
payload cells may vary slightly with block reuse; tests assert stable live-owner
counts/bounded warm storage and complete application release. Traps retire the
instance without promising RFL stack unwinding.

## Verification and next work

```sh
cargo test --release --jobs 2 -p rfl --lib --test wasm_browser --test macros \
    --test operator_hints --test compile --test traits --test constructor_traits \
    --offline -- --test-threads=1
python3 rcc-wasm/examples/browser/run.py --ui --build-only
```

Tests execute compiled Wasm and cover batched counter messages, unchanged views,
keyed moves/insertion/deletion/tag replacement, fresh and retired handlers,
controlled rejection and composition, attribute removal, large chunked renders,
SVG/fragments, macro hygiene/evaluation order/schema/type errors, move-only
models/messages, queue overflow/reentrant cleanup, async commands/fetch and
normal disposal. Two numeric-hint regressions also preserve dereferenced wide
integers and fixed scalar results of generic calls. `?test=ui` additionally checks a real browser's DOM identity,
input focus/selection, event loop, unchanged-view crossing count and cleanup.

The [browser application roadmap](browser-apps-roadmap.md) keeps the larger
request/search application and adds classic TodoMVC as the SPA showcase, with
hash routing and browser storage. Those capabilities are [implemented](browser-routing.md);
request/search cancellation and component-local resource APIs remain planned.

U2 remains a larger request/search application: typed request errors and
cancellation policies, component-local resource scopes, and repeatable startup/
allocation/diff benchmarks. The foundations above are usable independently of
that example; they do not introduce React hooks, SSR or hydration.

Earlier U1 child-conversion verification on 2026-10-18: 292 release tests passed across the library (97), compiler
integration (105), constructor traits (16), macros (29), numeric hints (4), traits
(21) and compiled-Wasm browser/ownership tests (20). Child regressions cover
borrowed scalars, integer limits, nested optional/list views, message-type
rejection, custom conversions, evaluation order and block-shaped expressions.
The documented `--ui --build-only` command succeeds. The demo passes the
real-browser check, including controlled select initialization and selection,
with eight DOM commits and eleven host calls after the syntax simplification.

Routing/storage/TodoMVC acceptance and reproduction commands are recorded in
[browser-routing.md](browser-routing.md).
