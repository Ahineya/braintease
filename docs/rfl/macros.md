# RFL procedural macros

RFL has imported, user-authored expression and item macros. They run during
compilation and produce ordinary RFL syntax, which goes through the existing
resolver, type checker, ownership checker and lowering pipeline. The compiler
does not implement HTML expansion; the standard macro is ordinary user code.
The source formatter has a separate, non-executing HTML layout parser.

## First macro

Put the implementation in a separate module:

```rfl
// identity_macro.rfl
import {TokenStream} from "std/macro";

#[proc_macro]
export fn identity(input:TokenStream)->TokenStream {input}
```

Import it into the compile-time namespace:

```rfl
import macro {identity as keep} from "./identity_macro.rfl";

fn keep(n:i16)->i16 {n+1}
fn main(){
    let answer=20;
    println(keep!{answer+22});
    println(keep!(keep(40)));
    println(keep![keep!{answer}]);
}
```

This prints `42`, `41`, and `20`. An ordinary function may have the same name as
a macro. Imports support aliases and `export import macro {...} from "..."`
reexports. Macro visibility follows the marked exported entry point; an ordinary
import cannot turn it into a runtime function. Signatures are checked against
the actual `std/macro.TokenStream` type, including aliases, rather than spelling.
Entries must be non-generic, synchronous RFL functions with one value parameter
and an explicit `TokenStream` result. Implementation helpers may use generics,
ADTs, closures, loops and managed collections.

The implementation must be in a separate module. Same-module bootstrap and
import/reexport cycles are errors. Supported invocation positions are expressions
and module items; type/pattern invocations, derives and general attribute
transforms are not supported. Item macros may generate ordinary declarations
and more invocations, but cannot add textual imports, reexports or new procedural
macro declarations. Caller-supplied identifier tokens can name generated items.

## Syntax construction and hygiene

[`std/macro.rfl`](../../rfl/std/macro.rfl) is the public compiler-facing library.
It does not expose the Rust AST or a native shared-library ABI.

```rfl
import {TokenStream,splice} from "std/macro";

#[proc_macro]
export fn twice(input:TokenStream)->TokenStream {
    splice("{let temporary=$0;temporary+temporary}",[input])
}
```

`splice` inserts complete token streams at `$0`, `$1`, etc. It never substitutes
inside strings, comments or identifier fragments. The quotation is parsed as
syntax. This macro evaluates its input once and introduces a private local;
a caller's unrelated `temporary` binding keeps its identity. A macro can still
deliberately duplicate a supplied expression by splicing it twice; evaluation
count is the macro author's responsibility.

| API | Contract |
| --- | --- |
| `empty`, `concat` | Construct and concatenate syntax streams. |
| `quote` | Construct definition-site syntax for parsing by the compiler. |
| `splice` | Quote syntax with explicit token-stream interpolation. |
| `ident` | Identifier in this expansion's definition-site context. |
| `call_site_ident` | Explicitly construct an identifier that resolves in caller scope. |
| `fresh_ident(name,label)` | A fresh context family, shared only by equal labels within one expansion. |
| `literal` | A complete RFL literal spelling, including its quotes and escapes. |
| `punctuation`, `joint_punctuation` | Separate punctuation, or join it to the following punctuation token. |
| `group` | Construct a balanced `(`, `[`, or `{` group. |
| `span_of`, `context_of`, `with_span` | Preserve/inspect opaque metadata; changing a diagnostic location does not change binding identity. |
| `call_span`, `diagnostic`, `diagnostic_notes` | Report an error with a primary location and bounded notes. |
| `caller_target` | Read the caller's requested `ripple`, `native`, or `wasm` target. |
| `declared_input` | Read only compiler-provided declared input content and its fingerprint. |

