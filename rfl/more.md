# RFL — Values, Collections, and Resource Management
**Language Specification · Draft 0.2**

**Status:** Proposed additions to RFL 0.1  
**Compiler:** `rfl`  
**Target:** Ripple IR  
**Scope:** Value semantics, references, collections, error handling, resource cleanup, destructuring, constants, and callable types.

This document extends the original RFL specification and the methods/extension-methods proposal.

Where it conflicts with earlier drafts, this document takes precedence.

In particular, mutable references and `?` are now included in the proposed language rather than deferred.

---

## 1. Fundamental principle: everything behaves like a value

RFL uses **value semantics** by default.

Consider:

```rust
let a = [1, 2, 3];
let mut b = a;

b.push(4);
```

After execution:

```text
a = [1, 2, 3]
b = [1, 2, 3, 4]
```

Modifying `b` cannot modify `a`.

This applies to:

- Primitive values
- Structures and tuples
- Algebraic data types
- Dynamic arrays
- Strings
- Closures and their captured environments

A variable contains a value, not an implicitly shared mutable object.

### 1.1 Implementation freedom

The compiler may implement value copying using:

- Direct copying for small values.
- Shared immutable storage.
- Reference counting.
- Copy-on-write.
- Other optimizations that preserve observable value semantics.

The programmer does not need to know which strategy is used.

### 1.2 No implicit shared mutable state

```rust
let mut a = [1, 2, 3];
let mut b = a;

a.push(4);
b.push(5);
```

Results:

```text
a = [1, 2, 3, 4]
b = [1, 2, 3, 5]
```

This guarantee is fundamental to RFL.

Raw pointers and explicitly unsafe operations are excluded from this guarantee.

---

# 2. Copy-on-write collections

RFL 0.2 specifies **copy-on-write (COW)** for managed mutable collections.

This is a runtime implementation strategy supporting the value semantics defined above.

### 2.1 Sharing

```rust
let a = [1, 2, 3];
let b = a;
```

Both values may initially share the same allocation.

Conceptually:

```text
a ────┐
      ▼
   [1, 2, 3]
      ▲
b ────┘
```

The allocation is reference-counted.

### 2.2 Mutation

```rust
let mut b = a;
b.push(4);
```

If the backing allocation is shared, it is copied before mutation.

```text
a ──────► [1, 2, 3]

b ──────► [1, 2, 3, 4]
```

If the allocation is uniquely owned, it may be modified directly.

### 2.3 Requirements

- Mutating one value must not affect another logically independent value.
- Shared storage must be detached before mutation.
- Immutable values can share storage indefinitely.
- Reference counts are invisible to application code.
- The compiler may eliminate unnecessary retains, releases, and copies.

COW is an optimization of value semantics, not a separate user-visible ownership model.

---

# 3. Mutable references

RFL supports temporary references for efficiently accessing and modifying values.

```rust
&T
&mut T
```

These are distinct from raw pointers.

| Type | Meaning |
|---|---|
| `T` | Ordinary value |
| `&T` | Temporary read-only reference |
| `&mut T` | Temporary exclusive mutable reference |
| `*const T` | Raw read-only pointer |
| `*mut T` | Raw mutable pointer |

## 3.1 Read-only references

```rust
fn sum(xs: &Vec<i16>) -> i16 {
    let mut result = 0;

    for (x in xs) {
        result += x;
    }

    result
}
```

Usage:

```rust
let numbers = [1, 2, 3];

let result = sum(&numbers);
```

No copy of the collection is required.

The referenced value cannot be modified through `&T`.

## 3.2 Mutable references

```rust
fn append(xs: &mut Vec<i16>, value: i16) {
    xs.push(value);
}
```

Usage:

```rust
let mut numbers = [1, 2];

append(&mut numbers, 3);
```

Afterward:

```text
numbers = [1, 2, 3]
```

The function modifies the caller's variable.

If the collection's backing storage is shared with another value, COW detaches it before mutation.

## 3.3 Borrowing rules

RFL 0.2 uses a deliberately restricted borrowing model.

**Rule 1:** Multiple read-only references may coexist.

```rust
let a = &numbers;
let b = &numbers;
```

**Rule 2:** A mutable reference requires exclusive access.

```rust
let a = &mut numbers;
let b = &mut numbers; // Error: already mutably borrowed
```

