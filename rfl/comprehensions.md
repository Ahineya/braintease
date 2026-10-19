# Sequential comprehensions

The comprehension syntax is an expression:

```rfl
let total = for {
    a <- Some(20);
    b <- Some(22);
    yield a + b;
};
```

It expands to the ordinary statically checked `std/fp::bind` and `map`
operations. The first source is evaluated once when constructing the expression;
each later source and local `let` runs inside its preceding continuation. Option
and Result short circuit; Vec visits combinations in left-major order; reusable
Effect recipes execute their continuations afresh on every run. The final yield
is mapped during execution, rather than injected as an already-evaluated value.
A comprehension requires at least one binding and exactly one final yield.

`for once { ... }` selects `flat_map_once` and `map_once`, with consuming
continuations for Option, Result and OwnedEffect. Ordinary `for` selects reusable
continuations and the independent FlatMap/Functor capabilities. Vec may invoke a
continuation many times and does not implement the consuming contracts.

Bindings accept ordinary irrefutable parameter patterns and optional type
annotations. Local `let` declarations belong after a binding. Use explicit
`move(name)` when transferring an existing exclusive value, exactly as in the
corresponding function calls; sugar never silently clones resources. An exclusive
value cannot be captured by an ordinary reusable continuation: use a consuming
program or an explicit shared handle when it must survive later bindings.

The compiler retains source spans for bindings, expressions and patterns. Helper
names are private compiler aliases; local functions or imports named bind/map
cannot change a comprehension's interpretation. Existing `for (x in values)`
loops and identifiers named yield/once retain their meaning. Yield is contextual
inside a comprehension; `<-` is recognized only at its binding boundary.

Guards and refutable patterns have no implicit filtering policy. Return, await
and `?` across generated continuations require explicit callbacks/effect lifting;
they must not silently acquire a different return or suspension boundary. Loops
and explicit nested callbacks retain their ordinary local control flow. There
is no inferred parallelism or additional runtime beyond the desugared calls.

## Expansion and execution

```rfl
for {
    a <- first;
    let doubled = a * 2;
    b <- next(doubled);
    yield a + b
}
```

has the behavior of:

```rfl
bind(first, fn(a) => {
    let doubled = a * 2;
    map(next(doubled), fn(b) => a + b)
})
```

The compiler uses the actual std/fp operations through private aliases. A
single-binding comprehension needs only Functor; additional bindings need
FlatMap. The consuming form analogously needs FunctorOnce and, for additional
bindings, MonadOnce. Neither form calls pure, and no FromFactory requirement is
introduced. Empty/failing computations release unused consuming captures using
the same contracts as explicit combinator calls.

Only binding clauses, local let declarations and a final yield are accepted at
the comprehension's top level. Put synchronous work in a local initializer or
an explicit expression block. A final yield semicolon is optional. Tuple,
struct and other irrefutable patterns use ordinary parameter checking; a
refutable Some/Ok pattern is an error rather than an implicit filter. All
bindings use one constructor and fixed arguments such as the Result error type.
Type annotations on bindings or the whole result resolve ordinary inference
ambiguities; no new type inference heuristics are added.

The first source runs during construction even for a lazy Effect. Constructing
an effect with print/read_line is itself lazy; arbitrary synchronous I/O in a
source-producing function is ordinary eager RFL code. Later sources, local
initializers and yield expressions execute according to the selected constructor.
Their callback bodies are not automatically lifted into asynchronous functions.
Use from_async/from_future or named async helpers to suspend inside an effect.
Break/continue within a loop written inside an initializer/yield block stay
local to that loop; explicit nested callbacks retain their own return/try rules.

## Console example

[`examples/comprehensions.rfl`](examples/comprehensions.rfl) defines one generic
Console greeting with two sequential effect bindings. It runs that recipe
twice against the recording interpreter, using the preloaded names Ada and
Ripple rather than prompting for keyboard input, and produces:

```text
Hello, Ada!
Hello, Ripple!
```

```sh
target/release/rfl run rfl/examples/comprehensions.rfl --target native
target/release/rfl run rfl/examples/comprehensions.rfl
target/release/rfl run rfl/examples/comprehensions.rfl --target rbf
```

`--target rbf` exports the original Brainfuck CPU and runs it through the RBF
host, with lightweight telemetry on stderr: a dot every five seconds, BF/guest
counts and rates every thirty seconds, and a final summary. Add `--jit` to use
the Brainfuck JIT. Guest output stays on stdout and stdin supplies guest TTY
input. This runner uses sixteen 64,000-cell RAM banks, with IRQ polling disabled.
It uses the linker's compact `--format bf` exporter, which streams ordinary
Brainfuck initialization and expands only the CPU template. It requires the
release `rasm`, `rlink`, and `rbf` tools plus the built runtime. Temporary
assembly, objects and expanded Brainfuck are removed when the run finishes.
The `rbf` choice belongs to `run`; source checking/code generation still use
the Ripple target and its software floating point.
See the [Brainfuck performance investigation](../docs/rfl/brainfuck-performance.md)
for export costs, generated-code measurements and remaining optimization work.

The formatter retains the source syntax and comments. Hover, navigation, rename
and completion operate on the original bindings, rather than private helper
names. The IDEA lexer already accepts the syntax; semantic support comes from
its configured compiler, whose discovery behavior is unchanged.

## Choosing a partial constructor

Unary constructors such as Option and Vec are inferred normally. A constructor
with fixed arguments needs an explicit varying slot, as in ordinary generic FP
calls:

```rfl
let result = for<Result<_,String>> {
    a <- Ok(20);
    b <- Ok(22);
    yield a + b
};
```

Use `for<Effect<R,E,_>>` for reusable effects and
`for once<OwnedEffect<R,E,_>>` for consuming programs. The constructor annotation
applies to every binding; it is compile-time metadata with no runtime dictionary.
A whole-result annotation alone does not identify which of several constructor
arguments may vary. General type aliases remain a future ergonomics improvement.
Binding annotations also contextualize their source, so
`for { n:i16 <- None; yield n }` has an unambiguous empty Option result.

## Validation

The comprehension tests cover ordinary/custom/generic constructors, dependent
sources and local bindings, tuple/struct patterns, shadowing, consuming captures,
owned errors, lazy reusable outputs, unpolled destruction and child-first pending
cancellation. Rejection tests cover incompatible constructors, refutable patterns,
missing yields/separators, guards, reference captures and nonlocal control flow.
A bounded program runs on the original Brainfuck CPU. The guest-work regression
compares with explicit bind/map calls and rejects material extra instruction
work (a 5% margin plus 1000 instructions for the small fixture).

Separate checker regressions first reproduced discarded callback input types
and missing source context, then verify tuple/struct/owned/wide inputs, nullary
variants and vector literals. Both native and RVM execute the runtime cases.
LSP protocol coverage checks original binding spans, visibility before/after a
binding, normalized result types, private helper exclusion, signature help,
ordinary/constructor-parameter rename, formatting and diagnostic locations.

```sh
cargo test --release --jobs 2 -p rfl --test comprehensions --test callback_inference
python3 rfl/editors/intellij/tests/lsp_comprehensions.py target/release/rfl
python3 rfl/editors/intellij/build.py --test
```
