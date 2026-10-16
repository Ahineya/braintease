# RFL traits milestone: Printable and library printing

**Status: implemented, 16 October 2026.** See [implementation notes](../../rfl/IMPLEMENTATION.md#static-traits-and-library-printing), [the runnable example](../../rfl/examples/traits.rfl), and [conformance tests](../../rfl/tests/traits.rs).

The staged plan below records the design and acceptance criteria.
Make `Printable`-based `print` and `println` the first complete, user-visible
traits milestone, before generic networking I/O abstractions or async support.
This puts the existing [traits draft](../../rfl/traits.md) into practical use;
it does not replace that specification or extend it with associated types.

## Intended behavior

A type becomes printable through an explicit `Printable` implementation.
`print` and `println` become ordinary generic library functions constrained by
that trait. Users can print their own types without editing the compiler.

Conceptual code using the proposed trait syntax:

```rust
export trait Printable {
    fn print(&self);
}

export fn print<T: Printable>(value: T) {
    value.print();
}

export fn println<T: Printable>(value: T) {
    value.print();
    putc('\n');
}
```

`write_text` is the current low-level text-output function in
[`std/io`](../../rfl/std/io.rfl); the trait draft calls the equivalent leaf
primitive `write_str`. Use a consistent final name when implementing the
library. Writing the newline must not recursively call generic `print`.
The code above is now accepted; the shipped `println` uses the byte-output leaf `putc` for its newline.

The trait receiver borrows the value for printing. The generic entry points
above take `T` as in the draft; check their call/ownership behavior against
RFL's actual managed-value semantics. Printing must not mutate the value or
leave it unusable when existing language semantics preserve it. The printing
contract requires no intermediate formatted String; this is not a promise that
all ownership operations or arbitrary user implementations allocate nothing.

## Compiler and library boundary

Keep low-level byte/text output and numerical formatting helpers available to
the library. Move the choice of how a type prints into explicit implementations,
including integers, booleans, characters and strings. Floating-point printing
should reuse available formatting support and preserve existing tested output.
Do not add a format-string engine as part of this milestone.

The compiler supplies general trait declaration/implementation checking,
registry lookup, generic bounds, method resolution and monomorphization.
Trait method calls lower to ordinary calls in shared IR. Printing should not
need a second type whitelist in the checker or a separate intrinsic path that
lets non-Printable types bypass their bounds.

Inventory current print/println forms, visibility/prelude behavior, formatting,
newlines and evaluation order before migrating. Preserve supported observable
behavior, or explicitly document and test an intentional language change. A
literal fast path may remain only when it is proven equivalent to the resolved
implementation; recognizing a type or function name alone must not bypass
trait semantics.

No dynamic dispatch, runtime reflection, new ISA instruction, associated type,
generic trait parameter, default trait method or async feature is required.
Use the existing architecture ABI for low-level runtime calls on each target.

## Implementation stages

1. **Capture the baseline.** Add output/ownership regressions for current scalar,
   string and floating printing where supported. Include empty strings,
   repeated calls, newline behavior and values used again after printing.
   Preserve useful diagnostics for wrong call arguments.
2. **Concrete traits.** Register a local trait and implement it for a user type
   such as `Point`. Validate method completeness, signature/receiver matching,
   visibility and qualified calls. Reject duplicate/ambiguous implementations
   from the start; later stages must not rely on silently choosing one.
3. **Generic bounds.** Implement `T: Printable` checking and specialize generic
   printing through the bound. Test a concrete type with a same-named inherent
   method: generic code must retain the trait contract defined by its bound.
4. **Generic implementations.** Support `Vec<T>` and `Option<T>` when
   `T: Printable`, and `Result<T, E>` when both payloads are printable. Prove
   recursive resolution with nested values and enforce coherence/orphan rules
   across the module graph. Follow the draft's representations and supported
   type coverage; do not imply every collection is automatically printable.
5. **Library migration.** Export `Printable`, `print` and `println` from the
   agreed standard module/prelude, with ordinary implementations for supported
   primitives, strings and generic collections/ADTs. Replace the compiler's
   built-in selection path after the generic path passes the baseline tests.
6. **Verify lowering and cost.** Run target parity and inspect representative
   IR for direct implementation calls, shared formatting helpers and reachable
   specializations. Check code size, ownership churn and unnecessary formatted
   String construction, particularly for the BF target.

The draft's [language acceptance example](../../rfl/traits.md#20-acceptance-test)
is a focused starting point. Its [required compiler behavior](../../rfl/traits.md#19-required-compiler-behavior)
remains the broader conformance checklist. Finish the required coherence and
module checks before declaring the end-to-end milestone complete.

## Acceptance tests

| Case | Required result |
|------|-----------------|
| User-defined Point with a local trait | Its implementation supplies the output; no compiler change per type |
| Generic print/println | Calls resolve through explicit Printable bounds; println adds exactly one newline |
| Primitive/string/float migration | Existing supported output remains correct, including boundary values and text encoding |
| Option and Vec of printable elements | Formatting composes through generic implementations |
| Nested Vec/Option and printable Result payloads | Recursive bounds resolve and output matches the draft's constructor/collection representations |
| Borrowed matching and managed values | Printing does not move a borrowed payload, mutate collections, leak owners or prematurely free storage |
| Non-Printable value or nested payload | Compile-time error identifies the unsatisfied type/bound |
| Missing, extra or mismatched implementation methods | Compile-time error points to the declaration/implementation mismatch |
| Overlapping or illegal foreign implementations | Rejected according to the draft's coherence/orphan rules |
| Ambiguous methods and qualified calls | Ambiguity is diagnosed; qualification selects the intended trait |
| Inherent method with the same name | Direct-call precedence follows the draft; generic bounded calls retain their trait meaning |
| Traits/functions imported across modules | Implementation discovery and lexical method visibility work without explicit implementation imports |

Use focused execution and compile-failure tests, then run the existing RFL suite.
Record RVM and native results separately. Ordinary lowering must preserve BF
compatibility; select a small explicit BF smoke case rather than making a large
BF run mandatory for each compiler iteration. Repeated nested printing should
also check resource reuse, not only matching a one-line output.

## What follows

This milestone proves a usable static trait system while replacing a real
compiler special case with library code. General formatting into memory, files
or network streams is later work: introduce writer-oriented interfaces with
explicit I/O errors/progress instead of stretching stdout-oriented `Printable`
into every output abstraction.

Associated types and the first polling foundation are now implemented. The
next required language stages are general places, resource ownership and
automatic cleanup, followed by task composition and suspension lifetimes before
async syntax. Generic trait parameters can follow concrete library needs. See
[RFL networking and general-purpose async](../networking/rfl.md) for the current
library contracts, limitations and acceptance targets.

This plan is preparatory language work and does not block the C DHCP/TCP/HTTP
milestones. Implementation should be coordinated with the other agent's ongoing
compiler work rather than changing those sources as part of documentation.
