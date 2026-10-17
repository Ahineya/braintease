# Async functions

RFL supports lazy, move-only `async fn` functions and postfix `.await` on both
Ripple and native targets. They implement the existing `std/future::Future`
contract, so they work with executors, combinators and bounded task scopes.
No VM instruction or host coroutine facility is required.

```rust
import { Future, sleep, yield_now } from "std/future";
import { TaskError } from "std/task";

async fn answer() -> Result<i16, TaskError> {
    yield_now().await;
    sleep(25)?.await;
    Ok(42)
}
```

Calling `answer()` returns `Async<Result<i16, TaskError>>`. It evaluates and
captures arguments immediately, but runs no part of the body until polled.
The written return type describes the eventual output; an omitted return type
means `unit`. Generic functions and associated output types are supported:

```rust
async fn forward<F:Future>(future:F) -> F::Output {
    move(future).await
}
```

Import `Future` from `std/future` in a module that uses `await`; an imported
alias works too. Await uses that trait's `poll` and `cancel` methods, including
when an inherent method has the same name. It consumes its future. Fresh
expressions transfer directly; use `move(local).await` for a named move-only
future. Ordinary callback futures keep their documented copy/shared semantics.

## Running and scheduling

Keep an ordinary `fn main` and drive the future through `block_on`, its budgeted
variant, or a task scope. Constructing a future does not spawn a task. Awaiting a
child forwards the parent's current Context/Waker; it does not allocate a new
scheduler slot or recursively schedule the parent from a wake callback.

A child that returns Pending must arrange a wake, just as a manually implemented
Future does. On the next poll, execution resumes at that await without
reevaluating the child expression or repeating earlier statements. A fresh
Context is supplied on every poll. Ready delivers its value once and execution
continues in the same poll, until another Pending or the function's return.
There is no implicit preemption: a long synchronous loop still needs a
`yield_now().await` to cooperate.

Await works inside expressions, conditions, loops, branches and match arms;
ordinary left-to-right argument evaluation and snapshots remain intact. Return,
`?`, break and continue retain their normal ownership and cleanup rules.

The complete [async timer example](examples/async-timers.rfl) runs a yielding
String producer and a real timer in one task scope:

```sh
target/release/rfl run rfl/examples/async-timers.rfl
target/release/rfl run rfl/examples/async-timers.rfl --target native
```

It prints `hello from async RFL` and `42`. See the
[reactor contract](../docs/networking/rfl.md#real-time-reactors) for host time,
idle bounds and service callbacks. A network reactor/socket API remains separate
work; async syntax does not install networking or background progress.

## Ownership, suspension and cancellation

`Async<T>` is move-only, including when T is an ordinary scalar. Products and
collections containing it are transitively move-only. Handles may move between
ordinary locals, return from functions, or be dropped out of construction order.
Their storage remains stable. Monomorphic function values refer to the lazy constructor,
not to the body.

Safe references cannot be captured as async parameters, returned as outputs, or
cross an await. Local borrowing before and after an await is supported; finish
the borrow's lexical block before suspending:

```rust
async fn work(value:Owned<i16>) {
    { let borrowed=value.borrow(); println(*borrowed); }
    yield_now().await;
    { let borrowed=value.borrow(); println(*borrowed); }
}
```

This first rule is conservative: even a reference whose last read precedes the
await must finish its block. References passed as earlier arguments to a call
are also protected while later arguments are evaluated. For example,
`consume(&value, operation().await)` is rejected. Field-sensitive loans and
references into a suspended frame are future work. Raw pointers retain their
existing unsafe lifetime and exclusivity obligations.

Dropping or cancelling a suspended future synchronously cancels its unfinished
child, then releases initialized local resources in their normal reverse lexical
scope order. An unpolled future drops its captured arguments without running its body.
Uninitialized later locals and already moved/dropped values are not destroyed.
After a child returns Ready, its fields are released without calling cancel.
After the parent returns Ready, delivered output belongs to its caller and
survives destruction of the future handle. Explicit cancellation is idempotent;
polling after completion or cancellation is an explicit runtime error.

Cleanup cannot await and must remain bounded. Executors still own task wake-slot
retirement, and custom futures still own operation-specific unregistration.
Fatal process faults/exits do not unwind frames.

## Compiler representation and current limits

The checker preserves the public `Async<T>` return separately from the body's T.
It uses a compiler-private ADT constructor containing an `ErasedOwner`, so the
existing resource checker enforces moves without a new copy loophole. The
lowerer emits an ordinary constructor and a separate resumable body procedure.

The frame has an independent heap allocation with an erased-owner header. It
never keeps a bookmark into the synchronous LIFO call arena. Ordinary calls
made during a poll still use that arena and return before Pending. Entering an
async body returns its frame to the same driver, rather than recursively running
another trampoline. Pending and Ready write directly into their caller's Poll
slot. Deep recursive awaits do not grow the hardware polling stack, and results
need no source-sized stack buffer. Adding an output type does not copy every
reachable function's dispatch arm.

Await stores its child in an Owned allocation with statically selected Future
cancellation glue. Resume IDs and initialization/clearing of managed slots select
safe cancellation paths. Pending boundaries record the active resource cleanup
order; a final managed-slot sweep releases snapshots and partial expressions.
Internal poll-context/reference slots are cleared before suspension. Existing
scalar liveness analysis publishes values needed after the resume point.

This initial layout reserves all function slots and does not yet pack mutually
exclusive states or retire every dead managed snapshot at suspension. Frame
allocations retain existing machine-width/allocator limits; a function can span
code banks. Resume metadata reserves one terminal value and diagnoses state
counts beyond its range.

Async blocks/closures, async methods, borrowed async parameters, general
self-referential frames, pinned views, and automatic executor entry are not
implemented. Move-only closure captures remain separate work. The next compiler
refinements should improve frame liveness/layout and diagnostics without weakening
cancellation or borrow guarantees.

The recursive-await regression exercises 6,000 levels on native and a bounded
512 levels on RVM. Polling uses the same driver at every depth. The current
[guest allocator](../runtime/malloc.c) scans the complete block list for both
allocation and free: many simultaneously live frames therefore add quadratic
allocator work. Better free-list indexing or frame pools are a performance
follow-up; this implementation makes no allocation-free or throughput claim.

## Validation

The complete RFL release suite passes 349 tests, including eleven async
regressions and two new editor checks:

```sh
cargo test --release -p rfl --tests --quiet -- --test-threads=4
```

Execution regressions cover native and RVM, and the example above runs through
both CLI targets. IntelliJ lexer/run checks and LSP smoke, traits and associated
types protocol checks pass. The suite retains its bounded BF resource smoke
check; a full async execution on RBF has not been validated.