Created bindings have compiler-minted syntax contexts, independently of source
locations. Internal names encode those identities for the existing resolver;
choosing a funny printable prefix is not the public hygiene mechanism.
Spliced caller tokens retain their original context and source identity.
Definition-site library references pull in ordinary target-compiled helpers,
even if the caller has an identically named function or local. Those resolver
dependencies are distinct from the macro implementation's host dependency graph.
Generated method names also use the traits visible at their definition site;
method names spliced from caller expressions retain the caller's trait scope.
Implicit helper imports do not expose those traits to unrelated caller methods.
Associated projections and traits declared by item macros retain the generated
helper's lexical trait scope as well. A generic helper's `T::Output` keeps the
contract selected from `T`'s bounds when specialized, even if the concrete type
also implements another visible trait with an associated type named `Output`.
Macro entry points and compiler API implementation helpers are excluded when a
mixed macro package is loaded for its ordinary runtime helpers.

Keeping runtime builders in a separate ordinary module, as the example does,
also avoids a v1 phase-separation limitation: projection of mixed modules follows
explicit compiler-API types and references. A compile-time helper that calls an
imported builder and leaves its compiler-API return type inferred can require an
explicit return annotation or separation from the runtime builder module.

A generated public item name should usually come from an input identifier. For
example, `splice("fn $0()->i16{42}",[input])` used as `declare!{answer}` gives the
editor a real caller spelling to navigate and rename. `call_site_ident("answer")`
is also possible, but there is no caller token to rename in that case; editing
the macro implementation is required.

### Lossless tokens

`TokenStream.tokens` is a flat representation of balanced token trees, with
paired Open/Close entries. It avoids depending on private compiler layout.
`Token.kind` values in protocol 2 are:

| Kind | Meaning |
| --- | --- |
| 1 | Identifier, including language-word spellings in a raw DSL. |
| 2 | Literal spelling. |
| 3 / 8 | Punctuation, separate / joint with subsequent punctuation. |
| 4 | Whitespace or comment, with original spelling. |
| 5 / 6 | Opening / actual closing delimiter. |
| 7 | Other raw Unicode text. |
| 10 / 11 | Quotation / quotation with explicit splice streams. |
| 12 / 13 | Internal balanced splice-value markers emitted by `splice`. |
| 20 / 21 | Error / explanatory note emitted by diagnostic builders. |

The lexer captures a macro group before trying to lex its body as ordinary RFL.
Apostrophes in words, `@`, Unicode and meaningful whitespace survive. Delimiters
must balance; ordinary quoted strings, character literals and comments shield
delimiters. A lone apostrophe starts a character literal only when it forms a
complete one-character/escaped-character spelling. Ordinary comment syntax
still applies inside raw input; quote URLs/strings containing comment markers.
Embedded RFL expression streams retain their input locations and are parsed by
the ordinary parser when used in the output. Final expression output must be
exactly one expression: trailing tokens are an error.

`origin`, `syntax`, and the handle fields of `Span`/`SyntaxContext` are opaque
invocation-scoped metadata. Copy them through the API; do not interpret their
text, persist them or manufacture IDs. The compiler validates returned handles,
token spellings, balanced groups and construction operations. Requests contain
no host pointers. Source identity, parent expansion, definition module and syntax
context are tracked separately from the legacy byte-range `Span` representation.

## Targets, order and declared inputs

Caller cfg is processed before resolving macro imports or running invocations.
Inactive imports do not require a macro package to exist. The implementation
itself is compiled for native execution; `caller_target(input)` still reports
the target of the program being expanded. Definition-site runtime dependencies
are resolved and compiled for that caller target.

Item invocations expand in source order. The compiler then walks declaration
bodies in its ordinary AST order, expanding an outer invocation before any
invocations in its output. An invocation discarded by an outer macro never runs.
The order is deterministic; macros have no shared mutable process state.

Extra build data must be declared in a sidecar named after the implementation,
for example `labels.rfl.inputs` alongside `labels.rfl`:

```text
# Module-relative UTF-8 files; blank lines and whole-line comments are ignored.
labels.txt
schema.json
```

`declared_input(input,"labels.txt")` returns `Result<Dependency,String>` containing
`path`, exact UTF-8 `text`, and a versioned `hash`. The current fingerprint is
`fnv1a64:...`, for build invalidation, not authentication. Cache keys also retain
the complete input content. Undeclared inputs return `Err`; file changes and
manifest changes invalidate the artifact key and editor analysis. Inputs belong
to the actual implementation module, including when imported through a facade.

## Native bootstrap and limits

