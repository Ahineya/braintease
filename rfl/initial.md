# RFL — Ripple Functional Language
**Language Specification · Draft 0.1**

*A pragmatic, statically typed, expression-oriented programming language for the Ripple computing architecture.*

**Status:** Proposed specification  
**Compiler:** `rfl`  
**Source extension:** `.rfl`  
**Compilation target:** Ripple IR  
**Execution target:** Ripple ISA  
**Lowering:** [lowering.md](lowering.md) locks how RFL hides machine limits (one hardware stack bank, one global bank, 16-bit cells) from RFL code.  
**Interoperability:** Ripple C99 ABI  
**Philosophy:** Functional by default. Imperative when useful. No unnecessary ceremony.

---

## 1. Language philosophy

RFL combines the expressive capabilities of ML-family languages with familiar syntax and practical systems-programming facilities.

Its primary goals are:

1. **Expressiveness.** Algebraic data types, pattern matching, generics, first-class functions, closures, and type inference.
2. **Simplicity.** A small, coherent language without unnecessary syntactic or semantic complexity.
3. **Predictability.** Explicit mutation, well-defined evaluation order, and deterministic resource management.
4. **Interoperability.** Direct compatibility with programs compiled using Ripple's existing C99 compiler.
5. **Modularity.** File-based modules, explicit imports and exports, and automatic dependency compilation.
6. **Portability.** RFL targets Ripple IR, remaining independent of the machine implementation.
7. **Pragmatism.** Imperative constructs are available where they provide simpler solutions than functional equivalents.

RFL is not intended to enforce purely functional programming.

It provides functional abstractions while allowing ordinary mutation, loops, and procedural programming.

### 1.1 Syntax principles

RFL uses:

- Braces for blocks.
- Parentheses for function arguments and calls.
- Semicolons for terminating statements.
- Conventional infix operators.
- Explicit function declarations.
- No significant indentation.
- No implicit currying.
- No implicit partial application.

For example:

```rust
fn add(a: i16, b: i16) -> i16 {
    a + b
}

fn main() {
    let x = add(10, 20);
}
```

Indentation and whitespace have no semantic significance except for separating tokens.

---

# 2. Types

RFL uses static typing with local and function-level type inference.

## 2.1 Primitive types

| Type | Description |
|---|---|
| `bool` | Boolean |
| `i8`, `u8` | 8-bit integers |
| `i16`, `u16` | 16-bit integers |
| `i32`, `u32` | 32-bit integers |
| `i64`, `u64` | 64-bit integers |
| `f32` | 32-bit floating point |
| `f64` | 64-bit floating point |
| `char` | Unicode scalar value |
| `String` | Immutable UTF-8 string |
| `unit` | Unit type |

Integer widths are guaranteed regardless of the underlying Ripple implementation.

The default integer type is `i16`, reflecting Ripple's native word size.

Arithmetic uses the corresponding Ripple integer operations. Integer arithmetic wraps at its declared width; division by zero is a runtime error.

Conversions between different numerical types are explicit:

```rust
let a: i16 = 42;
let b: i32 = a as i32;
```

There are no implicit conversions between `bool` and integers.

## 2.2 Type inference

Type annotations may be omitted whenever the compiler can infer the type unambiguously.

```rust
let a = 10;
let b = 20;
let c = a + b;
```

Similarly:

```rust
fn add(a, b) {
    a + b
}
```

The compiler infers compatible types from the function body and its usage.

Numeric literals are initially type-constrained literal values. Unconstrained integer literals default to `i16`.

An unresolved or ambiguous type is a compile-time error.

### Polymorphic inference

RFL supports rank-1 parametric polymorphism.

For example:

```rust
fn identity(x) {
    x
}
```

The compiler infers a polymorphic signature equivalent to:

```rust
fn identity<T>(x: T) -> T
```

Recursive definitions are inferred using a monomorphic recursive binding, which can subsequently be generalized.

Polymorphic recursion requires an explicit type signature.

Mutable bindings are not generalized. This prevents unsound polymorphic use of mutable storage.

---

# 3. Variables and mutability

Variables are immutable by default.

```rust
let x = 42;
```

Mutation requires `mut`:

```rust
let mut counter = 0;

counter += 1;
counter = counter * 2;
```

Reassignment to an immutable binding is a compile-time error.

