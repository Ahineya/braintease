# Browser application milestones

Decisions locked on 2026-10-18. This extends the [Wasm direction](wasm-browser.md)
and [typed UI](browser-ui.md). Items marked planned are not existing APIs.

## Current foundation

Implemented: typed Model/Msg/update/view; owned async commands; keyed Wasm-side
reconciliation; controlled inputs, composition and selection; bounded DOM/event
batches; lazy fetch/timer futures; application task ownership and cleanup.
Hash routing, local persistence and the TodoMVC application are now implemented.

The public `html!` now accepts direct textual and view children. For example:

```rfl
fn view(count: &i16) -> Html<Msg> {
    html!{<button on_click={Increment}>Count: {count}</button>}
}
```

Strings, characters, booleans and integers use `TextChild`. Existing views and
optional/list views use `ViewChild`, preserving their associated message type.
Both are ordinary statically dispatched library traits. Conversions borrow only
for the call, evaluate the expression once and perform no JavaScript work.

## U2: requests and resource scopes — planned

Keep the larger async request/search application as a separate milestone; a local
TodoMVC application does not prove network cancellation behavior.

* Typed loading/success/empty/error states and finite response/body parsing.
* Latest-request-wins cancellation, explicit request identities, and ignoring
  stale completions without discarding a later request's result.
* Component-local resource scopes owned by stable component identity. Removing
  a component closes ingress, cancels its tasks and releases its state.
* Extend the existing application-owned router subscriptions to component-local
  lifetimes and other browser notifications, without application lifecycle FFI.
* Repeatable compile/download/startup, update/diff and allocation measurements;
  keep crossing counts separate from machine-dependent timing.

## U3: classic TodoMVC and SPA capabilities — implemented

Implemented on 2026-10-18; see [routing, storage and TodoMVC](browser-routing.md)
for the public APIs, wire additions, run command and acceptance tests.

* Typed hash parsing/link generation, initial location subscriptions, push/replace,
  native anchors, bookmarks and browser back/forward. Unknown routes have an
  application fallback. V1 uses literal ASCII paths, with queries/escapes rejected.
* Bounded browser storage commands with explicit unavailable/quota/size failures.
  Encoding/decoding stay in RFL. Initial load never overwrites an unseen snapshot;
  saves are serialized by revision and failures have explicit reset/retry paths.
* Double-click, blur and focus events; deferred focus survives patch chunks.
  Enter/blur commits are idempotent and Escape/IME behavior is explicit.
* TodoMVC add/edit/delete, individual/toggle-all completion, remaining counts,
  clear completed, stable IDs, route filters and persistence across instances.
* Compiled-Wasm tests for history, editing, storage failures, ordering, no-op
  rendering and complete ownership release. A separate optional real-browser
  regression uses isolated storage and checks native DOM behavior.

These application-owned subscriptions do not imply component-local hooks/scopes.
U2 request/search cancellation and performance measurements remain planned; the
independent TodoMVC milestone does not substitute for that network coverage.

## Later

History/path routing with deployment guidance; richer request/response APIs;
reusable component APIs over proven scopes; accessibility checks; measured event
lookup/delegation improvements. Hooks, SSR, hydration and full React compatibility
are independent design decisions, not requirements for TodoMVC.
