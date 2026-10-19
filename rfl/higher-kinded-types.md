# Type constructors and functional programming

The first implementation leads with type constructors (higher-kinded types, or
HKTs) and generic functional programming. Functor/Monad contracts are implemented in [generic-traits.md](generic-traits.md); a ZIO-style effect library and
comprehensions build on those contracts. Existing async functions, futures,
ownership and cancellation remain the execution foundation.

## Implemented: first-order constructor parameters

A normal parameter `A` stands for a value type. `F<_>` stands for a constructor
that accepts one value type; `F<_, _>` accepts two. Applying the constructor
produces a value type:

```rfl
fn same<F<_>, A>(value: F<A>) -> F<A> { move(value) }
fn same2<F<_, _>, A, B>(value: F<A, B>) -> F<A, B> { move(value) }

fn main() {
    println(same<Option, i16>(Some(42)));
    println(same([20, 22])); // F = Vec, A = i16
    let result: Result<i16, String> = same2(Ok(42));
    println(result);
}
```

Supported constructors are `Vec`, `Slice`, `Owned`, and generic algebraic data
types whose own parameters all have value kind. User-defined types participate
on the same terms as `Option` and `Result`. Constructor identity is nominal:
matching representation does not make two constructors interchangeable.
Bare `Result` has arity two. Its partial application `Result<_, E>` can fill
an `F<_>` parameter; see [partial type application](partial-type-application.md).

Inference decomposes applications in argument types and expected result types.
It works recursively (`F<F<A>>`), through function signatures and within generic
data types. Generic functions can forward abstract constructors to other generic
functions, explicitly or by inference. Every occurrence must agree on the same
constructor and argument types. Parameters that cannot be solved need explicit
type arguments or an expected type; RFL does not guess a constructor for `None`.

Constructor parameters can also describe data:

```rfl
type Wrapped<F<_>, A> = Wrapped(F<A>);
fn unwrap<F<_>, A>(value: Wrapped<F, A>) -> F<A> {
    match move(value) { Wrapped(inner) => move(inner) }
}
```

A constructor is compile-time metadata, with no value, address or runtime layout.
Bare `F` cannot be a field, local, function argument or collection element. The
checker verifies generic bodies with rigid constructor/application identities,
including unused declarations. A function returning `F<A>` cannot manufacture
`Some(42)` and assume that every caller chose `Option<i16>`.
Concrete calls specialize before IR lowering. The existing container layout,
resource checks, destruction rules and COW behavior apply to the resulting types.
No ISA extension, host runtime dictionary or new BF instruction is involved.

### Initial functional helpers

[`std/fp.rfl`](std/fp.rfl) offers three operations with explicit callbacks:

- `map_with(value, f, mapping)` applies the supplied container map operation.
- `flat_map_with(value, f, binding)` applies the supplied bind operation.
- `transform(value, convert)` changes containers while preserving the element type.

The callback arguments are ordinary existing RFL function values. These helpers
are a working foundation, not implicit `Functor`/`Monad` instances. The mapper or
binder determines evaluation count, short circuiting and effects; the compiler
does not certify its laws. `transform` accepts a callback specialized to the
current element type, rather than a rank-2 natural transformation.

[`examples/type-constructors.rfl`](examples/type-constructors.rfl) uses the same
`map_with` with `Option`, `Vec` and a user-defined `Boxed`. Input and element
callback come before the container operation so inference can resolve the
operation's concrete function signature. A generic function name now specializes
when an expected function signature determines every parameter; bounds and the
complete signature are checked before creating an ordinary callable. An expected result annotation can
resolve a target constructor for `transform`.

### Deliberate boundaries

This milestone does not implement:

- Generic trait parameters (constructor bounds and generic methods are now supported by the [next extension](generic-traits.md)).
- General type aliases, type lambdas or constructors accepting constructors
  (`F<_<_>>`). Direct partial slots such as `Result<_, E>` are now supported.
  `Wrapped` itself still cannot fill `F<_>`.
- Constructor parameters in impl headers, including blanket impls over `F<A>`.
- Generic associated types, higher-rank function values or local polymorphic values.
- Inference of omitted function parameter types through constructor-polymorphic
  calls/data construction. Those enclosing functions need parameter annotations;
  an explicit diagnostic requests them.
- Implicit copies or reusable closure captures of resources. Explicit consuming
  closures are now implemented in [consuming-callables.md](consuming-callables.md).

These boundaries are checked explicitly. Ordinary first-order generics, trait
bounds and associated types retain their current meaning. Constraints on the
parameters of a concrete ADT are still checked when that ADT is applied.

## Generic FP contracts: current checkpoint

1. **Implemented:** generic trait methods, constructor contracts such as
   `Functor<_>`, explicit `F<_>: Functor` bounds and static operation selection.
   See [the contract](generic-traits.md).
2. **Implemented:** direct partial application with explicit kind checking,
   slot-aware coherence and generic fixed arguments. `Result<_, E>` works as a
   unary success constructor. Inference never reverses arbitrary type functions.
   General type aliases remain an ergonomics follow-up. See [the contract](partial-type-application.md).
3. **Implemented with explicit ownership policies:** `Functor` and `Monad` have Option/Vec and fixed-error
   Result instances, with user-ADT examples, laws and ownership tests. They remain
   independent capabilities until supertraits exist. Sequential Apply +
   FromFactory, consuming ApplyOnce and Vec traversal are now implemented;
   [contracts and limits](applicative.md).