```rust
let x = 10;

x = 20; // Error
```

Shadowing is permitted:

```rust
let x = 10;
let x = x + 5;
```

The second declaration creates a new binding.

### 3.1 Value semantics

Primitive values and fixed-layout structures have value semantics.

Heap-backed immutable values, including recursive algebraic data types, use shared immutable storage.

Copying such values does not duplicate their entire contents.

---

# 4. Functions

Functions use conventional declaration and invocation syntax.

```rust
fn multiply(a: i16, b: i16) -> i16 {
    a * b
}
```

Function arguments are evaluated left to right.

Arguments are passed by value according to their types and ABI representation.

Functions have fixed arity.

```rust
multiply(2, 3); // Valid
multiply(2);    // Error
```

There is no implicit currying.

## 4.1 Implicit returns

Blocks are expressions.

The value of the final expression is returned when that expression is not terminated with a semicolon.

```rust
fn square(x: i16) -> i16 {
    x * x
}
```

Equivalent to:

```rust
fn square(x: i16) -> i16 {
    return x * x;
}
```

A trailing semicolon discards the expression's value.

```rust
fn print_square(x: i16) {
    print(x * x);
}
```

Here the function returns `unit`.

Explicit `return` is supported for early exits.

## 4.2 Anonymous functions

Anonymous functions use `fn` without a name:

```rust
let double = fn(x: i16) {
    x * 2
};
```

A shorthand expression body is also supported:

```rust
let double = fn(x) => x * 2;
```

Both forms create first-class function values.

## 4.3 Closures

Anonymous functions may capture variables from their lexical environment.

```rust
fn make_adder(n: i16) {
    fn(x) => x + n
}

fn main() {
    let add10 = make_adder(10);

    let result = add10(32);
}
```

Result: `42`.

Closures capture immutable values by value.

Mutable outer bindings are not captured by writable reference in RFL 0.1. Explicit mutable state must instead be passed through a supported mutable-storage interface.

A closure conceptually contains a code reference and an environment.

The representation is managed by the compiler and runtime.

---

# 5. Algebraic data types

RFL supports algebraic data types with named constructors.

```rust
type Option<T> =
    | None
    | Some(T);
```

And:

```rust
type Result<T, E> =
    | Ok(T)
    | Err(E);
```

Constructors are ordinary callable values.

```rust
let a = Some(42);
let b = None;
```

Type arguments are inferred when possible.

## 5.1 Recursive types

Recursive algebraic data types are supported directly.

```rust
type List<T> =
    | Nil
    | Cons(T, List<T>);
```

**Recursive ADTs use automatic indirection.**

The programmer does not need to explicitly insert `Box`, pointers, or allocation operations.

For example:

```rust
let numbers =
    Cons(1,
        Cons(2,
            Cons(3, Nil)));
```

The compiler manages the underlying representation.

For RFL 0.1, nontrivial ADT values use immutable heap-backed storage. Nullary constructors may use compact constant representations.

Recursive fields therefore do not require infinite inline storage.

This design deliberately prioritizes convenient functional programming over exposing memory representation details.

## 5.2 Pattern matching

```rust
fn unwrap_or<T>(value: Option<T>, fallback: T) -> T {
    match value {
        Some(x) => x,
        None => fallback,
    }
}
```

Patterns may contain nested constructors:

```rust
fn first_two(xs) {
    match xs {
        Cons(a, Cons(b, rest)) => Some((a, b)),
        _ => None,
    }
}
```

Supported pattern forms include constructors, literals, tuples, bindings, and `_`.

Patterns are evaluated in source order.

The compiler performs exhaustiveness checking for finite algebraic data types.

Unreachable patterns produce diagnostics.

Non-exhaustive matches are compile-time errors.

### 5.3 Match expressions

`match` returns a value.

```rust
fn describe(x: Option<i16>) -> String {
    match x {
        None => "Nothing",
        Some(0) => "Zero",
        Some(_) => "Something",
    }
}
```

All reachable branches must produce compatible types.

---

# 6. Structures and tuples

## 6.1 Structures

```rust
struct Point {
    x: i16,
    y: i16,
}
```

Construction:

```rust
let p = Point {
    x: 10,
    y: 20,
};
```

Field access:

```rust
let x = p.x;
```

Structures have value semantics.

Mutable structures require a mutable binding:

```rust
let mut p = Point { x: 10, y: 20 };

p.x = 30;
```

## 6.2 Tuples

```rust
let point = (10, 20);

let (x, y) = point;
```

Tuple elements may have different types.

```rust
let value = (42, true, "hello");
```

Tuples support destructuring and pattern matching.

---

# 7. Functional programming

RFL provides higher-order functions without requiring functional-style invocation syntax.

## 7.1 Map

```rust
type List<T> =
    | Nil
    | Cons(T, List<T>);

fn map(f, xs) {
    match xs {
        Nil => Nil,
        Cons(x, rest) => Cons(f(x), map(f, rest)),
    }
}
```

The compiler infers a signature equivalent to:

```rust
fn map<T, U>(
    f: fn(T) -> U,
    xs: List<T>
) -> List<U>
```

Usage:

```rust
let numbers = Cons(1, Cons(2, Cons(3, Nil)));

let doubled = map(fn(x) => x * 2, numbers);
```

There are no implicit curried applications.

## 7.2 Filter

```rust
fn filter(predicate, xs) {
    match xs {
        Nil => Nil,

        Cons(x, rest) => {
            let tail = filter(predicate, rest);

            if (predicate(x)) {
                Cons(x, tail)
            } else {
                tail
            }
        },
    }
}
```

## 7.3 Pipelines

RFL 0.1 does not require a pipeline operator.

Function composition uses ordinary calls.

```rust
let result = filter(is_even, map(double, numbers));
```

A pipeline operator may be considered later, but it is not part of the initial language.

---

# 8. Control flow

RFL is expression-oriented while retaining ordinary imperative control flow.

## 8.1 If expressions

```rust
fn abs(x: i16) -> i16 {
    if (x < 0) {
        -x
    } else {
        x
    }
}
```

Parentheses around the condition are required.

The condition must have type `bool`.

When used as an expression, both branches must produce compatible values.

An `if` without `else` has type `unit`.

## 8.2 While loops

```rust
fn factorial(n: i16) -> i16 {
    let mut result = 1;
    let mut i = 2;

    while (i <= n) {
        result *= i;
        i += 1;
    }

    result
}
```

`while` evaluates to `unit`.

## 8.3 For loops

```rust
for (x in numbers) {
    print(x);
}
```

Iteration uses a standard library iteration protocol.

For RFL 0.1, the protocol is defined for built-in arrays and standard library collections. User-defined iterator protocols are deferred.

## 8.4 Break and continue

```rust
while (true) {
    if (done()) {
        break;
    }

    continue;
}
```

Both are supported in loops.

---

# 9. Memory management

**RFL 0.1 uses automatic reference counting for managed heap objects.**

This is an initial design decision, not a requirement imposed by Ripple IR.

Managed objects include recursive ADTs, closures with heap-allocated environments, and immutable strings.

The compiler inserts retain/release operations as needed.

```rust
let a = Cons(1, Nil);
let b = a;
```

Both bindings refer to the same immutable list.

When no references remain, the memory is reclaimed.

### 9.1 Cycles

Reference cycles are not collected in version 0.1.

The safe managed-value subset is immutable, so ordinary lists and trees cannot form mutable cycles.

Future mutable heap references may require either cycle detection or a tracing collector.

### 9.2 Runtime

Managed allocation is provided by a small RFL runtime library.

The runtime may use the existing Ripple allocator.

RFL does not require a garbage collector for its initial implementation.

### 9.3 Raw memory

Raw pointers exist primarily for C interoperability and low-level systems programming.

They are not interchangeable with managed references.

Dereferencing raw pointers, unchecked casts, and arbitrary pointer arithmetic require an explicit `unsafe` context.

The first implementation does not require a full Rust-style ownership or borrow checker.

---

# 10. C interoperability

**RFL uses Ripple's existing C ABI.**

`extern "C"` refers to the ABI implemented by `rcc`, not the native ABI of the machine hosting RippleVM.

## 10.1 Importing C functions

**math.c**

```c
int sum(int a, int b) {
    return a + b;
}
```

**main.rfl**

```rust
extern "C" {
    fn sum(a: c_int, b: c_int) -> c_int;
}

fn main() -> c_int {
    sum(20, 22)
}
```

The declaration specifies the function signature and linker symbol.

Calls use Ripple's C calling convention.

