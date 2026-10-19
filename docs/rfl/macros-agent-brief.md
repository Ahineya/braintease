# Agent task: implement general RFL procedural macros

Status: v1 implemented, 2026-10-18. See the [public contract and verification
record](macros.md) and [experimental examples](../../rfl/examples/macros/).
The assignment below records the accepted scope; the Wasm executor and production
browser UI remain separate follow-ups.

Implement the RFL macro system described here. Deliver working user-authored
macros, tests, examples and editor support; do not stop at another proposal or a
compiler-only HTML special case. Read the shared [Wasm/browser decisions](wasm-browser.md)
for context. Another agent owns that backend; your work must be usable before
the Wasm backend exists.

## Goal and accepted boundaries

RFL needs Rust-like function-style macro invocations, particularly to support a
typed JSX-shaped DSL inside `html! { ... }`. Expansion produces ordinary RFL
syntax before type checking. Existing type, ownership, effect and target rules
then apply unchanged. Macros are a general language feature, not a new runtime
execution model or a browser compiler special case.

The first implementation includes expression and item expansion. Declarative
`macro_rules`-style matching, derive macros, attribute-transform macros, typed
reflection, arbitrary type/pattern-position invocations and a production browser
UI framework are follow-ups. Unsupported positions must diagnose explicitly.
The marker declaring a procedural macro is part of this task; it does not imply
general user-defined attribute macros.

Use this surface direction:

```rfl
// In a separate macro implementation module; compiler API names below
// illustrate the public library that this task must define and document.
import { TokenStream } from "std/macro";

#[proc_macro]
export fn identity(input: TokenStream) -> TokenStream { input }
```

```rfl
import macro { identity } from "./identity_macro.rfl";

fn main() {
    println(identity! { 20 + 22 });
}
```

Support parenthesized, braced and bracketed invocation groups. Macro names live
in an explicit macro namespace; imports, aliases, exports and visibility must
behave consistently. Keep `macro` contextual where possible. Macro entry points
are compile-time functions with a checked non-generic protocol signature, not
ordinary runtime calls. Implementation helpers can use ordinary RFL generics,
ADTs, loops and collections.

Macro implementations live in separate modules for v1. Reject same-module
bootstrap cycles with a clear error. Item macros may generate ordinary
declarations and further invocations. Generating new module imports/reexports
is deferred; diagnose it instead of silently changing the dependency graph.

The example names are the intended starting spelling. If existing compiler
constraints require a public API adjustment, document the precise replacement
and update every example/test. Do not weaken hygiene, type checking or
user-authored execution to make the example work.

## Read first

Follow repository instructions, including `CLAUDE.md` and applicable `AGENTS.md`.
Read files fully before editing them, never use `sed`, and never full-read
expanded Brainfuck files. The checkout contains substantial unrelated work.
Do not reset, stash, reformat or overwrite it. Do not change compiler discovery
to prefer a workspace binary: the user explicitly rejected that behavior.

Relevant sources and contracts:

* `rfl/src/lexer.rs`, `ast.rs`, `parser.rs`, `modules.rs`, `lib.rs`
* `rfl/src/parser/cfg.rs`: caller target controls cfg, not compiler host
* `rfl/src/parser/comprehension.rs`: existing expansion/private helper precedent
* `rfl/src/check/`, formatter and LSP modules/tests
* `rfl/src/native.rs`: existing native compilation/execution foundation
* `rfl/consuming-callables.md`, `traits.md`, `generic-traits.md`, `async.md`
* `rfl/comprehensions.md` and its source-level editor regression tests
* `rfl/editors/intellij/` and compiler-backed editor tests

Do not add an alternative lightweight type checker or silently fall back to
guessed types when expansion fails. No new runtime instruction or BF CPU change
is needed. A blocking compiler bug needs a focused failing regression first:
RFL failures belong in RFL tests; C failures follow the repository's rct policy.

## Token and syntax API

Provide a public, versioned compiler-facing library rather than exposing the
compiler's private Rust AST layout as a plugin ABI. The minimum facilities are:

* Token streams/trees with identifiers, punctuation, literals and nested groups.
* Source spans and recoverable original spelling/trivia, including whitespace.
* Structured construction/quotation and token splicing, with explicit hygiene
  semantics. Do not implement expansion by unconstrained string substitution.
* Expression/item parsing helpers or equivalent structured syntax builders.
  Syntax helpers do not expose partially inferred types or execute target code.
* Diagnostics with a primary span, notes and expansion/call-site context.
* Read-only caller-target context and declared dependency inputs.

