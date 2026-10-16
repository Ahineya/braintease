# RFL — Traits
**Language Specification · Draft 0.3**

**Status:** Implemented in `rfl` (16 October 2026); see [implementation notes](IMPLEMENTATION.md#static-traits-and-library-printing)
**Compiler:** `rfl`  
**Target:** Shared IR (Ripple VM/BF and native)
**Dependencies:** Generics, monomorphization, inherent methods, extension methods, module system

This specification extends RFL 0.1.

It introduces static traits, trait implementations, generic trait bounds, associated types, and trait-based method resolution.

It does **not** introduce dynamic dispatch, inheritance, trait objects, or runtime reflection.

---

## 1. Design philosophy

RFL traits describe capabilities that types can implement.

A trait declares required methods and may declare associated types.

A type implements a trait by providing those methods.

Generic functions may require their type parameters to implement specific traits.

### Core principles

1. Traits are statically resolved.
2. Trait implementations are explicit.
3. Generic functions use trait bounds.
4. Trait methods compile to ordinary functions.
5. Trait dispatch requires no runtime metadata.
6. Traits do not replace inherent methods or extension functions.
7. Ambiguous implementations are compile-time errors.
8. No implicit conversions or automatic trait implementations exist.

**A trait is a compile-time contract, not a runtime object.**

---

# 2. Trait declarations

Traits use the `trait` keyword.

```rust
trait Printable {
    fn print(&self);
}
```

A trait declaration contains method signatures without implementations and optional associated type declarations (section 23).

Methods may accept ordinary parameters and return values.

```rust
trait Formatter {
    fn format(&self, prefix: String) -> String;
}
```

### Rules

- A trait has a unique name within its defining module.
- Trait methods have no bodies.
- Trait methods must declare their parameter and return types explicitly.
- The receiver must be `self`, `&self`, or `&mut self`.
- Traits may contain multiple methods.
- Traits cannot contain fields or stored state.
- Default method implementations are not supported in RFL 0.2.

An omitted return type means `unit`.

### Exporting traits

Traits participate in the existing module system.

```rust
export trait Printable {
    fn print(&self);
}
```

Other modules can import them normally:

```rust
import { Printable } from "./printing";
```

---

# 3. Trait implementations

A type implements a trait using:

```rust
impl Trait for Type {
    // Implementations
}
```

Example:

```rust
struct Point {
    x: i16,
    y: i16,
}

impl Printable for Point {
    fn print(&self) {
        write_str("Point(");
        write_i16(self.x);
        write_str(", ");
        write_i16(self.y);
        write_str(")");
    }
}
```

Here `write_str` and `write_i16` represent low-level output functions provided by the standard library. They are not generic `print` calls.

Usage:

```rust
let p = Point { x: 10, y: 20 };

p.print();
```

Output:

```text
Point(10, 20)
```

### Implementation requirements

An implementation must:

- Provide every method required by the trait.
- Match the method names and signatures.
- Use compatible receiver types.
- Match return types.
- Satisfy the trait's generic constraints.

Missing, additional, or incompatible trait methods are compile-time errors.

An implementation cannot introduce additional methods into the trait's interface.

Additional type-specific methods belong in ordinary inherent `impl` blocks.

### Inherent methods remain independent

```rust
impl Point {
    fn distance_squared(&self) -> i16 {
        self.x * self.x + self.y * self.y
    }
}
```

This is unrelated to `Printable`.

Both forms of `impl` coexist.

---

# 4. Generic trait bounds

Traits may constrain generic type parameters.

```rust
fn print<T: Printable>(value: T) {
    value.print();
}
```

This function accepts any type implementing `Printable`.

Valid:

```rust
print(42);
print("Hello");
print(Point { x: 10, y: 20 });
```

Assuming the standard library provides `Printable` implementations for integers and strings.

Invalid:

```rust
struct Secret {
    value: i16,
}

print(Secret { value: 42 });
```

Compiler error:

```text
error: type `Secret` does not implement `Printable`

  required by:
    print<T: Printable>(value: T)
```

### 4.1 Bounds are checked statically

For:

```rust
fn display_twice<T: Printable>(value: T) {
    value.print();
    value.print();
}
```

The compiler knows `T` supports `Printable::print`.

No runtime type inspection is necessary.

### 4.2 Multiple bounds

Multiple constraints use `+`:

```rust
fn process<T: Printable + Serializable>(value: T) {
    value.print();
    value.serialize();
}
```

All constraints must be satisfied.

For RFL 0.2, constraints are declared directly on generic parameters.

`where` clauses are not required.

### 4.3 Inferred type parameters

As with ordinary generic functions, generic parameters can be inferred from arguments.

```rust
print(42);
```

The caller does not need to write:

```rust
print<i16>(42);
```

---

# 5. Generic trait implementations

Traits may be implemented for generic types.

This is essential for collections and ADTs.

Consider:

```rust
type Option<T> =
    | None
    | Some(T);
```

We want `Option<T>` to be printable whenever `T` is printable.

```rust
impl<T: Printable> Printable for Option<T> {
    fn print(&self) {
        match self {
            None => {
                write_str("None");
            }

            Some(value) => {
                write_str("Some(");
                value.print();
                write_str(")");
            }
        }
    }
}
```

In a `match` over `&Option<T>`, payload bindings are read-only references. The pattern match does not move the contained value.

Now:

```rust
print(Some(42));
print(Some("Hello"));
print(None as Option<i16>);
```

Works.

But:

```rust
struct Secret {
    value: i16,
}

print(Some(Secret { value: 42 }));
```

Fails because `Secret` does not implement `Printable`.

### Generic implementation semantics

The compiler establishes:

```text
T implements Printable
          │
          ▼
Option<T> implements Printable
```

This condition is checked during generic instantiation.

No implementation is generated for types that do not satisfy the required bounds.

---

# 6. Compositional trait resolution

Consider the standard library:

```rust
impl<T: Printable> Printable for Vec<T> {
    fn print(&self) {
        write_str("[");

        let mut first = true;

        for (item in self) {
            if (!first) {
                write_str(", ");
            }

            item.print();
            first = false;
        }

        write_str("]");
    }
}
```

Now:

```rust
print([1, 2, 3]);
```

Produces:

```text
[1, 2, 3]
```

More interestingly:

```rust
print([Some(1), None, Some(3)]);
```

Produces:

```text
[Some(1), None, Some(3)]
```

The compiler establishes:

```text
i16 implements Printable
         │
         ▼
Option<i16> implements Printable
         │
         ▼
Vec<Option<i16>> implements Printable
```

**Trait satisfaction is recursive and compositional.**

This is the principal mechanism through which RFL supports generic behavior across nested types.

---

# 7. Method resolution

This section extends the existing method and extension-method specification.

Given:

```rust
value.method(a, b)
```

The compiler resolves the call using the static type of `value`.

### Resolution order

**1. Inherent methods**

Search methods declared in:

```rust
impl Type {
    // ...
}
```

**2. Trait methods**

Search methods supplied by traits that:

- Are visible in the current lexical scope.
- Are implemented by the receiver type, or guaranteed by its generic bounds.
- Provide a method with the requested name.

**3. Extension functions**

If no inherent or trait method matches by name, search visible receiver-first free functions.

The precedence is:

```text
Inherent methods
       ↓
Trait methods
       ↓
Extension functions
```

### Important: no overload resolution by signature

Within each category, RFL does not select between same-named candidates merely by trying different argument types.

An inherent method with the correct name but incorrect arguments produces an error rather than falling back to a trait or extension function.

Likewise, multiple applicable trait methods with the same name are ambiguous.

This avoids surprising changes in method resolution.

---

# 8. Trait method ambiguity

Consider:

```rust
trait Printable {
    fn render(&self);
}

trait Serializable {
    fn render(&self);
}
```

And:

```rust
impl Printable for Point {
    fn render(&self) {
        // ...
    }
}

impl Serializable for Point {
    fn render(&self) {
        // ...
    }
}
```

If both traits are in scope:

```rust
point.render();
```

The call is ambiguous.

RFL does not guess.

### Explicit disambiguation

Use a trait-qualified call:

```rust
Printable::render(&point);
Serializable::render(&point);
```

This syntax explicitly selects the trait.

The receiver must still satisfy the trait's implementation requirements.

### Rule

**An ambiguous method call is always a compile-time error.**

There is no implicit preference between unrelated traits.

---

# 9. Trait visibility and imports

Trait declarations follow ordinary module visibility rules.

Example:

**`printing.rfl`**

```rust
export trait Printable {
    fn print(&self);
}
```

**`main.rfl`**

```rust
import { Printable } from "./printing";
```

Importing a trait makes its methods available for method resolution.

### Trait implementations are globally discoverable

An important distinction:

**Trait declarations must be in scope to use their methods through dot syntax, but trait implementations do not require separate imports.**

For example:

```rust
import { print } from "std/io";

fn main() {
    print(Some(42));
}
```

The caller does not need to import the implementation of `Printable` for `Option<i16>`.

The compiler discovers implementations from the complete module dependency graph and linked standard library.

The generic function's bound determines which implementation is required.

### Standard prelude

RFL may provide a small automatically imported prelude containing common types and traits.

For version 0.2, `Printable` is included in the standard prelude.

This allows:

```rust
value.print();
```

For printable values without explicitly importing `Printable`.

Other user-defined traits require ordinary imports.

---

# 10. Coherence and overlapping implementations

**There must be at most one applicable implementation of a particular trait for a particular concrete type.**

Consider:

```rust
impl<T: Printable> Printable for Vec<T> {
    // ...
}
```

Then:

```rust
impl Printable for Vec<i16> {
    // ...
}
```

This is forbidden.

The implementations overlap.

Even if the second implementation is more specific, RFL does not implement specialization.

### 10.1 No specialization

RFL 0.2 does not support:

- Prioritized implementations.
- Specialized implementations.
- Negative implementations.
- Fallback implementations.
- Implicit implementation selection based on source order.

### 10.2 Conservative overlap checking

Two generic implementations conflict if their type patterns can overlap for any possible instantiation.

Bounds cannot be used to distinguish otherwise overlapping implementations.

For example:

```rust
impl<T: Printable> MyTrait for Vec<T> {
    // ...
}

impl<T: Serializable> MyTrait for Vec<T> {
    // ...
}
```

This is rejected because a type might satisfy both constraints.

### 10.3 Orphan rule

An implementation may be declared only in a module that defines at least one of:

- The trait being implemented.
- The nominal type receiving the implementation.

For generic types, ownership belongs to the module defining the outer nominal type.

Thus, a user's module may implement a standard trait for its own `Point`, or its own trait for standard `Vec<T>`, but not a standard trait for standard `Vec<T>`.

Compiler-provided primitive types are considered owned by the core language library for this purpose.

This prevents unrelated third-party modules from arbitrarily implementing foreign traits for foreign types.

The compiler additionally checks the complete build graph for overlapping implementations, including conflicts between the owning modules.

---

# 11. Traits and inherent methods

Trait implementations do not replace inherent implementations.

Example:

```rust
struct Point {
    x: i16,
    y: i16,
}

impl Point {
    fn print(&self) {
        write_str("Point inherent");
    }
}

impl Printable for Point {
    fn print(&self) {
        write_str("Point trait");
    }
}
```

Then:

```rust
let p = Point { x: 10, y: 20 };

p.print();
```

Calls the inherent method.

However:

```rust
Printable::print(&p);
```

Calls the trait implementation.

And:

```rust
fn print_generic<T: Printable>(value: T) {
    value.print();
}
```

Calls the trait implementation.

This last distinction is important.

**Methods inside generic functions are resolved using the declared bounds of the generic type parameter, not by inspecting the eventual concrete type's inherent methods.**

Therefore:

```rust
print_generic(p);
```

Calls `Printable::print`.

Generic code retains consistent behavior across specializations.

---

# 12. Traits and extension methods

Extension functions remain unchanged.

```rust
fn squared(x: i16) -> i16 {
    x * x
}
```

This still works:

```rust
let x = 10.squared();
```

Trait methods and extension functions are distinct mechanisms.

| Feature | Inherent methods | Trait methods | Extension functions |
|---|---|---|---|
| Associated with a type | Yes | Through implementation | No |
| Declared inside `impl` | Yes | Yes | No |
| Requires a trait | No | Yes | No |
| Supports generic bounds | Yes | Yes | Yes |
| Available to generic `T` through a bound | No | Yes | No |
| Static dispatch | Yes | Yes | Yes |
| Runtime metadata | No | No | No |

An extension function does not automatically establish a trait implementation.

For example:

```rust
fn print(value: Point) {
    // ...
}
```

Does **not** make `Point` implement `Printable`.

An explicit `impl Printable for Point` is required.

---

# 13. Traits and the type system

Trait bounds participate in static type checking.

Consider:

```rust
fn foo<T: Printable>(value: T) {
    value.print();
}
```

The compiler's typing environment records:

```text
T: Printable
```

Within `foo`, the compiler may use any methods guaranteed by `Printable`.

It cannot assume unrelated methods exist.

### Example

```rust
fn foo<T: Printable>(value: T) {
    value.print();

    value.serialize(); // Error
}
```

Unless `T` also has a `Serializable` bound, the second call is invalid.

This remains true even if some concrete types implementing `Printable` happen to have an inherent `serialize()` method.

### Type inference

Trait constraints may be inferred from calls to already-constrained generic functions.

However, **RFL 0.2 requires explicit bounds on generic function definitions that invoke trait methods on unconstrained type parameters.**

For example:

```rust
fn foo<T: Printable>(value: T) {
    value.print();
}
```

Is valid.

But:

```rust
fn foo<T>(value: T) {
    value.print();
}
```

Is rejected unless `T` is otherwise known to be a concrete type.

The compiler does not infer a new implicit trait bound from an unknown method name.

This keeps generic function interfaces predictable and avoids requiring a sophisticated constraint-inference system.

---

# 14. Trait compilation

RFL traits use **static dispatch and monomorphization**.

No runtime trait objects are generated.

Consider:

```rust
fn print<T: Printable>(value: T) {
    value.print();
}
```

Called as:

```rust
print(42);
print(true);
```

The compiler creates concrete specializations:

```text
print<i16>
print<bool>
```

Each specialization invokes the corresponding `Printable` implementation.

Conceptually:

```text
print<i16>
    └── Printable::print for i16

print<bool>
    └── Printable::print for bool
```

These are ordinary function calls.

### 14.1 Trait implementation lowering

```rust
impl Printable for Point {
    fn print(&self) {
        // ...
    }
}
```

Lowers to a uniquely named Ripple IR function, conceptually:

```text
Point__Printable__print
```

The exact mangling scheme is implementation-defined.

### 14.2 Trait calls

```rust
point.print();
```

After method resolution becomes an ordinary call to the implementation function.

### 14.3 Generic trait calls

A generic call is resolved during monomorphization.

```rust
print<Point>(point);
```

Requires locating the unique implementation of `Printable` for `Point`.

### 14.4 No additional Ripple instructions

Traits require:

- A trait-definition table.
- An implementation registry.
- Trait-bound checking.
- Coherence checking.
- Trait-aware method resolution.
- Specialization-time implementation lookup.

They do not require changes to the Ripple ISA.

They may require new metadata in RFL's typed AST and specialization machinery, but do not require a new runtime dispatch mechanism.

---

# 15. Traits and C interoperability

Traits are a native RFL construct.

They do not cross the C ABI directly.

Invalid:

```rust
extern "C" {
    fn process<T: Printable>(value: T);
}
```

C has no corresponding representation for a generic trait bound.

However, C-compatible structures may implement RFL traits.

```rust
#[repr(C)]
struct Point {
    x: c_int,
    y: c_int,
}

impl Printable for Point {
    fn print(&self) {
        // ...
    }
}
```

Likewise, trait implementations may invoke ordinary C functions.

```rust
extern "C" {
    fn putchar(c: c_int) -> c_int;
}

impl Printable for MyChar {
    fn print(&self) {
        putchar(self.value as c_int);
    }
}
```

RFL trait dispatch remains internal to RFL.

The C ABI sees ordinary functions and values.

---

# 16. Standard library: Printable

RFL 0.2 introduces the standard trait:

```rust
export trait Printable {
    fn print(&self);
}
```

It is defined in `std/io` and included in the standard prelude.

### 16.1 Generic printing

The standard library exports:

```rust
export fn print<T: Printable>(value: T) {
    value.print();
}
```

And:

```rust
export fn println<T: Printable>(value: T) {
    value.print();
    write_str("\n");
}
```

Here `write_str` is the underlying output primitive, so `println` does not recursively invoke generic printing.

### 16.2 Standard implementations

The standard library provides `Printable` implementations for at least:

| Type | Representation |
|---|---|
| `i8`, `i16`, `i32`, `i64` | Signed decimal |
| `u8`, `u16`, `u32`, `u64` | Unsigned decimal |
| `bool` | `true` or `false` |
| `char` | Character |
| `String` | String contents |
| `Vec<T>` | Bracketed elements when `T: Printable` |
| `Option<T>` | Constructor representation when `T: Printable` |
| `Result<T, E>` | Constructor representation when both types are printable |

Floating-point implementations may be provided when the corresponding runtime support is available.

### 16.3 Example

```rust
fn main() {
    print(42);
    print(" hello ");
    println([Some(1), None, Some(3)]);
}
```

Output:

```text
42 hello [Some(1), None, Some(3)]
```

No runtime reflection.

No dynamic dispatch.

No format-string parsing.

No intermediate string allocation is required by the printing contract.

Each type decides how to write its representation.

---

# 17. Future formatting traits

`Printable` deliberately describes direct output.

Other representations may eventually use separate traits.

For example:

```rust
trait Debug {
    fn debug_print(&self);
}
```

Or a more general output abstraction:

```rust
trait Display {
    fn format(&self, out: &mut Writer);
}
```

Such a trait would support formatting into strings, files, network streams, and other output destinations.

However, neither is required by RFL 0.2.

**The first trait system should solve the actual printing problem without requiring a complete formatting subsystem.**

---

# 18. Explicitly excluded features

Associated types are supported by the 0.3 extension in section 23.
The following remain unsupported:

| Feature | Status |
|---|---|
| Trait objects (`dyn Trait`) | Deferred |
| Dynamic dispatch | Deferred |
| Virtual method tables | Not required |
| Default trait methods | Deferred |
| Trait inheritance / supertraits | Deferred |
| Generic trait parameters | Deferred |
| Specialization | Excluded |
| Negative implementations | Excluded |
| Automatic trait derivation | Deferred |
| Blanket implementations with overlapping matches | Excluded |
| Implicit trait implementations | Excluded |
| Trait-based operator overloading | Excluded |
| Runtime trait lookup | Excluded |
| Reflection | Excluded |

**Traits are exclusively compile-time contracts in this version.**

---

# 19. Required compiler behavior

An implementation of RFL 0.2 is conforming to this specification when it supports the following:

| Requirement | Expected behavior |
|---|---|
| Parse trait declarations | Accepts `trait Name { ... }` |
| Parse trait implementations | Accepts `impl Trait for Type` |
| Validate implementations | Checks all required method signatures |
| Resolve generic bounds | Verifies `T: Trait` constraints |
| Resolve generic implementations | Supports `impl<T: Trait> Trait2 for Type<T>` |
| Detect missing implementations | Compile-time error |
| Detect overlapping implementations | Compile-time error |
| Apply orphan rule | Rejects illegal foreign implementations |
| Resolve trait methods | Uses static receiver types and declared bounds |
| Detect method ambiguity | Compile-time error |
| Support qualified calls | `Trait::method(&value)` |
| Monomorphize generic calls | Generates concrete implementations |
| Lower trait calls | Produces ordinary Ripple IR calls |
| Integrate with modules | Finds implementations across dependency graph |
| Implement standard `Printable` | Supports generic output |

---

# 20. Acceptance test

This is the test I'd use to validate the initial implementation.

```rust
trait Printable {
    fn print(&self);
}

type Option<T> =
    | None
    | Some(T);

impl Printable for i16 {
    fn print(&self) {
        write_i16(*self);
    }
}

impl<T: Printable> Printable for Option<T> {
    fn print(&self) {
        match self {
            None => write_str("None"),

            Some(value) => {
                write_str("Some(");
                value.print();
                write_str(")");
            }
        }
    }
}

fn print<T: Printable>(value: T) {
    value.print();
}

fn main() -> i16 {
    print(42);
    write_str("\n");

    print(Some(123));
    write_str("\n");

    0
}
```

For this self-contained test, `write_i16` and `write_str` are assumed to be existing low-level output primitives or explicitly imported from the runtime. The test defines `Printable` locally rather than importing the standard trait, so it does not depend on prelude implementations.

Expected output:

```text
42
Some(123)
```

Expected return value:

```text
0
```

This exercises trait declarations, concrete implementations, generic implementations, generic trait bounds, static method resolution, nested ADT pattern matching, and monomorphized calls.

---

## 21. Recommended implementation order

I'd implement traits in five stages.

**Stage 1 — Concrete traits**

Support:

```rust
trait Printable {
    fn print(&self);
}

impl Printable for Point {
    fn print(&self) {
        // ...
    }
}
```

No generics yet.

**Stage 2 — Generic bounds**

Support:

```rust
fn print<T: Printable>(value: T) {
    value.print();
}
```

Resolve concrete implementations during monomorphization.

**Stage 3 — Generic implementations**

Support:

```rust
impl<T: Printable> Printable for Option<T> {
    // ...
}
```

This unlocks compositional generic behavior.

**Stage 4 — Coherence and ambiguity**

Implement overlap detection, orphan rules, and explicit trait-qualified calls.

**Stage 5 — Standard library**

Provide the standard `Printable` implementations for primitive types, strings, ADTs, and collections.

---

## 22. Design summary

RFL now has three distinct forms of reusable behavior.

**Inherent methods** describe operations naturally associated with a particular type.

```rust
point.distance_squared();
```

**Extension methods** let ordinary functions use method-call syntax.

```rust
numbers.map(fn(x) => x * 2);
```

**Traits** define capabilities required by generic code.

```rust
fn print<T: Printable>(value: T) {
    value.print();
}
```

All three use static dispatch.

All three compile into ordinary function calls.

None requires runtime objects or virtual tables.

The distinction is entirely about how functions are declared, discovered, and type-checked.

# 23. Associated types (0.3)

A trait can name types chosen by each implementation. They are part of the static
contract and resolve to ordinary concrete types before lowering. They require no
runtime tags, vtables, or ISA changes on Ripple, Brainfuck, or native targets.

```rust
trait Produce {
    type Output: Printable;
    fn produce(&self) -> Self::Output;
}

impl Produce for i16 {
    type Output = String;
    fn produce(&self) -> Self::Output { "hello" }
}

fn show<T: Produce>(value: T) -> T::Output {
    let output = value.produce();
    output.print();
    output
}
```

Every implementation must define every associated type exactly once. Extra names,
duplicate definitions, missing definitions, incorrect method signatures, and
unsatisfied associated bounds are errors, including in unused implementations.
`type Output: A + B;` requires the selected type to implement both traits.
Unbounded `type Output;` accepts any otherwise valid owned value type. Strings,
collections, tuples, arrays, structs, ADTs, and function values retain their usual
layout and ownership behavior. Associated types cannot contain escaping borrowed
references; method parameters can still borrow an associated value.

`Self` denotes the implementing type in trait signatures and implementation type
annotations. `Self::Output` selects the current trait's member, even when another
trait has a member with the same name. `Self` is reserved as a type name.

Type positions support these projections:

| Form | Meaning |
|---|---|
| `Self::Output` | Current trait's associated type |
| `T::Output` | Associated type selected by the generic parameter's bounds |
| `Concrete::Output` | Associated type from a visible applicable trait |
| `<T as Produce>::Output` | Explicitly selected trait; also works with concrete types |
| `T::Output::Item` | Nested projection, when `Output` has the required bound |

Unqualified projections must identify exactly one applicable, lexically visible
trait. Use the qualified form when multiple traits define the same member.
Import aliases work for both the base type and trait. Existing coherence, orphan,
and module visibility rules apply. Associated projections cannot appear in impl
receiver patterns: those patterns must identify their generic arguments directly.

Generic declarations are checked against their declared contracts. A
`T::Output` is distinct from any concrete type until specialization; a particular
implementation choosing `i16` does not let a generic body assume arithmetic on
all outputs. Declared associated bounds permit their methods in generic code.
Chained projections normalize recursively, with direct and indirect cycles
rejected. Excessively deep resolution is also rejected with a cycle diagnostic.

Projections do not infer their base type backwards. In a call taking both `T`
and `T::Output`, the base is inferred from ordinary arguments or supplied explicit
type arguments; the projected arguments must then match the normalized type.
This preserves ordinary argument evaluation order and borrowing rules.

The current extension excludes associated defaults, generic associated types,
associated equality bounds (`T: Produce<Output = String>`), generic trait
parameters, and supertraits. These require separate extensions; the compiler
reports unsupported forms explicitly.

The language server supports associated symbols, type highlighting, hover,
definitions, and completion after `Self::`, a bounded `T::`, a concrete name, or
`<T as Trait>::`. Ambiguous members are omitted. Associated member rename updates
the contract, implementations, and resolved projections together. Nested projected
receivers and erased generic impl arguments are handled conservatively by the
rename index; unsupported resolution refuses the edit. Completion after a nested
projection is not yet provided.

See [examples/associated-types.rfl](examples/associated-types.rfl) for a manually
polled future with a managed `String` output. This establishes the type contract
needed for a future async design; it does not add async syntax, suspension, or an
executor. Conformance cases live in `tests/associated_types.rs`.