**Rule 3:** A value cannot be mutated while read-only references to it are active.

```rust
let a = &numbers;

numbers.push(42); // Error while a is active
```

**Rule 4:** References cannot escape their owning function or block.

Specifically, borrowed references cannot:

- Be returned from functions.
- Be stored in structures, ADTs, or collections.
- Be captured by closures.
- Be stored in global variables.

**Rule 5:** References are checked at compile time.

There is no runtime borrow checker.

### 3.4 Lexical lifetimes

RFL 0.2 uses lexical borrow scopes.

```rust
let mut numbers = [1, 2, 3];

{
    let borrowed = &mut numbers;
    borrowed.push(4);
}

// Borrow ended.

numbers.push(5);
```

The borrow ends when its lexical scope ends.

The compiler does not need Rust-style lifetime parameters or sophisticated nonlexical lifetime inference.

This is intentionally conservative.

Some programs that are safe in principle may require an additional block to satisfy these rules.

### 3.5 Temporary borrows

Temporary borrows created for individual calls last until that call completes.

```rust
numbers.push(42);
numbers.push(43);
```

Both operations are valid because each temporary mutable borrow ends after its call.

---

# 4. Method receivers

The existing `impl` specification is extended to support three receiver forms.

```rust
impl<T> Vec<T> {
    fn len(&self) -> usize {
        // ...
    }

    fn push(&mut self, value: T) {
        // ...
    }

    fn transformed(self) -> Vec<T> {
        // ...
    }
}
```

### 4.1 Receiver kinds

| Receiver | Semantics |
|---|---|
| `self` | Receive an ordinary value |
| `&self` | Borrow the value read-only |
| `&mut self` | Borrow the value mutably |

### 4.2 Automatic borrowing

Method calls automatically create temporary references when appropriate.

```rust
let mut numbers = [1, 2, 3];

numbers.len();
numbers.push(4);
```

These are equivalent to calls that explicitly pass `&numbers` and `&mut numbers`, respectively.

The compiler determines the receiver mode from the resolved method declaration.

**Automatic borrowing applies to method receivers only.**

Ordinary function parameters use explicit references:

```rust
append(&mut numbers, 42);
```

There is no general implicit reference conversion.

### 4.3 Mutability

Methods requiring `&mut self` cannot be called on immutable values.

```rust
let numbers = [1, 2, 3];

numbers.push(4); // Compile-time error
```

Methods using `&self` can be called on mutable or immutable values.

---

# 5. Dynamic arrays

RFL provides a built-in generic dynamic array type:

```rust
Vec<T>
```

Array literals produce `Vec<T>` by default.

```rust
let numbers = [1, 2, 3];
```

Equivalent type:

```rust
let numbers: Vec<i16> = [1, 2, 3];
```

## 5.1 Empty arrays

```rust
let numbers: Vec<i16> = [];
```

An empty array without sufficient type context is a compile-time inference error.

## 5.2 Basic operations

```rust
let mut numbers = [10, 20, 30];

numbers.push(40);
numbers.pop();

let length = numbers.len();
let first = numbers[0];
```

The standard library provides at least:

```rust
impl<T> Vec<T> {
    fn len(&self) -> usize;
    fn is_empty(&self) -> bool;

    fn push(&mut self, value: T);
    fn pop(&mut self) -> Option<T>;

    fn get(&self, index: usize) -> Option<T>;

    fn clear(&mut self);
}
```

`usize` is the target-defined unsigned index/length type, capable of representing supported collection lengths. It is not necessarily the width of a physical Ripple address.

## 5.3 Indexing

```rust
numbers[0]
```

Returns the element by value.

For heap-backed elements, this may share immutable storage rather than physically duplicate it.

Mutable indexing is supported:

```rust
numbers[0] = 42;
```

The compiler ensures the backing allocation is uniquely owned before modifying it.

### Bounds checking

Indexing is bounds-checked by default.

```rust
numbers[100]
```

If the index is out of bounds, execution terminates with a runtime bounds error.

For recoverable access:

```rust
let value = numbers.get(100);
```

This returns `None`.

Unchecked indexing is deferred to explicitly unsafe library operations.

There is no user-defined indexing or operator overloading in RFL 0.2.

---

# 6. Slices

RFL provides immutable slices.

