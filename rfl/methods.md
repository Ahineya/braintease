# RFL Specification: Methods and Extensions
**Version 0.1 — Proposal**

## 1. Inherent methods

Methods are declared in an `impl` block associated with a type.

```rust
export type Option<T> =
    | None
    | Some(T);

impl<T> Option<T> {
    export fn unwrap_or(self, fallback: T) -> T {
        match self {
            None => fallback,
            Some(x) => x,
        }
    }

    export fn is_some(self) -> bool {
        match self {
            None => false,
            Some(_) => true,
        }
    }
}
```

Usage:

```rust
fn main() -> i16 {
    let value = Some(42);

    value.unwrap_or(0)
}
```

### Rules

- `impl<T> Type<T>` associates methods with a type.
- `self` represents the receiver.
- The receiver is the first parameter of the underlying function.
- Method calls use `value.method(args)`.
- Methods are statically dispatched.
- Methods may be generic.
- Methods are private unless marked `export`.
- An `impl` block may only be declared in the module defining the type.
- Multiple `impl` blocks for the same type are permitted.

**No new runtime mechanism is required.**

The compiler translates:

```rust
value.unwrap_or(0)
```

Into the equivalent of:

```rust
Option::unwrap_or(value, 0)
```

Which is an ordinary function call in Ripple IR.

## 2. Associated functions

Functions without `self` are associated functions rather than instance methods.

```rust
struct Point {
    x: i16,
    y: i16,
}

impl Point {
    export fn origin() -> Point {
        Point { x: 0, y: 0 }
    }

    export fn offset(self, dx: i16, dy: i16) -> Point {
        Point {
            x: self.x + dx,
            y: self.y + dy,
        }
    }
}
```

Usage:

```rust
let p = Point::origin();
let q = p.offset(10, 20);
```

Associated functions use `Type::function(args)`.

They cannot be invoked through instance-method syntax unless they declare a receiver.

## 3. Extension methods

**Any visible free function can be called using method syntax if its first parameter is compatible with the receiver.**

No special declaration syntax is required.

For example:

```rust
export fn squared(x: i16) -> i16 {
    x * x
}
```

Both forms are equivalent:

```rust
let a = squared(10);
let b = 10.squared();
```

The compiler rewrites:

```rust
receiver.function(a, b)
```

As:

```rust
function(receiver, a, b)
```

The ordinary function signature determines whether the call is valid.

### Example: collection operations

```rust
export fn map<T, U>(
    xs: List<T>,
    f: fn(T) -> U
) -> List<U> {
    match xs {
        Nil => Nil,
        Cons(x, rest) => Cons(f(x), map(rest, f)),
    }
}
```

Both forms work:

```rust
let a = map(numbers, fn(x) => x * 2);

let b = numbers.map(fn(x) => x * 2);
```

This means **collection-first argument ordering is encouraged** for functions intended to support method syntax.

There is no implicit currying or argument reordering.

## 4. Extension methods and modules

Extension methods participate in the existing module system.

**`collections.rfl`**

```rust
export fn sum(xs: List<i16>) -> i16 {
    match xs {
        Nil => 0,
        Cons(x, rest) => x + sum(rest),
    }
}
```

**`main.rfl`**

```rust
import { sum } from "./collections";

fn main() -> i16 {
    let numbers = Cons(10, Cons(20, Cons(12, Nil)));

    numbers.sum()
}
```

An extension function is available through method syntax **only if the function is visible in the current scope**.

This means importing a function makes its extension syntax available automatically.

No global method registration or special extension imports exist.

Import aliases also work:

```rust
import { sum as total } from "./collections";

let result = numbers.total();
```

Inherent methods, by contrast, are associated with their defining type. Importing that type makes its exported inherent methods available.

## 5. Method resolution

Given:

```rust
value.foo(a, b)
```

The compiler performs the following resolution:

**Step 1 — Determine the receiver type.**

Infer or resolve the static type of `value`.

**Step 2 — Search inherent methods.**

Look for a method named `foo` in the receiver type's `impl` blocks.

If found, validate its arguments and generic constraints.

**Step 3 — Search extension functions.**

