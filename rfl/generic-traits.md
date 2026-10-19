# Generic methods and constructor traits

This extension builds on [static traits](traits.md) and
[type constructor parameters](higher-kinded-types.md). All dispatch remains static.
The compiler checks declarations before specializing concrete calls, with no
change to the native ABI, Ripple ISA or Brainfuck CPU.

## Generic receiver methods

Ordinary traits can quantify method parameters independently of the implementing
type. Each implementation must work for every type admitted by the method:

```rfl
trait Identity {
    fn identity<T>(&self, value: T) -> T;
}
impl Identity for bool {
    fn identity<A>(&self, value: A) -> A { move(value) }
}
fn use_it<I: Identity, T>(instance: I, value: T) -> T {
    instance.identity(move(value))
}
```

Parameter names may differ between contract and implementation. Their order,
kinds and bounds must match. Implementations may neither add nor remove bounds;
`fn identity<T: Printable>` cannot implement an unconstrained `identity<T>`.
Duplicate parameters and shadowing an impl's parameters are errors. Signature
comparison shares rigid identities by position, rather than guessing their
future concrete types. Unused implementation bodies are checked too. As elsewhere
in RFL, implicit copies of abstract parameters receive their final resource
checks at specialization; a particular instantiation cannot copy a move-only
value. The supplied FP instances use explicit moves and support owned payloads.

Method type arguments currently come from argument types and expected result
types, as with existing generic inherent methods. Explicit `value.method<T>()`
and `Type::method<T>()` syntax remains a separate extension. Free functions can
expose explicit type arguments when a method's constructor/result cannot be
inferred. Ordinary associated outputs such as `Self::Output` can appear alongside
method parameters, with the existing projection and associated-bound rules.

For a non-generic trait method that preserves its receiver type, the expected
result can also reach a generic receiver constructor. For example, assigning
`make().identity()` to `Wrapped<i32>` can infer `make`'s parameter when its declared
return is `Wrapped<T>` and the unique visible operation returns that same
`Wrapped<i32>`. Parenthesized conditional, block and match receivers preserve this
context too. This does not select between ambiguous traits or infer the base of
an associated projection. Bare return parameters such as `id<T>(T) -> T` still
use argument inference instead of being forced to the final method result type.


## Constructor contracts

A constructor trait states the kind of `Self` with underscore slots:

```rfl
trait Functor<_> {
    fn map<A, B>(value: Self<A>, f: fn(A) -> B) -> Self<B>;
}
impl Functor for Option {
    fn map<A, B>(value: Self<A>, f: fn(A) -> B) -> Self<B> {
        match move(value) {
            Some(a) => Some(f(move(a))),
            None => None
        }
    }
}
fn mapped<F<_>: Functor, A, B>(value: F<A>, f: fn(A) -> B) -> F<B> {
    F::map(move(value), f)
}
```

`trait Functor<_>` constrains a unary constructor; it does not declare a named
trait parameter. `trait Binary<_, _>` constrains binary constructors. A plain
trait has value-kind Self. Bounds must match their parameter's kind: `T: Functor`
and `F<_>: Printable` are errors.

Implementations name the bare constructor (`Option`, `Vec`, or a user ADT).
`Option<i16>` is a value type and cannot implement a constructor trait. The [partial-application extension](partial-type-application.md) adds direct slots
and impl parameters in fixed positions, such as `impl<E> Functor for Result<_, E>`.
Higher-order constructor kinds remain deferred. Existing coherence and orphan rules
apply to the defining module of that constructor, including across aliases.

Constructor operations are static functions taking applied values. A bare
constructor has no receiver, so `self`, `&self` and `&mut self` are rejected in a
constructor contract. `Self<A>` works in headers and impl bodies. Associated types
on constructor traits are deferred; this extension does not add GATs.

`F::map` selects from F's declared constructor bounds. That choice is retained
for specialization: unrelated traits and inherent methods on the eventual
container cannot redirect the call. Multiple applicable operations with the same
name are ambiguous. `Functor::map(value, f)` explicitly selects a contract when
the constructor can be inferred from its first applied-Self argument; a method
such as `pure` can use an expected result type instead. Otherwise use the bare
constructor qualifier, for example `Option::pure(42)`, with the trait in scope.

