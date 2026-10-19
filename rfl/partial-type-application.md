# Partial type application

RFL can fix selected arguments of a first-order constructor while leaving direct
`_` slots open. Slots fill left to right:

```rfl
fn same<F<_>, A>(value: F<A>) -> F<A> { move(value) }
fn main() {
    let value: Result<i16, String> = Ok(42);
    println(same<Result<_, String>, _>(value));
    println(same<Result<i16, _>, _>(Err("failure")));
}
```

`Result<_, String>` has kind arity one, whereas bare `Result` has arity two.
`Triple<_, String, _>` has arity two. `Result<_, _>` is the same constructor as
bare `Result`; repeated equal partial applications share one compiler identity.
Partial application composes, including through an abstract constructor argument.
No partial constructor is a runtime value or a new container representation.

## Inference direction

A known partial constructor projects the varying arguments from an applied type,
checking the fixed arguments for exact equality. Inferring unknown `F<A>` from
`Result<i16, String>` would require choosing which slot varies. RFL requests an
explicit constructor instead of inventing a type function. A result annotation
alone does not select that slot. Constructor arguments carried by generic ADTs
retain their identity, so those arguments can determine F normally.

At the top level of an explicit function/variant argument list, `_` instead asks
ordinary generic inference to solve that parameter. Thus
`map<Result<_, String>, _, _>(value, callback)` fixes F and infers A and B.
Expected result types also constrain remaining parameters. A supplied type must
agree with those expectations; placeholders cannot hide a mismatch. Unsolved
parameters produce an error. `Ok<_, String>(42)` is supported too.

Known arguments also reach inline variants: `same<Result<_, String>, _>(Ok(42))`
fixes the error type before checking Ok's payload, which determines the success
type. The same path handles ordinary generic Result parameters and nested
constructor-bearing variants. Expressions are checked and evaluated once in
source order. Explicit conflicting arguments are still rejected. Callbacks with
an underconstrained return type may still require a result annotation.

These two uses of `_` are contextual: a slot inside a constructor argument stays
open; a top-level call argument requests inference. `_` in a local/field/function
value type, such as `Result<_, String>` as a function parameter, is rejected.
Nested holes such as `Result<Vec<_>, String>` are not type lambdas. Fixed
arguments must be complete value types. General type aliases, higher-order kinds,
GATs and arbitrary type functions remain separate extensions.

## Generic constructor implementations

Constructor traits accept patterns with fixed value parameters:

```rfl
impl<E> Functor for Result<_, E> {
    fn map<A, B>(value: Self<A>, f: fn(A) -> B) -> Self<B> {
        match move(value) {
            Ok(a) => Ok(f(move(a))),
            Err(error) => Err(move(error))
        }
    }
}
```

Here `Self<A>` means `Result<A, E>`. E is an impl parameter; A/B belong to the
method. Universal signature checking, fixed-argument bounds, nominal identity,
orphan rules and unused-declaration checks apply as usual. Every impl parameter
must occur in a fixed position. Constructor-valued impl parameters and blanket
impls over an arbitrary F are still deferred.

Coherence compares the open-slot mask and unifies the fixed argument patterns.
`impl<E> Functor for Result<_, E>` overlaps a `Result<_, String>` instance.
`Result<_, E>` and `Result<T, _>` describe distinct constructors. All-open spelling
and bare spelling overlap. Bounds cannot be used to claim overlapping instances
are disjoint. Partial application does not grant ownership of the underlying
nominal type to the module that mentions it.

Calls through an F bound retain their declared contract during specialization.
For a partial constructor, use a generic function with explicit F or a helper;
bare `Result::map` still identifies the binary constructor. Trait-qualified
operation inference does not guess a partial constructor either.

## Result in std/fp

`Functor` and `Monad` have success-slot instances for `Result<_, E>` with any
valid error type. Err transfers its error unchanged without invoking callbacks.
Ok transfers its payload to map/bind, and pure creates Ok. Both owned success
and owned error payloads obey the existing move and destruction rules.

`map_result(value, f)` and `flat_map_result(value, f)` infer the fixed error type
from the ordinary Result parameter. They call the same generic trait APIs; they
do not add a second implementation. Callbacks returning an underconstrained
variant may still need a result annotation or explicit variant type arguments.

See [examples/fallible-fp.rfl](examples/fallible-fp.rfl). These operations are
eager and use reusable callbacks. Separate [consuming continuations](consuming-callables.md)
now support owned captures. Lazy effects remain the next API/runtime milestone;
this extension does not make println pure.

## Validation

`tests/partial_constructors.rs` covers slot order/composition, constructor-valued
data and canonical identity, rigid checking, fixed-argument bounds, coherence,
imports/orphans, inference conflicts, formatter stability, Result laws,
short-circuiting and owned cleanup. Programs execute on native and RVM, with a
small partial-dispatch case on the Brainfuck CPU. The LSP protocol regression
checks concrete types/signatures, navigation, completion and malformed slots.

```sh
cargo test --release -p rfl --test partial_constructors
target/release/rfl run rfl/examples/fallible-fp.rfl
target/release/rfl run rfl/examples/fallible-fp.rfl --target native
python3 rfl/editors/intellij/tests/lsp_partial_constructors.py target/release/rfl
```
