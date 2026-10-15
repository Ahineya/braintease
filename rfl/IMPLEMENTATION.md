# RFL implementation audit

RFL has a useful core, but does not yet implement the complete language described by
[initial.md](initial.md), [methods.md](methods.md), [more.md](more.md), and
[traits.md](traits.md). The failures were broader than missing syntax: evaluation
order, frame reuse, ownership, exhaustiveness, and generated code size all had
correctness problems. This audit distinguishes repaired behavior from remaining
work. Measurements below are from 15 October 2026.

## Repairs

- **Functional Brainfuck interpreter:** `examples/bf-fn.rfl` now prints `A` and
  exits successfully. Frame allocations carry their owning arena chunk and rewind
  checkpoint in a three-cell prefix. Release restores that checkpoint instead of
  guessing ownership from address ranges. The frame payload ABI stays unchanged.
- **ADT destruction:** destructor arguments are consistently `(tag, address,
  bank)`. Previously these were saved in the wrong order, corrupting cleanup.
- **Evaluation order:** operands and arguments hold value snapshots. A later
  argument mutating a binding no longer changes the already-evaluated argument.
- **Tail calls:** tail position survives sibling branches and unit-valued tails.
  Calls borrowing locals keep their owner frame alive; C entry wrappers retain
  their root frame and result. Both value and unit recursion have protocol tests.
- **Patterns:** a pattern matrix checks nested constructors, products, booleans,
  guards, coverage, and shadowed arms. The two redundant fallback arms in
  `bf-fn.rfl` were removed because its nested cases are exhaustive.
- **Early returns in match arms:** returning arms no longer force other arms to
  return `unit`. Segments are emitted in reverse postorder so pointer definitions
  precede blocks they dominate. Dead continuations do not enter resume dispatch.
- **Fixed arrays inside products:** layout records the element width, rather than
  multiplying by the array length twice. This previously produced diagnostics such
  as `expected [i16; 2], found [i16; 2]`.
- **Characters in collections:** vectors, slices, and fixed arrays accept `char`;
  character indexing, mutation, COW, push, and pop have an execution regression.
- **Strings:** concatenation, slicing, character encoding, and equality use shared
  generated helpers. Replacing a string releases its previous value. Inclusive
  slice ends check overflow before becoming exclusive ends.
- **Checking and diagnostics:** `check_source` and `check_file` do not lower RFL
  to executable IR. Libraries can be checked without `main`. File compilation
  maps type errors back from the combined module buffer to the original file.
- **Runner artifacts:** each run removes its own temporary assembly, object, and
  binary files, including on errors.

## Compiler organization

The former 8,844-line checker and 8,804-line lowerer are split by responsibility.
The entry files coordinate compilation and hold shared state; implementation is
in private child modules.

| Area | Files |
|---|---|
| Types, registration, specialization | `src/check/{types,register,functions,inference}.rs` |
| Expressions and calls | `src/check/{expressions,operators,calls,control}.rs` |
| Products, patterns, coverage | `src/check/{products,patterns,coverage}.rs` |
| Methods, references, captures | `src/check/{methods,borrows,closures,captures}.rs` |
| Checked expressions to segmented frame code | `src/lower/build.rs`, `src/lower/build/expr.rs` |
| IR emission and call protocol | `src/lower/{emit,frames,dispatch}.rs` |
| Ownership and object representations | `src/lower/{ownership,objects,memory}.rs` |
| Collections, text, arithmetic, C ABI | `src/lower/{arrays,vectors,strings,string_runtime,numeric,ffi}.rs` |
| RFL optimization preparation | `src/lower/optimize.rs` |

Struct and tuple layout tables are shared between procedures rather than deeply
cloned for each function. This is a structural cleanup, not a claim that the
checker now has an ideal internal API: several existing helpers still take too
many arguments, and inference and IDE metadata remain coupled to checking.

## Optimization changes

RFL's resumable heap frames hide locals from the backend's ordinary stack-local
SSA promotion. `promote_frame_words` gives nonescaping `i16` and `u16` slots local
homes while an activation runs, reloads them on entry, and publishes them before
a call stores its resume state. Results, borrowed slots, managed values, and
shared mutable captures keep their existing storage. The shared backend can then
promote the local homes and optimize arithmetic and loops. Wider and sub-word
slots are not promoted by this pass yet.

Only actual suspension continuations enter resume dispatch. Resume and function
selection use balanced decision trees. Unreachable RFL functions are excluded
from the driver after checking, including unused standard-library functions.
Closure targets and C exports remain roots or reachable dependencies.

Frame cleanup is generated once per procedure, rather than expanded at every
return and tail call. This matters for correctness as well as speed: duplicated
cleanup and string loops can overflow a code bank in otherwise small programs.
The existing string construction test exceeded the bank limit before these
changes and now assembles, links, and executes.

The shared backend optimizer itself is unchanged; these passes expose RFL's
semantics in a form it already understands. ARC traffic, frame-slot liveness,
wide scalar promotion, and call elimination remain optimization opportunities.

## Measurements