Single-source byte offsets are not enough for imported/generated tokens. Add
source identity, expansion provenance and syntax contexts through an appropriate
representation or side table. Preserve the meaning of existing source spans.
Generated identifiers must have genuine fresh identity; a printable unusual
prefix alone is not the public hygiene contract.

Caller-provided expressions keep caller binding identity. Macro-created locals
cannot capture or shadow unrelated caller locals accidentally. Library helper
references resolve in the defining module, even if the caller has functions or
imports with identical names. Any deliberate call-site identifier construction
must be explicit. Test nested expansion and cross-module aliasing.
Track definition-site runtime helper dependencies separately from macro-host
implementation dependencies. Generated calls must pull in the appropriate
ordinary target-compiled helper module without leaking compiler API code into
the application. This is resolver metadata, not permission for v1 item macros
to invent textual imports.

Handle operator token spacing/jointness (`>>`, `<`, `>`, `::`, etc.), delimiters,
strings, Unicode, comments and malformed input. A macro invocation must be
captured before normal expression parsing interprets its DSL. JSX text can
contain whitespace, apostrophes and punctuation invalid as ordinary RFL syntax;
preserve it for the DSL parser. Do not make quoted-text-only examples hide a
lexer that rejects valid raw macro input. Embedded `{...}` RFL expressions use
the ordinary expression parser and retain their original spans.

An expression expansion must parse as exactly one expression, with no ignored
trailing tokens. Item expansion must parse as valid items in its supported
position. Nested expansions retain their provenance and normal checks.

## Expansion, imports and cfg

Introduce explicit macro invocation nodes/token payloads and a separate
expansion phase. Discover the declared macro imports, load/type-check their
implementation graph, expand supported invocations, and then run ordinary
resolution/checking/lowering. Integrate with existing module scoping rather than
flattening macro output into synthetic text and losing binding identity.

Respect target cfg before executing inactive invocations or loading inactive
macro imports. Balanced syntax still needs to parse. The macro executable's own
execution target and its caller's requested target are different inputs. A
macro running through the native bootstrap must observe the caller's Ripple
target when expanding a Ripple program. Macro output must remain ordinary
target-checked RFL, including generated imports/helpers where supported.

Detect macro import/expansion/bootstrap cycles. Specify order of nested
expansion and keep it deterministic. Expansion cannot introduce silent changes
to evaluation count, order or ownership: quotations/builders must let a DSL
bind a supplied expression once when it would otherwise duplicate it.

## Executor and bootstrap: do not wait for Wasm

Define an executor interface and a versioned serialized request/response
protocol for token trees, spans, syntax contexts, diagnostics and caller-target
metadata. Public macro source must not depend on whether this protocol is
served by a native process or a future Wasm runner. Opaque compiler metadata
crosses through validated IDs, never host pointers or forged context integers.

Implement a bootstrap executor using the existing native compiler in a child
process, compiling user-authored RFL macro modules with a generated entry
wrapper. This is the default independent path for this milestone. Keep compiler
API implementation code modular so a Wasm executor can replace it later. Do not
implement the Wasm code generator or require the backend agent to finish first.

Use framed, bounded protocol messages with explicit version/length checks.
Separate protocol output from diagnostic/log output. Validate malformed output,
unexpected process exit and invalid spans/contexts. The protocol must support
the parsing/building facilities actually exposed by the public API; do not
promise compiler callbacks that the bootstrap transport cannot serve.

Enforce wall-time and output limits for owned child processes, as well as
expansion-depth, generated-token and diagnostic limits. Clean up only processes
and temporary files created for this operation. Document that native bootstrap
code is trusted build code: a subprocess and resource limits are not a security
sandbox. The future Wasm runner is the sandboxed execution direction.

Expose a deterministic macro execution profile with explicit compiler services.
Check transitive permitted services/dependencies; undeclared file, clock,
randomness, network or process observations cannot silently enter a supposedly
reproducible cached expansion. It is acceptable to reject these operations in
v1. Provide declared inputs through the compiler API, with their content hashes.

Cache compiled macro artifacts and expansion results separately. Keys include
compiler/protocol version, transitive implementation inputs, invocation tokens
and relevant trivia, caller-target context and declared dependency contents.
Cached syntax must be safely rebound to current source/expansion contexts;
stale spans and fresh-identifier collisions are correctness bugs. Do not cache
success after a failed expansion. If some contextual expansions cannot safely
be reused initially, skip their result cache explicitly while still caching
the compiled executable.

## JSX-shaped proof, independent of DOM