```rust
Slice<T>
```

A slice represents a contiguous range of elements.

```rust
let numbers = [10, 20, 30, 40, 50];

let middle = numbers[1..4];
```

Result:

```text
middle = [20, 30, 40]
```

### 6.1 Slice ownership

**RFL slices are owning immutable views**, not borrowed references.

A slice retains access to its backing storage through managed sharing.

It may outlive the collection from which it was created.

This avoids exposing lifetime parameters.

```rust
fn middle() -> Slice<i16> {
    let numbers = [1, 2, 3, 4];

    numbers[1..3]
}
```

This is valid.

The slice keeps the underlying allocation alive.

### 6.2 Interaction with COW

```rust
let mut numbers = [1, 2, 3, 4];

let slice = numbers[1..3];

numbers[1] = 99;
```

Afterward:

```text
numbers = [1, 99, 3, 4]
slice   = [2, 3]
```

The mutation detaches the vector's storage if necessary.

Existing slices continue observing the original values.

### 6.3 Restrictions

Slices are read-only.

Mutable slices are deferred.

Slices support `len()`, `get()`, indexing, and iteration.

---

# 7. Fixed-size arrays

RFL also supports fixed-size arrays:

```rust
[T; N]
```

Example:

```rust
let pixels: [u8; 16] = [
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0
];
```

The length is part of the type.

Fixed-size arrays have value semantics and may use inline storage.

They do not grow or shrink.

Conversion to a dynamic array is explicit:

```rust
let dynamic = pixels.to_vec();
```

Type annotations determine whether an array literal creates a fixed-size array or a vector.

---

# 8. Ranges and iteration

RFL supports integer ranges.

```rust
0..10
```

Includes `0` through `9`.

```rust
0..=10
```

Includes `0` through `10`.

## 8.1 For loops

```rust
for (i in 0..10) {
    println(i);
}
```

Iteration is supported for:

- Integer ranges
- `Vec<T>`
- `Slice<T>`
- Fixed-size arrays
- Standard library `List<T>`

Iteration over a collection follows value semantics. The iterable expression is evaluated once and a logical snapshot is traversed.

Mutating another variable holding a copy of the collection does not change the elements already selected for iteration.

User-defined iteration protocols and traits are deferred.

---

# 9. Error propagation with `?`

RFL supports `?` for propagating `Result` and `Option` failures.

The operator is postfix.

```rust
expression?
```

## 9.1 Result propagation

Given:

```rust
type Result<T, E> =
    | Ok(T)
    | Err(E);
```

The expression:

```rust
let value = operation()?;
```

Is equivalent to:

```rust
let value = match operation() {
    Ok(value) => value,
    Err(error) => return Err(error),
};
```

### Requirements

- The operand must have type `Result<T, E>`.
- The enclosing function must return `Result<U, E>`.
- The error types must match.
- No implicit error conversion occurs.

Because RFL has no traits, there is no generalized `From`-style conversion mechanism.

Explicit error conversion remains possible using ordinary functions and pattern matching.

## 9.2 Option propagation

Given:

```rust
type Option<T> =
    | None
    | Some(T);
```

The expression:

```rust
let value = operation()?;
```

Propagates `None` from a function returning `Option<U>`.

Otherwise, it produces the value inside `Some`.

### 9.3 Scope cleanup

Early returns caused by `?` execute registered `defer` actions.

This is essential for resource safety.

`?` works only with the canonical `Option` and `Result` types provided by the standard prelude, not arbitrary user-defined ADTs with matching names.

---

# 10. Scope cleanup with `defer`

RFL provides a `defer` statement.

```rust
defer expression;
```

Or:

```rust
defer {
    statements
}
```

A deferred action executes when its enclosing lexical scope exits.

## 10.1 Example

```rust
fn process_file(path: String) -> Result<i16, IOError> {
    let file = File.open(path)?;

    defer {
        file.close();
    }

    let contents = file.read_all()?;

    process(contents)
}
```

This example assumes the corresponding file API exists.

`file.close()` executes when the function's scope exits, including when `read_all()` fails.

## 10.2 Execution order

Deferred actions execute in reverse registration order.

```rust
fn example() {
    defer println("A");
    defer println("B");
    defer println("C");
}
```

Output:

```text
C
B
A
```