## 10.2 C type aliases

RFL provides target-specific C aliases:

```rust
c_int
c_uint
c_char
c_short
c_long
c_size_t
```

These correspond to the actual layouts and calling conventions implemented by Ripple's C99 compiler.

They are not assumed to match the host platform's C types.

## 10.3 Exporting functions to C

```rust
export extern "C" fn multiply(a: c_int, b: c_int) -> c_int {
    a * b
}
```

This generates a C-compatible exported symbol.

The function can be called from C:

```c
extern int multiply(int a, int b);
```

Exported C functions cannot use RFL-specific managed types directly.

## 10.4 C-compatible structures

```rust
#[repr(C)]
struct Point {
    x: c_int,
    y: c_int,
}
```

`repr(C)` requests the same representation used by the Ripple C compiler.

```rust
extern "C" {
    fn translate(p: *mut Point, dx: c_int, dy: c_int);
}
```

An FFI call may receive a pointer to an appropriately typed local value:

```rust
let mut p = Point { x: 10, y: 20 };

unsafe {
    translate(&mut p, 5, 10);
}
```

Raw pointer operations are explicitly marked unsafe where required. The declaration author is responsible for accurately specifying any additional safety preconditions.

## 10.5 Strings

Native RFL strings are immutable UTF-8 values.

C strings use the `c"..."` literal syntax:

```rust
extern "C" {
    fn puts(s: *const c_char) -> c_int;
}

fn main() {
    puts(c"Hello from RFL!");
}
```

C string literals include a terminating zero value and use the target C character representation.

Native strings do not implicitly convert to C pointers.

## 10.6 Foreign function pointers

Captured RFL closures are not C function pointers.

C-compatible function pointers require functions using the C ABI with no captured environment.

Passing managed closures directly to C is unsupported in 0.1.

---

# 11. Module system

RFL uses a file-based module system inspired by TypeScript.

**One source file represents one module.**

There are no required module declarations.

## 11.1 Exports

**math.rfl**

```rust
export fn sum(a: i16, b: i16) -> i16 {
    a + b
}

export fn multiply(a: i16, b: i16) -> i16 {
    a * b
}

fn internal_helper() {
    // Private to this module.
}
```

Declarations are private unless explicitly exported.

## 11.2 Imports

**main.rfl**

```rust
import { sum, multiply } from "./math";

fn main() {
    let result = multiply(sum(2, 3), 4);
}
```

Module paths without extensions resolve to `.rfl` files.

Relative paths resolve from the importing file's directory.

## 11.3 Import aliases

```rust
import { sum as add } from "./math";
```

Named imports are supported.

Default exports are not.

Wildcard imports are not part of version 0.1.

## 11.4 Exporting types

**option.rfl**

```rust
export type Option<T> =
    | None
    | Some(T);
```

**main.rfl**

```rust
import { Option } from "./option";

fn main() {
    let value = Some(42);
}
```

Importing an exported ADT makes its constructors available.

Constructor name collisions must be resolved explicitly.

## 11.5 Standard library modules

```rust
import { print, println } from "std/io";
import { List, map, filter } from "std/list";
```

`std/` paths resolve against the installed RFL standard library.

No package manager is required.

## 11.6 Circular dependencies

Circular module imports are forbidden in RFL 0.1.

The compiler reports the dependency cycle.

Type-only cyclic dependencies may be supported in later versions.

---

# 12. Importing C source files

RFL supports direct imports from C99 source files.

This is a source-level build integration feature, separate from `extern "C"`.

**math.c**

```c
int sum(int a, int b) {
    return a + b;
}
```

**main.rfl**

```rust
import { sum } from "./math.c";

fn main() -> c_int {
    sum(20, 22)
}
```

When encountering a C source import, the compiler driver:

1. Invokes the existing Ripple C99 frontend.
2. Obtains its externally visible symbol declarations and ABI signatures.
3. Compiles the C translation unit to Ripple IR.
4. Makes the imported signatures available to the RFL type checker.
5. Includes the compiled C module in the final link.

`static` C declarations are not importable.

For version 0.1, automatic C source imports support functions and globals with directly representable C ABI types.

C macros, complex header-level declarations, and unsupported composite types require explicit bindings or wrappers.

### 12.1 Precompiled C libraries

Precompiled objects and archives are supported through linker inputs.