The checker uses signature-only call contracts with an explicit Self parameter
and ordinary generic inference. Concrete calls replace these checking contracts
with real implementation functions before IR lowering. Neither signature-only
functions nor rigid type placeholders enter executable code. Contract checking
must not introduce Self into the lexical scope of argument expressions.

## Functional library

[`std/fp.rfl`](std/fp.rfl) now defines:

- `Functor`: `map`.
- `Monad`: `pure` and sequential `flat_map`.
- Generic free functions `map`, `pure`, `flat_map` and `map_monadic`.
- `Option`, `Vec` and fixed-error `Result` instances for both contracts.
- `map_result` and `flat_map_result` helpers that infer the error type.
- The original explicit-callback helpers `map_with`, `flat_map_with` and `transform`.

Functor and Monad are independent capabilities while supertraits are deferred.
Use `F<_>: Functor + Monad` when both are required. `map_monadic` derives mapping
using Monad alone and a reusable closure capturing the supplied function.
`Result<_, E>` now fixes the error argument; Err short circuits callbacks and
transfers the error unchanged. Applicative and traversal remain follow-ups.

Option preserves None without invoking the callback. Vec mapping and binding
visit inputs left to right; binding appends each returned vector in order.
Injection produces Some or a singleton vector. Implementations are required to
satisfy the documented identity/composition and unit/associativity laws; the
compiler checks types and ownership, not algebraic proofs.

The Vec implementations consume elements through pop rather than ordinary copying
iteration. A temporary reversed vector preserves source order, including for
move-only payloads. This costs extra allocations and linear passes; a consuming
forward iterator is a later performance improvement. Ordinary input aliases keep
their COW snapshots. Mapping/binding transfers owned elements exactly once and
retains deterministic destruction.

Callbacks remain ordinary reusable RFL functions. They can accept move-only
arguments, but cannot capture move-only resources. These contracts do not imply
lazy effect descriptions, suspended execution or implicit parallelism. Separate
`FunctorOnce`/`MonadOnce` contracts now accept consuming callbacks for Option and
fixed-error Result; see [consuming callables](consuming-callables.md).

See [`examples/functors.rfl`](examples/functors.rfl) for core and user ADT instances.

## Lambda result inference

A lambda without an expected result type now obtains its result from its checked
body and explicit return statements. Returning a container or managed value no
longer guesses i16. All explicit returns and a value-producing tail must agree;
missing or incompatible return paths are rejected. Nested lambda return tracking
is isolated. Nested result products are also checked for escaping references,
including references hidden inside tuples. Parameter inference keeps its
existing rules.

A lambda using `?` needs an expected function signature unless an earlier return
has already established its result type. The checker requests an annotation
instead of guessing the outer Option/Result type. Type annotations on the
receiving function value or enclosing generic result can supply that context.

## Editor and validation

The shared parser/formatter accepts constructor trait syntax. Completion after
`F::` and `Self::` lists constructor operations from visible bounds, and trait
qualifier completion shows static operations without adding a fake receiver.
Generic receiver methods appear in member completion; checked calls have
normalized hover, definition links and signature help. IDEA uses the same LSP.
The protocol regression `editors/intellij/tests/lsp_generic_traits.py` exercises
these features over stdio, including imported contracts and unsaved edits.
Workspace rename retains its existing conservative handling of erased types;
this extension does not promise complete constructor-operation rename inference.

Tests cover declaration errors, kind/arity mismatches, bound preservation,
aliases/orphans, method-local versus impl parameters, associated outputs,
frozen dispatch, lambda inference, callback order, COW and owned cleanup.
Functor and Monad law examples compare observable results on native and RVM.
A compact compiled Brainfuck case checks both constructor and generic receiver
dispatch without running a large guest workload.

```sh
cargo build --release -p rfl
cargo test --release -p rfl --test constructor_traits
python3 rfl/editors/intellij/tests/lsp_generic_traits.py target/release/rfl
target/release/rfl run rfl/examples/functors.rfl
target/release/rfl run rfl/examples/functors.rfl --target native
```