## 10.3 Deferred evaluation

**Deferred expressions are evaluated when the scope exits, not when the `defer` statement is encountered.**

```rust
let mut x = 1;

defer println(x);

x = 2;
```

Prints:

```text
2
```

The deferred expression refers to the original lexical binding, observing its value when the deferred action executes.

### 10.4 Exit conditions

Deferred actions execute on:

- Normal scope completion.
- Explicit `return`.
- Early return through `?`.
- `break`.
- `continue`.

For an early return, the return value is evaluated first, then deferred actions run, then control returns to the caller.

Deferred actions run before their enclosing scope's local values are released.

### 10.5 Restrictions

Deferred actions:

- Cannot themselves contain `return`, `break`, `continue`, or `?`.
- Cannot refer to local bindings declared after the `defer` statement.
- Cannot transfer borrowed references outside their permitted scopes.
- Execute only if their `defer` statement was reached.

Runtime aborts and unrecoverable traps are not required to execute deferred actions.

RFL 0.2 has no exception unwinding.

---

# 11. Destructuring

RFL permits patterns in variable declarations.

## 11.1 Tuples

```rust
let (x, y) = get_coordinates();
```

## 11.2 Structures

```rust
let Point { x, y } = point;
```

## 11.3 Nested patterns

```rust
let ((a, b), c) = value;
```

### 11.4 Irrefutability

Patterns in ordinary `let` declarations must be irrefutable.

This is valid:

```rust
let (x, y) = (10, 20);
```

This is invalid:

```rust
let Some(x) = get_value();
```

Because the expression could evaluate to `None`.

Refutable patterns must use `match`.

## 11.5 Function parameters

Irrefutable patterns are also permitted in function parameter declarations.

```rust
fn length_squared(Point { x, y }: Point) -> i16 {
    x * x + y * y
}
```

No additional runtime representation is necessary.

Destructuring lowers into ordinary value extraction operations.

---

# 12. Pattern guards

`match` arms may contain conditional guards.

```rust
match value {
    Some(x) if (x > 100) => "large",
    Some(x) if (x > 0) => "positive",
    Some(_) => "other",
    None => "missing",
}
```

### Rules

- The guard must evaluate to `bool`.
- The guard is evaluated only after its pattern matches.
- If the guard is false, matching continues with the next arm.
- Guards are evaluated in source order.
- Pattern bindings are available inside their guard.

### Exhaustiveness

Guarded arms do not count toward exhaustiveness unless an unguarded pattern independently covers the case.

This ensures that the compiler does not incorrectly assume an arbitrary guard will evaluate to true.

---

# 13. Compile-time constants

RFL supports named compile-time constants.

```rust
const WIDTH: usize = 320;
const HEIGHT: usize = 200;

const BUFFER_SIZE: usize = WIDTH * HEIGHT;
```

Constants are immutable and evaluated during compilation.

## 13.1 Supported constant expressions

RFL 0.2 supports:

- Numeric literals
- Boolean literals
- Character literals
- Arithmetic operations
- Comparison operations
- Boolean operations
- References to previously defined constants
- Parenthesized expressions

Constant expressions must be statically evaluable.

## 13.2 Array dimensions

```rust
const SIZE: usize = 128;

let buffer: [u8; SIZE];
```

Array lengths must be nonnegative compile-time constants representable by `usize`.

Definite assignment is required before reading an uninitialized local array.

## 13.3 No general compile-time execution

RFL 0.2 does not support:

- Compile-time function execution
- Compile-time allocation
- Compile-time loops
- Macros
- Reflection
- Arbitrary code generation

Constants are deliberately simple.

---

# 14. Callable types

RFL distinguishes between **native callable values** and **C-compatible function pointers**.

This resolves an important issue with higher-order functions.

## 14.1 Native callable types

The type:

```rust
fn(i16) -> i16
```

Represents any native RFL callable with that signature.

It may refer to:

- A named function.
- A noncapturing anonymous function.
- A capturing closure.

Example:

```rust
fn apply(f: fn(i16) -> i16, value: i16) -> i16 {
    f(value)
}
```

All of the following are valid:

```rust
fn double(x: i16) -> i16 {
    x * 2
}

apply(double, 21);

apply(fn(x) => x * 2, 21);

let factor = 2;
apply(fn(x) => x * factor, 21);
```