4. **Implemented:** `once fn` consuming callables, resource-owning/transitive
   captures and automatic consumption on invocation. `FunctorOnce`/`MonadOnce`
   expose separate Option/fixed-error Result operations, including cleanup when
   a continuation is skipped. Vec retains its reusable callback contracts. See
   [the ownership contract](consuming-callables.md).

Constructor traits now provide explicit static operations over `Self<A>`.
They extend the original receiver-only trait system without runtime witnesses.
Partial application now supports fixed-error Result. Typed lazy effects now distinguish reusable recipes from consuming programs
and independently owned executions.
See [the effect contract](effects.md) and [the runnable example](examples/effects.rfl).
General type aliases remain an ergonomics follow-up. Sequential Applicative and
Vec traversal now build on this foundation.

## Implemented: reusable typed lazy effects

`std/effect` provides reusable `Effect<R,E,A>` recipes. Each run creates its
own owned Async frame/environment and registers fresh finalizers on first poll.
`std/effect_once` retains `OwnedEffect` for exclusive captured resources.
`std/shared` permits retaining and returning the same owned resource across runs;
checked synchronous visits protect its payload, and last-handle cleanup preserves
the original typed destructor. `std/value::Copy` checks value-based constructors;
factory-based constructors can produce fresh exclusive values on every run.

Effect implements Functor/FlatMap/FromFactory; OwnedEffect implements
FunctorOnce/MonadOnce. An unrestricted value-taking Monad::pure cannot duplicate
an exclusive A across runs. Keep binding independent from injection, and use
pure_with for repeatable allocation. Existing eager container Monad instances
continue to accept owned values.

[The contract](effects.md) records all three resource policies, adapters,
repeat/overlap/cancel semantics, environment access and cleanup order. Tests cover
native/RVM execution, generic capabilities and ownership rejection. No new ISA,
background executor, purity checker or constant-space guarantee is introduced.

The allocation performance checkpoint is complete: profiling found malloc/free
consuming 93% of the repeated-chain workload's instructions. The guest allocator
now uses segregated free lists and physical neighbor links. At 256 stages the
same guest objects execute 18.4× fewer instructions; doubling depth costs about
2×, with instruction-budget regressions guarding that behavior. See
[measurements and limits](effects.md#allocation-performance). Further pooling
and allocation/retain reduction remain performance opportunities.

## Implemented: sequential combination and an injectable Console

`std/applicative` combines owned results with Apply/ApplyOnce; explicit Cartesian
Vec helpers require Copy inputs. `std/traverse` provides destination-constructor
contracts for a Vec source, including factories for fresh exclusive inputs.
Effect traversal uses an iterative async loop with cooperative batch yields and
instruction-budget regressions. `traverse_owned_with` lets per-run consuming
children retain inputs across awaits. [Full contract](applicative.md).

`std/console` supplies generic effectful reading/printing through a Console
capability. The same greeting application runs twice against a real standalone
native/Ripple terminal or a deterministic recording interpreter. Service methods
return owned futures so environment references do not cross suspension.
[Contract, examples and platform limits](console.md).

## Implemented: sequential comprehension syntax

`for { a <- first; b <- next(a); yield result }` expands to the existing
FlatMap/Functor operations. `for once { ... }` uses consuming continuations.
Local let declarations and irrefutable parameter patterns retain their source
locations and ordinary ownership checks. The final yield maps a value during
execution and does not introduce unrestricted pure/value copying.

Partial constructors use explicit heads: `for<Result<_,E>>`,
`for<Effect<R,E,_>>` and `for once<OwnedEffect<R,E,_>>`. Simple unary heads are
inferred. Optional binding annotations contextualize both source and callback.
The same generic Console greeting now has a
[comprehension example](examples/comprehensions.rfl).

[The contract](comprehensions.md) documents expansion, eager first-source
evaluation, lazy effect callbacks, moves and cancellation. Guards and refutable
patterns remain explicit errors until a filtering contract is specified;
return/try/await cannot silently cross a generated continuation. Existing loops
and contextual identifiers are unchanged. Editor operations retain source-level
bindings, and helper dependencies remain private.

Next:

1. Improve environment and type-alias ergonomics, then service/layer construction,
   fiber supervision, queued/async shared
   access and asynchronous finalization. Plain Shared access does not serialize
   an entire suspended network exchange. General aliases and associated-output
   equality remain independent ergonomics work.
2. Specify explicit filtering/guard semantics and optional parallel combinators
   independently of the sequential comprehension syntax.

## Comprehension follow-ups

Sequential lowering now uses ordinary checked binding operations. Preserve that
contract when adding guards: refutable patterns and guards need an explicit
failure/filter capability and must not invent a Result error. Independent
parallel operations need an explicit combinator. General type aliases can make
partial effect heads shorter without changing their constructor identity.

## Validation

`tests/type_constructors.rs` covers kinds, rigid declaration checking, inference,
constructor-bearing ADTs, nested applications, ownership and native/RVM parity.
The shared formatter preserves constructor syntax; semantic editor features use
the compiler's type checker. `editors/intellij/tests/lsp_type_constructors.py`
checks constructor aliases, concrete hover, navigation, formatting and kind
diagnostics through the actual stdio protocol. IDEA's lightweight lexer/PSI has no separate grammar
to regenerate. Update the configured compiler to obtain the new diagnostics.

```sh
cargo build --release -p rfl
cargo test --release -p rfl --test type_constructors
python3 rfl/editors/intellij/tests/lsp_type_constructors.py target/release/rfl
target/release/rfl run rfl/examples/type-constructors.rfl
target/release/rfl run rfl/examples/type-constructors.rfl --target native
```