The default executor compiles real RFL implementation code with a generated
entry wrapper, using the existing native backend, and executes the resulting
program in an owned child process. An executor trait and versioned wire protocol
form the seam for a future sandboxed Wasm runner. Public macro source is
independent of the runner. No Wasm backend, DOM, JavaScript or VM CPU change is
required to use this implementation.

**Native macro implementations are trusted build code.** Subprocesses, capability
checks and resource limits are not an OS security sandbox. The deterministic
profile checks the real typed call graph and transitive implementation inputs.
Direct/transitive extern services, C imports and unsafe code are rejected; the
private wrapper alone may use its stdio transport. Pure allocation and language
operations are allowed. File, clock, random, process and network observations
are not user macro services. Diagnostics use explicit protocol operations;
ordinary stdout from macro code is forbidden.

The `RFLM2` transport is UTF-8 octet framed, with explicit version, decimal length,
field length, count and trailing-output checks. Stdout is exclusively the
protocol; stderr is captured separately. Output metadata is validated against
that request. EOF/exit failures and malformed responses fail the compilation.
Request reads and response serialization use balanced string chunks, avoiding
repeated whole-frame copies as an HTML macro grows. This leaves the framing,
60,000-octet limit and execution deadlines unchanged. Acceptance tests exercise
a 57,000-character input and a response containing 1,000 trivia tokens within
the existing runtime budget, with Native/RVM output parity.

Current bounds:

* 60,000 octets per protocol body, including token metadata and declared inputs.
  This also stays within the compatibility ABI's 16-bit String length.
* 128 input/output group levels; 64 recursive expansions.
* 100,000 generated tokens and 1 MiB of generated spelling per compilation unit,
  conservatively including quotation/splice construction work.
* 16 notes per macro diagnostic, and at most eight displayed expansion ancestors.
* Five-second native runtime deadline, with a seven-second parent wall deadline;
  bounded stdout/stderr readers kill and reap only the owned execution child.
* At most 32 declared UTF-8 inputs, 20,000 input-content octets total, and a
  4,096-octet manifest. Request framing may impose a smaller combined limit.
* Thirty-second artifact-lock wait. Compilation itself uses the existing native
  compiler/linker path; the execution deadline does not bound compiler time.

Compiled implementations are cached under `target/rfl-macros/`. Keys include the
compiler identity, protocol, native runtime identity, entry point, transitive RFL
sources and declared input contents. OS locks serialize construction, and only a
successful build installs its cache stamp. Changing a dependency or a failed
build cannot install a stale successful artifact.

**Expansion-result caching is deliberately disabled.** The executable cache is
separate from syntax results. Each expansion receives current input spelling,
trivia, caller target and validated source/context handles, then gets fresh
compiler identities. This avoids stale locations or identifier collisions until
there is a separately proven context-rebinding result cache. Editor queries
reuse checked analysis for unchanged buffers/dependencies; each hover does not
spawn another macro process.

## Browser HTML macro

`std/html_macro` is the public browser-facing package built with this macro
system. It emits ordinary `std/html.Html<Msg>` constructors and is consumed by
the Wasm renderer. See [typed browser UI](browser-ui.md) for Model/Msg/commands,
keyed reconciliation, controlled input, SVG/fragments, syntax and current limits.
The experimental proof below remains a separate serialization fixture.

## Experimental JSX proof

[`rfl/examples/macros/`](../../rfl/examples/macros/) contains the user-authored
`html_macro.rfl`, pure typed `tree.rfl`, and runnable `main.rfl`:

```sh
rfl run rfl/examples/macros/main.rfl --target native
rfl run rfl/examples/macros/main.rfl --target ripple --jit
```

The macro supports nested tags, raw UTF-8 text and whitespace, quoted attributes,
`{RFL expressions}`, self-closing nodes, capitalized component functions and
`on_...` message arguments. Attributes are String values; event values have the
surrounding `Node<Msg>` message type. An embedded child expression produces a
`Node<Msg>`; wrap strings with ordinary `text(...)`. Components receive their
property expressions in written order followed by their children vector.
All expressions evaluate once, in source order. Ordinary type/ownership checking
rejects invalid props, messages and moved values.