```rust
extern "C" {
    fn puts(s: *const c_char) -> c_int;
}
```

Then:

```bash
rfl build main.rfl --link libruntime.par
```

This preserves the existing Ripple linker and archive model.

---

# 13. Generics

RFL supports parametric polymorphism.

```rust
fn identity<T>(x: T) -> T {
    x
}
```

Generic parameters may be inferred:

```rust
let a = identity(42);
let b = identity("hello");
```

## 13.1 Generic ADTs

```rust
type Pair<A, B> =
    | Pair(A, B);
```

## 13.2 Compilation strategy

RFL 0.1 uses **monomorphization**.

Each concrete generic specialization generates its own implementation.

For example:

```rust
identity<i16>(42);
identity<bool>(true);
```

generates specializations for `i16` and `bool`.

The compiler performs whole-program generic instantiation across the module graph.

Generic function bodies therefore remain available to the frontend until all required specializations have been identified.

The initial implementation does not require a separate binary module-interface format.

---

# 14. Ripple IR compilation

The `rfl` compiler is independent of `rcc`.

Both share the extracted Ripple IR infrastructure.

The conceptual pipeline is:

```text
RFL Source
    │
    ▼
Lexer / Parser
    │
    ▼
AST
    │
    ▼
Name Resolution
    │
    ▼
Type Inference / Checking
    │
    ▼
Typed AST
    │
    ▼
Generic Specialization
    │
    ▼
Ripple IR
    │
    ▼
Ripple Backend
    │
    ▼
Ripple Object Files
    │
    ▼
Ripple Linker
    │
    ▼
Executable
```

## 14.1 IR mapping

| RFL construct | IR representation |
|---|---|
| Primitive values | Typed scalar values |
| Local variables | Slots in a heap activation record, or SSA values between calls |
| Mutable variables | Loads and stores in that record |
| Structures | Aggregate layouts and GEP |
| ADTs | Tagged representations |
| Pattern matching | Tests, branches, and field extraction |
| Function calls | A return to the driver with the next heap frame. See [lowering.md](lowering.md) |
| Closures | Code references and environment structures |
| Recursive functions | Heap frames. A tail call drops the caller frame |
| Generic functions | Specialized IR functions |
| Foreign functions | External declarations and linker symbols |
| Modules | Namespaced symbols and linkable units |

No RFL-specific instructions are required in the Ripple ISA.

## 14.2 Control flow

`if`, `match`, and loop constructs lower into basic blocks and branches.

Expression results may initially be represented through temporary allocations at control-flow joins.

The backend remains responsible for register allocation, instruction selection, and the existing banked-memory conventions. Source-level RFL objects are not hardware-stack allocations. [lowering.md](lowering.md) is the contract for calls, static data, narrow integers, and faults.

## 14.3 Native symbol names

Native RFL symbols are mangled to avoid cross-module collisions.

C ABI symbols retain their declared external names.

The mangling scheme must uniquely encode the module identity and function specialization, but its precise byte representation is an implementation detail of version 0.1.

---

# 15. Build system

**The compiler manages the dependency graph automatically.**

There is no requirement for Makefiles, CMake, or a separate build-system description.

## 15.1 Building an executable

```bash
rfl build main.rfl
```

The compiler discovers imported modules, compiles all required sources, and links the resulting executable.

```bash
rfl build main.rfl -o program.bin
```

## 15.2 Running a program

```bash
rfl run main.rfl
```

The compiler builds the program and executes it using the configured Ripple VM.

The initial implementation targets the Rust VM by default.

## 15.3 Type checking

```bash
rfl check main.rfl
```

Performs parsing, dependency resolution, and type checking without generating machine code.

## 15.4 Emitting IR

```bash
rfl compile main.rfl --emit-ir
```

Produces Ripple's textual intermediate representation.

The same IR can be consumed by the existing Ripple backend tooling.

## 15.5 Import resolution

The compiler recursively traverses imports starting from the entry module.

Each source module is compiled once per build invocation.

Compilation order follows the dependency graph.

**Incremental compilation is explicitly out of scope for RFL 0.1.**

There is no persistent object cache, dependency fingerprint database, or build-artifact reuse requirement.

Every build may rebuild the entire dependency graph.

### 15.6 Projects

A project requires only an entry-point source file.