### 14.2 Closure representation

Conceptually, native callable values contain:

```text
Callable {
    code: CodeAddress
    environment: Environment
}
```

The compiler may optimize noncapturing functions into more compact representations.

Captured environments are managed values.

Their lifetimes are controlled by RFL's automatic memory management.

## 14.3 Capture semantics

Closures capture values at the time of closure creation.

```rust
let mut factor = 2;

let multiply = fn(x) => x * factor;

factor = 3;

let result = multiply(21);
```

`result` is `42`.

The closure captured the value `2`.

It does not observe subsequent reassignment of `factor`.

Captures are immutable inside the closure.

RFL 0.2 does not support writable capture of an outer local variable.

Closures may freely escape their defining scope because their environments own their captured values.

## 14.4 C function pointers

C-compatible function pointers use a distinct type:

```rust
extern "C" fn(c_int, c_int) -> c_int
```

Such a value contains a callable code reference using Ripple's C ABI.

It has no captured RFL environment.

Noncapturing RFL functions explicitly declared with the C ABI may be used as C callbacks.

Ordinary capturing closures cannot be converted into C function pointers.

No automatic callback trampoline generation is required in RFL 0.2.

---

# 15. Memory management

RFL 0.2 uses automatic reference counting for managed allocations.

Managed allocations include:

- Recursive ADT nodes.
- Dynamic-array backing storage.
- String storage.
- Slice backing storage.
- Captured closure environments.

### 15.1 Automatic lifetime management

```rust
fn create() -> Vec<i16> {
    let numbers = [1, 2, 3];

    numbers
}
```

The returned vector remains valid after the function exits.

The compiler transfers or retains the managed allocation as necessary.

### 15.2 Release

When a managed allocation is no longer referenced, it is released automatically.

The compiler inserts the necessary memory-management operations.

### 15.3 Cycles

Reference cycles are not collected.

The safe subset of RFL is designed around value semantics and immutable shared storage, preventing ordinary programs from creating mutable reference cycles.

Arbitrary raw-pointer operations are outside these guarantees.

### 15.4 Allocation failure

Implicit allocation failure is a runtime error in RFL 0.2.

The runtime may terminate execution when memory cannot be allocated.

Recoverable allocation APIs may be introduced later.

### 15.5 Resource management

Reference counting handles memory.

`defer` handles explicit nonmemory cleanup.

Resources such as files and sockets are not assumed to close merely because a managed value becomes unreachable.

This distinction is deliberate.

---

# 16. Interaction with methods

Everything described here integrates with the existing method system.

For example:

```rust
impl<T> Option<T> {
    export fn unwrap_or(self, fallback: T) -> T {
        match self {
            Some(value) => value,
            None => fallback,
        }
    }

    export fn map<U>(
        self,
        f: fn(T) -> U
    ) -> Option<U> {
        match self {
            Some(value) => Some(f(value)),
            None => None,
        }
    }
}
```

Then:

```rust
let result = Some(21)
    .map(fn(x) => x * 2)
    .unwrap_or(0);
```

Similarly, mutable collections:

```rust
let mut numbers = [1, 2, 3];

numbers.push(4);

let result = numbers
    .map(fn(x) => x * 2)
    .filter(fn(x) => x > 4);
```

Here `map` and `filter` may be ordinary extension functions.

They accept the collection as their first argument.

No traits are required.

No virtual tables are required.

And no implicit mutation of the original collection occurs unless a function explicitly receives a mutable reference.

---

# 17. Complete example

The following demonstrates value semantics, dynamic arrays, mutable methods, closures, `Result`, and error propagation.

```rust
import { println } from "std/io";

fn sum_positive(numbers: Vec<i16>) -> Result<i16, String> {
    let mut total = 0;

    for (x in numbers) {
        if (x < 0) {
            return Err("Negative number");
        }

        total += x;
    }

    Ok(total)
}

fn main() -> Result<i16, String> {
    let original = [10, 20];

    let mut numbers = original;

    numbers.push(12);

    let total = sum_positive(numbers)?;

    println(total);

    Ok(total)
}
```

Expected output:

```text
42
```

`original` remains unchanged.

`numbers` may initially share its allocation with `original`, but mutation detaches it.

`sum_positive` receives a logical value, not a mutable alias into the caller's variable.