If no applicable inherent method exists, search visible free functions named `foo`.

An extension candidate is applicable when its first parameter accepts the receiver and its remaining parameters accept the supplied arguments.

**Step 4 — Report errors.**

If no candidate is applicable, report an unknown or incompatible method.

If resolution is ambiguous, report a compile-time error.

### Precedence

**Inherent methods always take priority over extension methods.**

For example:

```rust
impl Point {
    fn length(self) -> i16 {
        42
    }
}

fn length(p: Point) -> i16 {
    100
}
```

Then:

```rust
p.length()  // Calls Point::length
length(p)   // Calls the free function
```

An inherent method with the matching name but incompatible arguments does not silently redirect to an extension function. It produces a type error.

This keeps method resolution predictable.

## 6. Receiver semantics

RFL 0.1 initially supports **by-value receivers**:

```rust
fn method(self, arg: T) -> U
```

The receiver follows ordinary RFL value semantics.

For immutable managed values, this means sharing the value according to the language's memory-management rules.

For ordinary structures, it means passing the value.

The initial method system does not introduce special ownership rules.

### Future receiver forms

The syntax reserves room for:

```rust
fn inspect(&self) -> i16 {
    // Read-only receiver.
}

fn push(&mut self, value: T) {
    // Mutable receiver.
}
```

Borrowed receivers are deferred until RFL defines the necessary reference and lifetime semantics. They must not implicitly introduce Rust-style borrow checking as a side effect of adding methods.

For now, methods that transform values can return updated values.

## 7. Generic methods

Both inherent and extension methods support generics.

```rust
impl<T> Option<T> {
    export fn map<U>(
        self,
        f: fn(T) -> U
    ) -> Option<U> {
        match self {
            None => None,
            Some(x) => Some(f(x)),
        }
    }
}
```

Usage:

```rust
let result = Some(21)
    .map(fn(x) => x * 2)
    .unwrap_or(0);
```

Generic parameters are inferred whenever possible.

As with ordinary generic functions, method specializations are monomorphized.

## 8. Compilation

Methods are ordinary functions after name resolution.

For example:

```rust
impl<T> Option<T> {
    fn unwrap_or(self, fallback: T) -> T {
        // ...
    }
}
```

Conceptually becomes a uniquely named function:

```text
Option::unwrap_or<T>(Option<T>, T) -> T
```

Compilation involves:

1. Resolving the receiver type and method.
2. Inferring generic arguments.
3. Generating the required function specialization.
4. Emitting an ordinary Ripple IR call.

**No special IR instructions, runtime dispatch, or object metadata are necessary.**

The method system is entirely a frontend feature.

## 9. Explicitly excluded

RFL 0.1 does not include:

- Traits or interfaces.
- Inheritance.
- Virtual methods.
- Dynamic dispatch.
- Method overloading by parameter types.
- Implicit receiver conversions.
- Operator overloading through methods.
- Borrowed receivers (`&self`, `&mut self`).
- Runtime method lookup.

---

## 10. The intended programming style

The important result is that RFL supports natural method chaining without forcing APIs into an object-oriented design.

```rust
import { List } from "std/list";
import { println } from "std/io";

fn main() {
    let numbers = List.from([1, 2, 3, 4, 5]);

    let result = numbers
        .map(fn(x) => x * 2)
        .filter(fn(x) => x > 5)
        .fold(0, fn(acc, x) => acc + x);

    println(result);
}
```

This example illustrates the intended API; it assumes the corresponding collection and I/O functions exist.

Crucially, `map`, `filter`, and `fold` could all be **ordinary exported functions**.

They don't need to live inside `List`.

They don't need to implement a trait.

They don't need a class hierarchy.

They're just functions that happen to accept a collection as their first argument.

And when you actually need methods tied to a particular type, `impl` provides that mechanism.

---

### One design choice I'd strongly preserve

**Don't automatically make every extension function a real method of the receiver's type.**

Keep extension resolution lexical.

That means:

```rust
import { map } from "./collections";
```

Makes `.map()` available in that module.

Without the import, it isn't available.

This prevents distant libraries from silently changing which methods a type supports, and preserves the simplicity of your TypeScript-inspired module system.