```text
hello/
├── main.rfl
├── math.rfl
└── utils/
    └── strings.rfl
```

A project manifest is optional and unnecessary for basic builds.

Package management, remote dependencies, and version resolution are not part of 0.1.

---

# 16. Program entry point

Executable programs define a `main` function.

Supported forms:

```rust
fn main() {
    // Returns unit.
}
```

Or:

```rust
fn main() -> i16 {
    0
}
```

The runtime translates `unit` completion into a successful exit status.

An integer return value becomes the program's exit status.

Executable entry-point requirements are validated by the compiler.

Libraries do not require `main`.

---

# 17. Error handling

RFL 0.1 has no exceptions.

Errors are represented as ordinary values.

The standard library defines:

```rust
type Result<T, E> =
    | Ok(T)
    | Err(E);
```

Example:

```rust
fn divide(a: i16, b: i16) -> Result<i16, String> {
    if (b == 0) {
        Err("Division by zero")
    } else {
        Ok(a / b)
    }
}
```

Callers use pattern matching:

```rust
match divide(10, 2) {
    Ok(value) => print(value),
    Err(message) => println(message),
}
```

Automatic error propagation syntax such as `?` is deferred.

---

# 18. Standard library

The initial standard library is deliberately small.

| Module | Responsibility |
|---|---|
| `std/io` | Terminal output and input |
| `std/list` | Immutable lists and higher-order operations |
| `std/option` | Optional values |
| `std/result` | Result values |
| `std/string` | Immutable string operations |
| `std/math` | Arithmetic helpers |
| `std/mem` | Low-level allocation and memory access |

The standard library may use C implementations compiled by `rcc`.

This is an implementation detail.

Applications do not need to know whether a standard library function was implemented in C or RFL.

Hardware and operating-system interfaces may be exposed through separate libraries.

The base language does not require knowledge of rOS.

---

# 19. Runtime and execution model

RFL programs execute under the existing Ripple ABI and runtime conventions.

There is no mandatory virtual machine inside RippleVM.

RFL-generated code is ordinary Ripple machine code.

### 19.1 Evaluation

Expression evaluation is eager.

Function arguments are evaluated left to right.

Pattern matching evaluates the scrutinee once.

### 19.2 Memory

Managed objects use reference counting.

RFL calls use heap activation records, so RFL recursion is not limited by the hardware stack bank. The hardware stack is the C stack used by the driver and by `extern "C"`. [lowering.md](lowering.md) specifies the layout, tail calls, and the frame arena.

Raw memory access follows Ripple's banked-memory semantics.

### 19.3 Runtime errors

The initial language defines runtime failure conditions for operations such as division by zero and checked out-of-bounds access.

The standalone runtime determines how these failures are reported to the host or operating system.

### 19.4 Concurrency

RFL 0.1 does not define threads, fibers, async functions, or concurrent execution.

These may be introduced later when Ripple's operating-system capabilities make them useful.

---

# 20. Syntax reference

A simplified grammar:

```ebnf
module          = { item } ;

item            = import
                | extern_block
                | [ "export" ] declaration ;

declaration     = function
                | type_decl
                | struct_decl
                | const_decl ;

function        = "fn", identifier,
                  [ generic_params ],
                  "(", [ parameters ], ")",
                  [ "->", type ],
                  block ;

lambda          = "fn", "(", [ parameters ], ")",
                  ( "=>", expression | block ) ;

type_decl       = "type", identifier,
                  [ generic_params ],
                  "=", variants, ";" ;

variants        = "|", variant,
                  { "|", variant } ;

variant         = identifier,
                  [ "(", type_list, ")" ] ;

import          = "import", "{", import_names, "}",
                  "from", string_literal, ";" ;

extern_block    = "extern", string_literal,
                  "{", { extern_decl }, "}" ;

block           = "{", { statement }, [ expression ], "}" ;

statement       = let_decl
                | assignment
                | expression, ";" ;

let_decl        = "let", [ "mut" ], identifier,
                  [ ":", type ],
                  "=", expression, ";" ;

expression      = literal
                | identifier
                | call
                | lambda
                | if_expr
                | match_expr
                | block
                | binary_expr
                | unary_expr ;

if_expr         = "if", "(", expression, ")",
                  block,
                  [ "else", ( block | if_expr ) ] ;

match_expr      = "match", expression,
                  "{", { pattern, "=>", expression, "," }, "}" ;
```