Release compiler built from baseline commit `a71b038` versus this implementation,
using the same backend sources and benchmark inputs. Compilation times are the
median of three compiler invocations, excluding Rust builds. Execution uses the
interpreter with 64,000-cell banks. Guest instruction counts come from RVM reports;
they are not wall-clock speedups.

| Input | Compile before | Compile after | Assembly instructions before / after | Executed instructions before / after |
|---|---:|---:|---:|---:|
| `benchmarks/scalars.rfl` | 83.7 ms | 47.3 ms | 2,981 / 2,591 | 3,699,137 / 709,206 |
| `benchmarks/strings.rfl` | 207.1 ms | 166.4 ms | 25,343 / 21,420 | 63,404 / 55,189 |

The scalar workload executes about 5.2 times fewer instructions. Both versions
print `29994`. Both string workloads print `Hello é` and `true`. These are focused
measurements, not a general performance guarantee.

The original functional BF example failed at runtime with `unreachable state`.
The corrected example prints `A`; its newly recognized exhaustive matches are
rejected by the baseline checker after removing the redundant fallback arms, so
the benchmark does not report a misleading like-for-like speedup for that file.

Reproduce with:

```sh
cargo build --release -p rfl
python3 rfl/tools/benchmark.py --baseline /path/to/old/rfl --execute
```

Omit `--baseline` to measure the current compiler alone. The script preserves
compile, assembly, link, and runtime failures in its JSON output.

## Remaining specification gaps

These are still unsupported; passing regression tests do not imply conformance.

| Requirement | Current behavior | Relevant implementation |
|---|---|---|
| Traits, bounds, coherence, qualified trait calls, `Printable` | Trait declarations are rejected; printing is compiler-built-in scalar/string output | `src/ast.rs`, `src/parser.rs`, `src/check/control.rs` |
| `defer`, including cleanup on `?`, return, break, and continue | No defer AST or lowering | `src/ast.rs`, `src/parser.rs`, `src/lower/build.rs` |
| Collections of managed or product values | `Vec<String>`, `Vec<Option<T>>`, nested vectors, and struct elements are rejected | `src/check/types.rs`, `src/typed.rs`, `src/lower/vectors.rs` |
| General reference places and safe dereference | `&point.x` is rejected; dereferencing `&i16` is treated as an unsafe raw-pointer operation | `src/check/borrows.rs`, `src/check/ffi.rs` |
| Constructors as callable values | `let cons = Cons` is rejected for a payload constructor | `src/check/constructors.rs`, `src/check/control.rs` |
| Polymorphic immutable local callables | A local identity lambda used first on an integer cannot also be called on a boolean | `src/check/closures.rs`, `src/check/inference.rs` |
| Full unsigned 64-bit literal range | `18446744073709551615` is rejected because integer tokens store signed `i64` | `src/lexer.rs`, `src/ast.rs` |
| Machine limits hidden from source values | Fixed-array lengths, frame metadata, reference counts, and root-frame storage still have machine-width or stack constraints | `src/ty.rs`, `src/lower/{frames,ownership}.rs` |
| Source locations on runtime faults | Fault reasons are printed, but faults do not carry the original expression location | `src/typed.rs`, `src/lower/faults.rs` |
| Complete build/link CLI | Only `check`, `compile`, `run`, and `lsp` exist; documented `build --link` is absent | `src/main.rs`, `src/run.rs` |

The collection restriction must not simply be removed. The typed module needs
collection element ownership metadata; copying, detaching, replacing, popping,
and dropping buffers must retain/release their elements recursively.

Similarly, traits need a registry and bound-aware generic checking, not just
rewriting methods after concrete substitution. Generic trait methods must retain
the meaning of their declared bounds even when a concrete type has an inherent
method with the same name.

## Follow-up order

1. Introduce a common place representation for names, fields, indexes, and
   dereferences, with explicit borrowing and write semantics.
2. Carry ownership/layout descriptors through typed collections and implement
   recursive element destruction and COW copying.
3. Unify scope exits and cleanup before implementing `defer`.
4. Implement the trait registry, explicit bounds, coherence, lexical trait
   visibility, qualified calls, and standard `Printable` implementations.
5. Complete generic local-value inference, numeric literal representation, and
   large-value storage; add conformance cases alongside each extension.

For code quality, replace argument-heavy checker/emitter helpers with focused
context and operation types. `cargo clippy -p rfl --all-targets --no-deps` still
fails on existing argument-count lints and IDE/LSP style issues; those lints were
not suppressed. Module loading for IDE analysis and compilation also still has
duplicated graph preparation, and checking C imports still constructs C IR.

## Validation

```sh
cargo test --release -p rfl -- --test-threads=4
cargo check -p rfl
```

The suite includes execution tests for the supplied functional BF example,
argument snapshots, ADT cleanup and frame churn, native and C calls, borrowed writes,
nested pattern coverage, fixed-array fields, character collections, string
helpers, real tail-call protocol, and original-file diagnostics. Optimization
regressions check that frame scalars reach SSA and repeated string operations
share helper bodies.