Supply a user-authored `html!`-style macro example against a small pure-RFL test
tree/builder library. Put that fixture/example under a clearly experimental
location, e.g. `rfl/examples/macros/`; do not establish an accidental final
browser UI ABI. Its expansion should produce ordinary typed constructors.

Exercise nested tags, raw text/whitespace, quoted attributes, embedded RFL
expressions, self-closing nodes, nested component calls and event/message
arguments. Preserve source evaluation order and evaluate each interpolation
once unless the DSL explicitly specifies another behavior. Type-check the
generated props/children/handlers using ordinary RFL contracts.

Render/serialize that test tree through native and RVM tests. No DOM, browser,
JavaScript or Wasm execution dependency is needed. A production renderer will
later batch DOM patches in Wasm; this macro must introduce no runtime JS calls
or runtime HTML parsing. A public HTML/SVG schema and full browser framework
remain integration work owned separately.

## Editor behavior

Share the same expansion/checking pipeline with CLI, check, formatter and LSP.
Preserve source-level macro syntax when formatting, including meaningful DSL
text. Compiler/LSP errors must point to invocation/interpolation locations with
expansion notes. Preserve original bindings for hover, definition, completion
and rename, and hide generated private symbols from ordinary completion.

Support incomplete invocation recovery: a missing closing delimiter while
typing must not crash the server or prevent diagnostics elsewhere. Share caches
and execution limits; changing a macro dependency invalidates affected buffers.
Do not execute a fresh unbounded process for every hover/keystroke or silently
display stale successful types after an error. Document native runner execution
limitations honestly. Keep the configured compiler discovery policy unchanged.

Update IDEA lexer/PSI/highlighting only where syntax actually requires it; most
semantics should continue to come from the compiler LSP. Add stdio protocol
tests using the actual compiler and preserve existing comprehension/HKT/trait
editor behavior.

## Tests and acceptance criteria

Before implementation, add focused regressions expressing the intended surface
behavior and demonstrate their failure. Keep executable assertions meaningful;
successful parsing alone does not prove expansion correctness.

Required coverage:

1. Imported user-authored identity and transforming macros; aliases, visibility,
   same-named ordinary functions, all invocation group delimiters, expression
   and item results, and nested invocations.
2. A macro generating a function/item used by ordinary RFL; malformed/trailing
   output, unsupported positions and illegal macro signatures diagnosed.
3. Hygiene against caller local/helper collisions, definition-site imports,
   caller expression binding identity, and two expansions generating locals in
   one scope. Include a negative ownership/type case with original locations.
4. Generics, closures, once callbacks, moves and managed values in interpolated
   expressions. Check evaluation order/count and cleanup when generated control
   flow skips a branch or returns early.
5. Inactive cfg does not execute/load macros; caller target is correct even
   when the macro runner uses the native execution target.
6. Cycles, runaway expansion, timeout, excess output, malformed protocol,
   executable failure and forged/stale metadata fail explicitly and clean up.
7. Compiled-artifact reuse, source/dependency/target invalidation and cache-safe
   source spans/fresh names. Repeat a build and compare observable results.
8. JSX-shaped tree assertions, Unicode/raw text/comments, malformed tags and
   delimiters, typed props/events, and interpolation errors in source positions.
9. Native/RVM output parity for generated programs. Add one bounded BF CPU smoke
   test using the existing compact export helper; do not run a full browser or
   greeting workload in BF.
10. Formatter round trips and actual LSP diagnostics/navigation/completion/rename
    for macro sources, imported/generated bindings and incomplete input.

Run relevant existing parser/checker, module/import, target cfg, closures,
ownership, comprehensions and editor suites. Use release builds/tests with
`--jobs 2`; format only edited Rust files. Do not create implementation-mirroring
tests in place of semantic/differential cases. If checks fail, fix and rerun the
affected coverage; do not silently label failures unsupported.

## Collaboration and finish

You own the RFL macro frontend, API, bootstrap executor, tests and editor work.
The other track owns `rcc-wasm/`, IR target/layout changes, target-neutral
linking/optimization, browser runtime and executor integration. Do not rewrite
those areas or add a competing Wasm target. Preserve new target variants in any
shared matches. Keep shared `Cargo.toml`/lockfile, `rfl/src/lib.rs`, cfg and module
edits focused; communicate actual conflicts through the user when necessary.

Deliver source changes, a concise public macro contract, working macro and
JSX-shaped examples, editor updates, and exact test results/remaining limits.
Record bootstrap limitations and the future Wasm-runner seam. Do not claim the
feature complete if only built-in macros work or tests use a fake executor in
place of real user-authored RFL macro execution.