This is a small test grammar and tree library, **not the final browser HTML/SVG
schema or renderer ABI**. It serializes trees in native and RVM tests; there is
no runtime HTML parsing or JS call. Entity decoding, spreads, a schema of valid
tags/properties, fragments and a production renderer are outside this example.

## Editors and verification

CLI/check and editor analysis use the same expansion and ordinary checker.
Formatting uses the same source parser and lossless group capture and does not
execute macros. Arbitrary DSL groups remain verbatim; explicit standard HTML
imports opt into [HTML-aware layout](../../rfl/formatting.md#standard-html-macros),
with parsed-tree/text preservation checks. Hover,
definition, completion and rename retain caller token identities. Private
created symbols are hidden. Generated declarations with caller-provided names
navigate back to those caller tokens. Refactoring expands syntax before using
the existing binding index and re-resolves the edited program before accepting
its edits.

The LSP analyzes unsaved macro implementation overlays, fingerprints transitive
source/declared inputs, and refreshes affected open documents. Failed expansion
replaces previous analysis; it does not keep reporting stale successful types.
Incomplete groups are diagnosed; recovery discards the damaged declaration at a
following source-level declaration boundary and continues checking the rest.
It does not guess a type for the incomplete DSL.

The IDEA lexer treats macro groups as opaque, lossless tokens, supports lexer
restarts, and lets the server decide whether an interpolation identifier is
renameable. Compiler discovery policy is unchanged.

The implementation's focused regressions were first run red before adding
production support. Acceptance coverage lives in `rfl/tests/macros.rs`,
`rfl/src/macros/tests.rs`, and the actual stdio
[`lsp_macros.py`](../../rfl/editors/intellij/tests/lsp_macros.py). It executes real
user-authored RFL, checks native/RVM results, and includes a compact bounded BF
CPU smoke test. Protocol unit tests do not replace user-code execution tests.

### Verification record — 2026-10-18

Release tests use `--jobs 2`. The implementation was verified with:

| Coverage | Result |
| --- | --- |
| RFL library, parser, checker, editor and protocol tests | 97 passed |
| Macro acceptance tests with actual compiled RFL implementations | 30 passed |
| Existing compiler/module/import integration tests | 105 passed |
| Existing formatter integration tests | 16 passed |
| Existing target-cfg integration tests | 4 passed |
| Composition, comprehensions, constructor traits, managed collections, once functions, partial constructors, places, resources, scopes, traits and type constructors | 163 passed across 11 suites |
| Actual stdio LSP suites, including macros, comprehensions, traits, effects and applicative/console | 14 suites passed |
| IntelliJ lexer/editor, executable discovery and run configuration checks | Passed; installable plugin packaged |
| Normal release compiler and documented JSX example | Built; Native and RVM JIT output matched, including UTF-8 text and event count |

The macro acceptance tests include Native/RVM output parity, a compact BF CPU
execution, typed JSX construction, ownership/evaluation-order checks, generated
declarations and runtime helpers, bootstrap macros that generate another macro's
implementation, declared-input invalidation through that bootstrap graph,
timeouts, forged metadata, retry after failed expansion, and generated locals
whose spellings match prelude types or intrinsics. Library tests also
exercise macro implementations that exist only in unsaved editor buffers.
The browser-child additions cover definition-site method traits, caller splices,
expected receiver types through conditionals/blocks/matches, and generated traits
whose associated types keep their declaration-time contracts during specialization.

The principal compiler checks can be reproduced with:

```sh
cargo test --release --jobs 2 -p rfl --lib --test macros \
    --test compile --test format --test target_cfg
python3 rfl/editors/intellij/build.py --test --server target/release/rfl
python3 rfl/editors/intellij/tests/lsp_applicative_console.py target/release/rfl
python3 rfl/editors/intellij/tests/lsp_effects.py target/release/rfl
```

During iteration, the Rust checks used a separate `CARGO_TARGET_DIR` and release
profile overrides `CARGO_PROFILE_RELEASE_LTO=off` and
`CARGO_PROFILE_RELEASE_CODEGEN_UNITS=16` to shorten compiler relinking. These
overrides do not change the repository's normal release profile or executable
discovery policy.
