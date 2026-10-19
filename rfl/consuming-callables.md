# Consuming callables

`once fn(A) -> B` is a callable value that owns its environment and may be
invoked at most once. `once fn(a: A) => body` constructs one. Ordinary `fn`
values retain their reusable copy/reference-counting behavior.

```rfl
fn invoke(f: once fn(i16) -> String) -> String { f(42) }
fn main() {
    let value = owned("owned", fn(value: &mut String) => {
        println("released");
    });
    let callback = once fn(n: i16) => {
        println(n);
        println(*value.borrow());
    };
    callback(42); // consumes callback, then releases value
    println(invoke(once fn(n: i16) => "called once"));
}
```

## Ownership

Creating a consuming closure transfers each unshared captured binding, including
ordinary values. The original binding is moved and must be reinitialized before
reuse. Mutable unshared captures become mutable locals owned by the callback;
use a named temporary for a borrowed assignment place. An already shared mutable
capture retains the existing shared-box behavior. Exclusive resources cannot be shared
through such a box or captured by a reusable closure. Explicit `std/shared`
handles may be captured by reusable closures and retain the same resource;
access their payload through `with_shared` rather than mutable capture boxing.

Calling a consuming local automatically moves it: `callback(args)` needs no
`move` around the callee. Passing, returning, storing or discarding a named
consuming value still uses `move(callback)`. Aggregates and collections containing
consuming callables are themselves move-only, using existing resource rules.
Owning slices still cannot share resource elements; borrow an element or keep
the vector owner. Copies, repeated calls and transfers on a repeating loop path
are rejected.
Disjoint branches may each consume the same callback; repeating paths must
reinitialize it. Moving or calling while an incompatible loan is live is rejected.

A consuming closure can return another consuming closure or transfer a captured
owner out. Lexical dependencies pass through intervening environments, respecting
parameter, block and pattern shadowing. A reusable intermediate environment may
carry ordinary values, but cannot acquire a resource merely because an inner
closure is consuming. Implicit boxing of a moved mutable capture for mutation
by a nested reusable closure remains unsupported; keep that mutation in the
consuming body.

Borrowed references cannot be captured, stored or returned. A callback may accept
an ephemeral reference parameter and use it during its call. It cannot suspend:
`await` remains restricted to an async function body. An async function may own a
consuming callback across suspension; cancelling or dropping that future releases
any callback still held in its frame.

The two callable types do not implicitly convert. A named RFL function can be
adapted to a consuming signature when that signature is expected, because it has
an empty environment. This also works during generic result inference for
monomorphic function names. Generic function names still need a signature that
solves all their type parameters. Annotate lambda parameters when inference needs
help. `once` is contextual before `fn`, so existing bindings named `once` work.

## Cleanup and representation

An uncalled callback releases its captured resources without executing its body.
On a call, unshared captures transfer from the environment into the callee's
locals; their old cells are cleared. Captured resources then follow normal
scope/return/`?` cleanup, unless transferred to the result. Remaining captures in
an uncalled consuming environment drop in reverse first-capture order. Shared
captures keep their existing lifetime behavior.

A completed indirect call immediately releases its consumed callback temporary.
The four-cell function/environment representation, ordinary frame driver and
Ripple ISA are unchanged. Native, RVM and Brainfuck use the same guest ownership
semantics. Signature cardinality is compiler metadata; it participates in type
identity, inference, generic specialization, trait contract checking and coherence.

Runtime faults still terminate without unwinding, matching existing `Owned`
behavior. This feature does not add asynchronous finalizers or prove stack safety
for arbitrarily deep chains of captured callbacks.

## Consuming FP contracts

`std/fp` provides independent `FunctorOnce<_>` and `MonadOnce<_>` contracts:

- `map_once(value, callback)` transforms zero or one successful values.
- `pure_once(value)` injects an owned value.
- `flat_map_once(value, callback)` composes zero or one successful values.

Option and `Result<_, E>` implement them. None/Err never invoke the continuation;
they release it and its captures. Their existing reusable Functor/Monad instances
remain available. Vec deliberately has no consuming instance: its map/bind may
invoke a continuation for every element.

```rfl
import {FunctorOnce, map_once} from "std/fp";
fn main() {
    let owner = owned(42, fn(value: &mut i16) => { println("released"); });
    let result = map_once<Option, _, _>(Some(1), once fn(n: i16) => {
        *owner.borrow() + n
    });
    println(result); // released, then Some(43)
}
```

These operations remain eager. They establish continuation ownership for the
[consuming effect programs](effects.md), alongside reusable recipes with
explicit fresh/shared resource policies.
Neither callable cardinality nor the FP traits certify purity or algebraic laws.
See [examples/consuming-fp.rfl](examples/consuming-fp.rfl).

## Validation and editor support

`tests/once_functions.rs` checks ownership errors, branch/loop transfers,
transitive captures and shadowing, early returns, generic signatures, nested
managed cleanup, empty/error FP paths and cancellation before/after suspension.
Execution cases compare native and RVM; a bounded compiled BF case checks capture
transfer, invocation and abandonment. The shared parser/formatter and LSP expose
consuming types; IDEA's lightweight lexer accepts the syntax while keeping once
as an identifier. No separate plugin grammar is required.

```sh
cargo test --release -p rfl --test once_functions
python3 rfl/editors/intellij/tests/lsp_once_functions.py target/release/rfl
target/release/rfl run rfl/examples/consuming-fp.rfl
target/release/rfl run rfl/examples/consuming-fp.rfl --target native
```