This grammar is illustrative rather than a complete parser specification. Operator precedence, attributes, destructuring forms, and lexical rules require their own formal productions before the syntax is considered frozen.

---

# 21. Complete example

A small RFL program using ADTs, generics, closures, modules, and C interoperability.

### `math.c`

```c
int sum(int a, int b) {
    return a + b;
}
```

### `list.rfl`

```rust
export type List<T> =
    | Nil
    | Cons(T, List<T>);

export fn map(f, xs) {
    match xs {
        Nil => Nil,
        Cons(x, rest) => Cons(f(x), map(f, rest)),
    }
}

export fn fold(f, initial, xs) {
    match xs {
        Nil => initial,
        Cons(x, rest) => fold(f, f(initial, x), rest),
    }
}
```

### `main.rfl`

```rust
import { List, map, fold } from "./list";
import { sum } from "./math.c";

extern "C" {
    fn printf(format: *const c_char, ...) -> c_int;
}

fn main() -> c_int {
    let numbers =
        Cons(1,
            Cons(2,
                Cons(3,
                    Cons(4, Nil))));

    let doubled = map(fn(x) => sum(x, x), numbers);

    let total = fold(
        fn(acc, x) => acc + x,
        0,
        doubled
    );

    printf(c"Result: %d\n", total);

    0
}
```

Expected output:

```text
Result: 20
```

Build:

```bash
rfl run main.rfl
```

The compiler discovers both imports, uses the RFL frontend for `list.rfl`, the existing C frontend for `math.c`, and the declared C ABI reference for `printf`.

The driver then compiles and links the complete program.

No Makefile is required.

The resulting executable can run on any compatible Ripple implementation.

Including, naturally, the Brainfuck CPU.

---

# 22. Explicitly deferred features

The following are **not part of the RFL 0.1 specification**:

| Feature | Status |
|---|---|
| Incremental compilation | Deferred |
| Package manager | Deferred |
| Mandatory project manifests | Rejected |
| Implicit currying | Rejected |
| Significant indentation | Rejected |
| Exceptions | Deferred |
| Async/await | Deferred |
| Threads and fibers | Deferred |
| Traits and type classes | Deferred |
| Operator overloading | Deferred |
| Procedural macros | Deferred |
| Reflection | Deferred |
| Full garbage collection | Deferred |
| Advanced ownership analysis | Deferred |
| Automatic C header translation | Deferred |
| Higher-kinded types | Deferred |
| Polymorphic recursion inference | Deferred |
| JIT compilation | Deferred |
| Persistent language images | Deferred |

These decisions keep the first compiler focused on the core language.

---

## 23. Implementation milestones

I'd divide the implementation into four stages.

**Milestone 1 — Core language**

Implement parsing, immutable bindings, primitive types, functions, function calls, arithmetic, conditionals, and Ripple IR generation.

The first compiler should successfully compile:

```rust
fn main() -> i16 {
    42
}
```

**Milestone 2 — Functional language**

Add ADTs, pattern matching, inference, generics, closures, and the managed-object runtime.

At this point, the `List<T>` and `map()` examples should work.

**Milestone 3 — Modules and interoperability**

Implement imports, exports, module-graph compilation, C ABI declarations, C source imports, and linking.

At this point, mixed-language programs should work without Makefiles.

**Milestone 4 — Standard library and rOS**

Implement basic collections, strings, I/O, error handling, and operating-system bindings.

At this point, RFL becomes a practical language for writing Ripple applications rather than merely demonstrating compiler features.

---

## 24. Design identity

RFL is intended to occupy a fairly specific niche.

It is not trying to be Rust without borrow checking, C with algebraic data types, or OCaml with braces.

It takes the expressive capabilities of functional programming seriously without requiring programmers to adopt an entirely functional style.

Its central design commitments are:

**Types should be inferred whenever possible.**

**Mutability should be explicit.**

**Functions should behave like ordinary functions.**

**Memory management should not dominate application code.**

**Modules should eliminate manual build orchestration.**

**C interoperability should be straightforward.**

**The language should compile to an existing, well-defined IR.**

And finally:

**The source language must not care whether RippleVM is implemented in Rust, Brainfuck, JavaScript, or physical hardware.**