The `?` operator propagates failures without nested pattern matching.

And the entire program uses **zero explicit pointers**.

---

# 18. Compiler lowering

Most new features require no changes to the Ripple ISA.

| Feature | Implementation |
|---|---|
| Value copying | Direct copy or managed retain |
| COW mutation | Uniqueness check and conditional copy |
| `&T` | Temporary address with static borrow checks |
| `&mut T` | Temporary exclusive address |
| Dynamic arrays | Runtime-managed buffer |
| Fixed arrays | Fixed-size aggregate |
| Slices | Managed buffer, offset, length |
| Indexing | Bounds check and GEP |
| `?` | Conditional branch and early return |
| `defer` | Scope-exit cleanup blocks |
| Destructuring | Field extraction |
| Pattern guards | Conditional branches |
| Constants | Compile-time evaluation |
| Closures | Function reference and environment |
| Callable invocation | Direct or indirect IR call |

The existing Ripple IR already supports many of these operations.

Some facilities may require IR extensions or runtime helpers, particularly indirect calls, managed allocation, and reference-count operations.

Those extensions belong below the RFL frontend and do not require new source-language syntax.

### 18.1 Runtime library

RFL should have a small runtime providing operations conceptually equivalent to:

```text
rfl_alloc
rfl_retain
rfl_release

rfl_vec_new
rfl_vec_reserve
rfl_vec_ensure_unique
rfl_vec_push
rfl_vec_pop

rfl_bounds_fail
rfl_alloc_fail
```

The exact function names and signatures are implementation details.

These functions can initially be implemented in C and compiled using the existing `rcc` toolchain.

That would allow RFL to gain automatic memory management and collections without first requiring the language to be capable of implementing its own runtime.

---

# 19. Explicitly excluded features

The following are outside the RFL 0.2 language:

| Feature | Decision |
|---|---|
| Traits | Deferred |
| Trait objects | Deferred |
| Classes and inheritance | Excluded |
| Operator overloading | Excluded |
| Macros | Excluded |
| Async/await | Excluded |
| Threads | Deferred |
| General garbage collection | Deferred |
| Rust-style lifetime parameters | Excluded |
| Full borrow checking | Excluded |
| Mutable closure captures | Deferred |
| Mutable slices | Deferred |
| User-defined iterators | Deferred |
| Exceptions | Excluded |
| Implicit error conversion | Excluded |
| Arbitrary compile-time execution | Deferred |
| Incremental compilation | Deferred |

RFL 0.2 remains intentionally small.

---

## 20. Implementation priorities

I would implement these features in the following order.

| Priority | Feature | Reason |
|---|---|---|
| 1 | `Vec<T>` and literals | Immediately useful |
| 2 | Value semantics and COW | Establishes predictable mutation |
| 3 | `&T`, `&mut T` | Enables efficient APIs |
| 4 | Mutable method receivers | Makes collections ergonomic |
| 5 | Native callable types | Makes closures work with higher-order functions |
| 6 | `?` | Small implementation, major ergonomic benefit |
| 7 | `defer` | Necessary for reliable resource cleanup |
| 8 | Destructuring and guards | Extends existing pattern matching |
| 9 | Constants | Useful for systems programming |
| 10 | Slices and fixed arrays | Completes the collection model |

The dependency that matters most is **settling the value and borrowing semantics before building too much mutable standard-library code**.

---

## 21. The resulting RFL programming model

At this point, the language has a remarkably coherent set of rules.

**Values behave like values.** Assigning one variable to another doesn't create surprising shared mutation.

**Mutation is explicit.** `let mut` and `&mut` tell the compiler exactly where changes are permitted.

**References are temporary.** You get efficient access without a general lifetime calculus.

**Collections are automatic.** Arrays, allocation, resizing, sharing, and cleanup are managed for you.

**Functions are ordinary functions.** Methods are convenient syntax over statically resolved functions.

**Closures are ordinary values.** They can capture their environment and be passed around freely.

**Errors are values.** `Result`, `Option`, and `?` provide predictable failure handling.

**Resources are explicit.** `defer` handles cleanup without exceptions or implicit finalization.

**Types are inferred.** Annotations are required where they matter, not everywhere.

**Compilation is automatic.** Imports determine the build graph. No Makefiles.