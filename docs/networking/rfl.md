# High-level RFL networking and general-purpose async

**Status: polling, child-state borrowing, resource ownership, owned composition and task scopes implemented;
general-purpose async remains in progress.**
Static traits and associated types are implemented. `std/task`, `std/future`, and
`std/executor` provide a first executable polling layer. There is no async syntax,
network reactor or RFL socket wrapper yet. Explicit `Owned<T>` resources now
run synchronous cleanup at scope exit; `own_future`, `ready_owned`, `join`,
`race` and `timeout` adopt that ownership mechanism. The C DHCP/DNS/TCP/HTTP milestones are already implemented; see the
[networking roadmap](roadmap.md).

**Resumed: 16 October 2026.** Field and indexed-element borrowing now lets parent
futures poll child state in place. See the [roadmap checkpoint](roadmap.md#saved-checkpoint--16-october-2026)
for the original polling milestone. Move-only `Owned<T>` resources and deterministic scope cleanup now provide the
next foundation. Owned composition and bounded task lifecycles are implemented.
Async syntax remains deferred until suspension and cancellation semantics are
implemented.

The destination is pleasant, general-purpose async RFL, including networking,
timers and other event sources. Explicit cancellation is an implementation-stage
API, not a requirement that applications manually clean up every abandoned await.

## Implemented library contract

```rust
export type Poll<T> = Pending | Ready(T);
export trait Future {
    type Output;
    fn poll(&mut self, cx: &mut Context) -> Poll<Self::Output>;
    fn cancel(&mut self);
}
```

`Output` has no implicit Printable, error, or scalar bound. A future may return
any supported owned value, commonly `Result<T, E>`. Operation failures belong in
that output. Executor stop, clock errors and scheduling-budget exhaustion belong
in `RunError`; they do not pretend to be the future's arbitrary output type.

Construction is lazy: irreversible work starts on first poll. A poll performs
bounded work, never blocks, and returns Pending only after arranging a future
wake (I/O, a deadline, or a cooperative self-wake). Spurious wakes and polls while
pending are allowed. Ready delivers the result once. Polling a completed or
cancelled operation is a protocol error. Custom implementations must enforce
this contract. `poll_fn` supplies a checked callback wrapper that reports an
explicit runtime error on repeated terminal polls. Specify `poll_fn<T>` explicitly
when callback output inference needs a type; `ready(value)` infers from its input.

Cancellation is synchronous, bounded and idempotent, including before the first
poll. It must unregister operation-specific waits and release external state;
it cannot retract already transmitted bytes. It takes no Context: future scope
cleanup must not require a borrowed executor or a live mutable polling frame.
The executor revokes task wakeups before invoking cancellation. Completed futures
are not cancelled, and delivered outputs remain the caller's responsibility.

`ready(value)`, `yield_now()` and `sleep(milliseconds)` are supplied. Sleep starts
its duration at first poll and rejects intervals above 2^31-1 milliseconds.
CallbackFuture copies alias one operation's terminal state, rather than start
independent operations. Ordinary captured values remain retained while callback
owners remain alive, including after cancellation; external cleanup belongs in
the explicit cancellation callback. This is a prototype representation, not
compiler-generated async-frame storage or a user-defined destruction mechanism.

## Wakeup and timer registry

`task_set(capacity, now)` creates 1..1024 bounded slots. Allocation returns an
initially runnable Waker or an explicit capacity error. Wakers carry a slot's
shared dispatcher and generation; copies alias the same wake capability.

- Wake sets a ready bit. Multiple wakes coalesce.
- The executor takes that bit before polling. A wake during poll therefore
  survives for another turn; waking never recursively calls poll.
- Retiring a slot clears readiness and timers. Old generations cannot wake,
  retire or arm a reused slot. Exhausted generations retire permanently.
- Each slot tracks its earliest requested deadline. Multiple registrations use
  the earliest; extra timer wakes are permitted. Retiring the task removes its
  timer, including on cancellation. Child operations sharing one task may cause
  harmless spurious timer wakes after one child finishes.
- Clock readings wrap at 32 bits. Consecutive readings must advance by at most
  2^31-1 milliseconds. Backwards/ambiguous jumps are errors. All deadlines must
  lie within the half-range comparison window.

TaskSet copies share the registry. The implementation uses existing closures
with shared scalar captures, one allocation group per slot. Wake does constant
work; allocation, ticking and finding the next deadline scan the bounded slots.
No reference to a movable RFL local is retained. No public trait objects, raw
pointers, new instruction, interrupt or OS thread are needed. Representation and
specialization costs must be measured before scaling the scheduler.

Waker `take_ready` and `retire` are executor-level operations; futures normally
use `wake`, `wake_at` and `is_live`. Low-level callers own the obligation to retire
allocations. TaskSet has no automatic resource finalizer yet. Wakers retain
slot state safely even if the TaskSet value is released; that alone does not
implicitly retire the slot.

## Executor and reactor

The executor schedules futures. A Reactor advances external work and supplies
a wrapping monotonic millisecond clock:

```rust
export trait Reactor {
    fn now(&self) -> u32;
    fn turn(&mut self, wait: Option<u32>) -> bool;
}
```

Each executor turn checks its optional turn budget, calls the reactor, validates
time, then polls runnable roots. Some(0) means do not sleep; Some(n) is an upper
bound on idle waiting, not an instruction to ignore incoming events for n ms.
None means no task timer is registered, not permission to stop servicing I/O or
protocol maintenance. A reactor must still observe its own deadlines and stop
requests. Returning false requests orderly cancellation.

`block_on` drives one borrowed future to completion or reactor stop.
`block_on_with_budget` additionally limits turns, including idle turns.
`join_with_budget` schedules two roots with unrelated output types, alternating
poll order. Each root runs at most once per turn; completed roots are retired
immediately and are not polled again. A pending root cannot starve its peer or
the reactor. Every normal error path retires all slots and cancels unfinished
roots exactly once through the future contract. A zero budget cancels before
polling. Fatal process faults are not exception unwinding.

The supplied tests/examples use deterministic reactors. No production clock,
network service, sleeping or event-aware wait is silently installed. A future
that runs forever inside poll cannot be preempted by a turn budget.
The owned task scopes below add heterogeneous spawn storage and typed join
handles. An event-aware runtime idle hook remains separate implementation work.

## Borrowing child state in place

Explicit borrows now accept named paths such as `&parent.child`,
`&mut parent.child`, `&mut children[index]`, and nested combinations. Inherent
and trait methods use those same places for automatic receiver borrowing:

```rust
fn poll(&mut self, cx: &mut Context) -> Poll<String> {
    match self.timer.poll(cx) {
        Pending => Pending,
        Ready(_) => Ready("finished"),
    }
}
```

This addresses the stored child directly, including through a parent reference;
it does not poll a copied child. The runnable
[child-futures example](../../rfl/examples/child-futures.rfl) demonstrates a parent
waiting for its timer and forwarding cancellation. Run it with
`target/release/rfl run rfl/examples/child-futures.rfl` and optionally
`--target native`. It uses a fake clock and prints the greeting followed by `125`.

Vector and fixed-array elements support shared and mutable borrowing; owning
slice elements support shared borrowing only. Mutable vector projections detach
each shared ancestor before exposing an element, retaining managed elements in
the copied buffer. Existing copies and owning slices retain their old values.
Indices evaluate once, left to right; each bounds check finishes before a later
index runs. The root is protected against writes while indices are evaluated.
Shared reads such as `&mut values[values.len()-1]` remain valid.

Borrow checking remains conservative: a child loan protects the whole named
root, so two mutable loans to distinct fields of that root are rejected. A local
loan ends at its lexical block boundary; call-only loans end after the call.
Reborrowing through an existing reference also freezes that reference's access
capability. References still cannot escape a block, be returned, captured by a
closure or stored in products/collections. String indexing returns a decoded
character and cannot expose a borrowed character cell. Explicit borrows of
rvalues remain rejected; shared method receivers may use a temporary for ordinary
values. A move-only receiver must first be bound to a named owner.
Generalized assignment syntax and field-sensitive lifetime analysis are separate
work. None of these references may yet cross an async suspension point.

Forward struct layouts are also normalized inside callable signatures and ADT
payloads; storing the existing `CallbackFuture` in a parent struct now uses the
same final Context layout as ordinary polling calls.

The borrowing milestone adds storage access. The resource milestone below adds
identity and cleanup through an explicit owner. Ordinary polling structs and
unwrapped callback futures retain their existing copy semantics.

## Move-only resources and deterministic cleanup

`Owned<T>` is the first resource type. It owns stable heap storage for `T` and a
synchronous `fn(&mut T) -> unit` cleanup callback:

```rust
let mut file = owned(handle, fn(handle: &mut i16) => { close_handle(*handle); });
{
    let handle = file.borrow_mut();
    use_handle(*handle);
}
let next = move(file); // file is now unusable
// next's cleanup runs automatically at this block's exit.
```

`close_handle` and `use_handle` above stand for application functions; no file
wrapper or network handle API is implied. `.borrow()` yields `&T` and
`.borrow_mut()` yields `&mut T`. The existing lexical rules protect the owner
from moving, replacement and conflicting access until the loan ends. References
cannot escape or cross a suspension point.

Ordinary values keep copy/COW semantics. Structs, tuples, ADTs, fixed arrays and
vectors transitively containing an owner are move-only too. Transfer a named
value with `move(name)` when passing, returning or storing it. Fresh expressions
already transfer their result. Explicit moves also consume ordinary values;
`drop(move(value))` releases a value immediately. Moved bindings can be assigned
a fresh value; reading or borrowing them before reinitialization is an error.
Control-flow checking merges branch states and checks repeating loop backedges.
Generic specialization checks resource-containing arguments, and unused generic
declarations are checked when they explicitly mention `Owned<T>`.

Cleanup runs on lexical block/function exit, replacement after evaluating the
new value, return, `?`, break/continue, discarded values, partial construction and
container destruction. Local owners release in reverse creation order. Aggregate
fields and container elements release in declaration/index order. A cleanup
callback runs before normal payload destruction; nested owners then release
normally. Temporaries may retain internal snapshots while an expression is being
evaluated; they do not grant a source-level clone capability. Cleanup is
synchronous, cannot await, and must do bounded work. Fatal process faults/exits do
not unwind owners. Graceful asynchronous close remains a separate operation.

The [owned-futures example](../../rfl/examples/owned-futures.rfl) demonstrates a
never-polled future, a pending parent with an owned child, and a completed future.
A concrete cleanup callback delegates to the checked callback future, whose
terminal state makes cancellation inert after completion or prior cancellation. The existing runner still
retires wakeups before cancellation. Run:

```sh
target/release/rfl run rfl/examples/owned-futures.rfl
target/release/rfl run rfl/examples/owned-futures.rfl --target native
```

This is explicit ownership infrastructure, not a blanket conversion of every
`Future` implementation into a resource. `CallbackFuture`, TaskSet and Waker keep
their documented prototype/shared semantics. The owned adapter described below provides resource semantics explicitly;
there is no resource `clone`/`share` API yet. Copyable callbacks cannot capture
move-only owners. Owning slices of resource elements, `get()`/indexed copies of
resources, partial field/index moves and consuming matches with guards are
rejected. Move a whole product and destructure it, borrow an indexed element, or
use vector `pop()` instead. Borrowed match patterns currently provide shared
views, including for an `&mut` scrutinee; they do not provide mutable ADT fields.

## Owned future composition

[`std/future_owned`](../../rfl/std/future_owned.rfl) supplies
`own_future(future) -> OwnedFuture<F>`. The adapter is move-only even when `F`
is copyable. It polls `F` in stable `Owned<F>` storage and automatically calls
`F.cancel()` when abandoned, including before first poll. Explicit cancellation
is idempotent. After Ready, it destroys `F` and its fields without calling
`F.cancel()`; the output transfers to the caller. Re-polling a terminal adapter
reports the same protocol error as `poll_fn`.

`ready_owned(value)` uses that adapter and can hold a move-only output. An
unpolled/dropped ready future releases the value; a successful poll transfers
it once. Ordinary `ready` and `CallbackFuture` keep their prototype copy/shared
semantics. Wrapping one existing callback alias does not revoke other aliases;
create fresh operations when independently owned work is required. Copyable
closures still cannot capture resource owners. Nested closure captures also have
existing checker limits; a separately bound cleanup callback can be captured by
a polling callback when it produces a fresh owned output.

[`std/combinators`](../../rfl/std/combinators.rfl) supplies ordinary Future
implementations with no executor-specific scheduling magic:

| Constructor | Output and polling rule |
|---|---|
| `join(left, right)` | `(A::Output, B::Output)`; polls pending children once each, alternating order across polls, and caches each completed output |
| `race(left, right)` | `Either<A::Output, B::Output>` (`First` / `Second`); left is polled first and wins ties; right is not polled if left is ready |
| `timeout(work, milliseconds)` | Construction returns `Result<Timeout<F>, TaskError>`; polling yields `Result<F::Output, TimeoutError>` (`Elapsed`) |

All three consume their children and are move-only. Use `move(child)` for a
named resource value. Dropping or cancelling a composite cancels unfinished
children and releases any cached resource outputs. A race cancels its loser
before its caller receives Ready. Completed children are never polled or
cancelled again. Distinct output types and nested composites are supported.

Timeout polls work before its timer, so ready work wins a deadline tie (including
zero duration). Its clock starts when the timer is first polled. Duration limits
and wrapping arithmetic match `sleep`; invalid construction still consumes and
cancels the supplied work, without polling it. Dropping an unpolled timeout
also cancels its work.

Children share the parent's Context/Waker. A child wake schedules the parent,
and pending siblings may receive spurious polls. A completed/losing child's
retained parent wake capability or earliest timer may still cause an extra
parent poll; it cannot resurrect that child. Retiring the root revokes the
capability. Operation-specific I/O registrations must still be removed by the
child's cancellation implementation. This layer adds no per-child wake registry,
spawned tasks, real clock or network reactor.

Two ownership operations support library state machines:

- `std/mem::replace(&mut place, value)` installs the new value and returns the
  old value, transferring ownership without dropping either. The mutable place
  is evaluated before the replacement, with normal loan, bounds and COW rules.
  `take(&mut option)` replaces an Option with None.
- `move(owner).into_inner()` transfers the payload of `Owned<T>` while skipping
  **only that owner's cleanup callback**. Its cleanup environment is released;
  the payload's nested resources remain owned by the result. This is an explicit
  disarm operation: use it only after handling the external resource obligation
  (for example, a future already delivered Ready). It does not close a file or
  cancel an unfinished operation for you. Existing borrows must end first.

The [composed-futures example](../../rfl/examples/composed-futures.rfl) joins a
String and timer, races different output types with a resource-valued loser,
and times out slow work. Both commands use a deterministic clock:

```sh
target/release/rfl run rfl/examples/composed-futures.rfl
target/release/rfl run rfl/examples/composed-futures.rfl --target native
```

It prints `hello from composed futures`, `125`, `loser released`, `42`, and
`timed out; pending work cancelled` on separate lines.

## Bounded task scopes and typed joins

[`std/scope`](../../rfl/std/scope.rfl) supplies `task_scope(capacity, now)` and a
move-only `TaskScope`. Pass the reactor's current clock reading; capacity uses
the same 1..1024 limit as TaskSet. The scope owns each task's stable future
storage, independent wake slot and cancellation obligation. Different future
and output types can share one registry.

```rust
let mut scope = match task_scope(2, clock.now()) {
    Ok(scope) => move(scope), Err(_) => { return 1; },
};
let mut answer = match scope.spawn(ready_owned(42)) {
    Ok(handle) => move(handle), Err(_) => { return 2; },
};
match scope.run_with_budget(&mut clock, 10) {
    Ok(_) => {}, Err(_) => { return 3; },
}
// answer implements Future with Output = Result<i16, JoinError>.
match block_on_with_budget(&mut answer, &mut clock, 1) {
    Ok(Ok(value)) => println(value), _ => { return 4; },
}
```

Import constructors/functions normally; this snippet assumes a Reactor value
named `clock`. [`examples/task-scopes.rfl`](../../rfl/examples/task-scopes.rfl)
is the complete runnable example, using a deterministic clock. `JoinHandle<T>`
and `JoinError` (`TaskCancelled`) are exported by `std/scope_state`; applications
normally infer handle types. That module's other names support the implementation
and are not an application API.

- `spawn(future)` returns `Result<JoinHandle<F::Output>, SpawnError>`.
  `ScopeClosed` and `TaskLimit` are explicit errors. Failed spawning releases the
  supplied work without polling it. Named resource futures require `move`.
  Existing aliases to copyable CallbackFuture inputs keep their documented
  prototype semantics; use fresh operations or move-only adapters.
- `turn(&mut reactor)` services one bounded turn, returning true when no tasks
  remain and false while tasks are pending. This lets application code spawn
  additional work between turns. `run` services until idle or reactor failure;
  `run_with_budget` also bounds turns. All use the Reactor clock/wait contract.
- Each ready task is polled at most once per turn; the scan's starting slot
  rotates. Readiness is cleared before poll, preserving wake-during-poll.
  Idle pending tasks are not repeatedly polled. Completed frames and their
  slots are reclaimed immediately, while results remain in join storage.
- A join handle is a move-only Future yielding `Result<T, JoinError>` exactly
  once, including resource-valued T. Pending polls register the current joiner's
  waker. Completion or scope cancellation wakes that joiner. A handle can itself
  be spawned as a task or used in join/race/timeout composition.
- Dropping/cancelling a join handle releases its cached result, unregisters its
  join waiter, requests cancellation of the underlying task and immediately
  retires that task's wake token. The scope still owns the task frame: it runs
  synchronous cancellation and frees that frame before another poll, on the
  next `turn`, `spawn`, `active` or `close`. It is a cancellation request, not
  implicit detachment. No detach operation is supplied in this version.
- `close()` and ordinary scope destruction synchronously retire **all** child
  tokens before cancelling and reclaiming unfinished tasks. Close is idempotent;
  further spawning fails. Pending surviving join handles yield TaskCancelled.
  Already completed outputs remain owned by their handles and can outlive the
  scope. Dropping an uncollected handle releases its output normally.
- Reactor stop, invalid time and exhausted run budget close the scope before
  returning the corresponding RunError. A zero budget cancels unpolled tasks;
  an already empty scope succeeds without turning the reactor. An error does
  not discard results that completed before it.

Join-result storage has its own owner; it cannot be reused for another task.
Generation-checked wake tokens and per-task lifetime gates prevent old cancels,
wakes and copied dispatch callbacks from touching a reused slot or freed frame.
The gates outlive their typed allocation when a callback is retained. Recursive
poll/destruction of a busy task reports a protocol error; it does not create an
aliased mutable view. Handle polling and scope driving remain single-threaded.
There is no background progress, public trait-object ABI, suspended reference,
OS thread or new VM instruction. Starting another scope or block_on does not
implicitly service this scope; drive it explicitly or arrange a Reactor adapter.

The small compiler boundary is opaque `ErasedOwner`: `__erase_owned(move(owner))`
transfers an `Owned<T>` allocation into an opaque move-only owner while retaining
its original typed finalizer and payload destruction. Erased values cannot be
borrowed, cloned or disarmed with into_inner. Registry/result capsules use this
so safe pattern extraction cannot bypass their lifetime gates. The private
`__borrow_raw(pointer)` primitive requires an unsafe block and a mutable pointer;
its caller must prove live, initialized, correctly typed storage and exclusive
access for the loan. It neither keeps an allocation alive nor checks aliases.
Scopes keep those views internal and check their separate gate before access.
No safe helper accepting an arbitrary raw state pointer is exported.

This first representation allocates typed capsules and control closures per task
and scans the bounded registry. It is not an allocation-free scheduler or a
production network reactor. Construction is split across small functions to stay
within VM code banks; managed retains now share one runtime helper rather than
expanding identical overflow/null/refcount checks at every copy. Typed leaf
releases also share helpers so caller cleanup stays within code banks. Measure code
size and allocation/runtime costs before increasing scheduler scale.

Run the complete example on either target:

```sh
target/release/rfl run rfl/examples/task-scopes.rfl
target/release/rfl run rfl/examples/task-scopes.rfl --target native
```

## Required path to pleasant async

These are required milestones, not optional polish after syntax:

1. **General places and ownership.** Field/index/child-state borrowing is
   implemented as described above. `Owned<T>` and resource-containing aggregates
   now require explicit moves while preserving ordinary value/COW semantics.
   Future/socket libraries must use that ownership mechanism deliberately; the
   callback prototype's shared terminal flag is not an owned async frame.
2. **Automatic deterministic cleanup.** Implemented for explicit owners across
   normal scope exits, replacement, early returns, `?`, loop exits and containers,
   including partial initialization. The concrete future example proves
   cancellation of never-polled and nested pending work. Compiler-generated
   async frames and move-only captures remain work; a freely copied struct does
   not gain an implicit Drop hook.
3. **Composition and task ownership.** Owned join/race/timeout now provide child
   states, cleanup of losing branches, deterministic ties and cached resource
   outputs. Bounded task scopes now own heterogeneous frames and provide typed
   join handles with checked wake identity. Scope exit cancels and reclaims
   unfinished children; handle drop requests cancellation at the next service
   boundary. Detachment remains unsupported rather than implicit.
   Preserve the implemented automatic cleanup of resource-valued outputs
   discarded by a race or error.
4. **Async lowering.** Lower lazy async functions to owned state machines in
   shared IR. Keep generated locals/children alive only for required states;
   transfer results and emit state-specific cleanup on every exit. Reusing RFL's
   resumable call frames is not by itself a lifetime or cancellation proof.
   Initially reject borrows crossing suspension unless their ownership/stability
   is proven; extend to safe borrowed operations deliberately. Diagnose these at
   source locations instead of accepting unsafe hidden pointers.
5. **Usable library integration.** Supply clock/idle/network reactors and typed
   I/O, then DNS/TCP/HTTP applications. General formatting/writer traits, spawn
   ergonomics, selection and async syntax must share the same progress and
   cancellation semantics established by the polling tests.

Associated equality constraints and generic trait parameters can follow concrete
composition/I/O needs. Neither public dynamic dispatch nor a complete Rust-like
trait/lifetime system is an implicit prerequisite for the first executor.

## Network bridge and ownership

Keep Ethernet on DEVICE_* and protocol state machines in the existing guest C
stack in libruntime.par, including on native. Host sockets must not substitute
for guest TCP/IP. One octet remains one compatibility cell.

A network reactor owns stable C storage for contexts, DHCP, DNS, connections,
listeners and TIME_WAIT records. RFL wrappers carry checked handles; no active
C object may move with an RFL struct or COW buffer. An owned operation/stream
handle must keep its registry owner alive, so cancellation remains valid after
an executor stops. Reactor shutdown stops new work and cancels outstanding work;
storage is freed only after dependent owners are released. A generation check
alone is not permission to dereference a freed registry. C-facing calls are
synchronous and must not keep a suspended C frame or recursively re-enter RFL polling.
Callbacks record events for later task scheduling. UDP callback payloads must be
copied before the callback returns.

Drive net_dhcp_poll in place of net_poll for an active DHCP client, including
while bound, then DNS/TCP/listener polling. Advance timers without traffic and
use bounded packet work per turn. Reactor maintenance deadlines participate in
idle decisions alongside task timers. Wakers do not create a NIC wake mechanism:
begin with bounded periodic device checks, then measure an optional runtime wait
hook without changing the ISA.

The first I/O futures own their buffers across pending polls. Later borrowed
reads/writes require the suspension lifetime work above. Define results as:

| Operation | Completion |
|---|---|
| Connect | Handshake succeeded, returning an owned stream |
| Read | Owned data, explicit EOF, or error; pending is distinct |
| Write | Possibly partial prefix copied into guest TCP TX storage |
| Flush | Previously accepted prefix acknowledged by TCP, not necessarily processed by the remote application |
| Shutdown | Accepted writes drained and local FIN sent; reading can remain available |
| Close | Documented graceful completion/resource release, retaining TIME_WAIT protection |

Cancellation cannot roll back a partial write. Forced abort is distinct from
graceful shutdown and TIME_WAIT retention. Stale handles and conflicting owners
must fail explicitly rather than affect a reused connection slot. Configuration
and link changes retain the C stack's existing invalidation behavior.

A runnable deterministic example is [futures.rfl](../../rfl/examples/futures.rfl):

```sh
target/release/rfl run rfl/examples/futures.rfl --target native
target/release/rfl run rfl/examples/futures.rfl
```

It completes an immediate String future alongside a 25 ms fake-clock timer,
printing the result, clock reading and turn count. It performs no real sleeping
or networking.

## Validation and next acceptance targets

After the borrowing milestone, the full release suite passed **286 tests** using
`cargo test --release -p rfl --tests --quiet -- --test-threads=4`.
[`tests/places.rs`](../../rfl/tests/places.rs) adds 15 regressions: execution cases
run on native and RVM, covering child polling/cancellation, nested managed COW,
arrays/slices, index order and bounds, reference alias restrictions, unsafe C
pointer coercions, addresses across memory banks and finalized indirect layouts.
The child-futures example also ran through the normal CLI on both targets,
printing `hello from a child future` and `125`. This does not validate async
syntax or resource cleanup; the subsequent resource checks below cover the latter.

At the saved checkpoint, the full RFL release suite passed **271 tests**.
The eight new polling/inference tests include seven runtime cases exercised on
native and RVM. The example above also ran on both targets with identical output.
These are recorded results from the completed milestone, not a claim that async
syntax, automatic scope cleanup or a production network reactor has been tested.

`rfl/tests/futures.rs` tests the shipped modules on native and RVM, with fake
clock/event sources. Include coalescing, wake-during-poll, stale/reused slots,
capacity limits, lazy timers, clock wrap, pending work without repeated polling,
reactor stop, cancellation, unrelated output types and alternating scheduling.

The resource milestone adds `rfl/tests/resources.rs`: scope/loop exits,
replacement, return/`?`, managed payloads, nested callback frames, container
transfers, branch/loop rejection, partial construction, unused generic declarations,
never-polled/nested future cancellation and resource-valued executor outputs.
Execution cases run on native and RVM; one bounded compiled Brainfuck CPU check
verifies finalizer invocation. Ordinary move errors and owner method completions
also reach editor analysis.

The full release suite passed **303 tests** with
`cargo test --release -p rfl --tests --quiet -- --test-threads=4`.
The resource file contains 15 tests: ten native/RVM parity cases, four checking
cases with multiple rejections, and one compiled Brainfuck CPU smoke check.
Two additional editor regressions cover owner completions/navigation/move errors
and the nested cleanup signature/type token. The owned-futures example ran
through the normal native and RVM CLI and printed the same cancellation/poll
sequence and `42`; RVM also reported its unrelated sandbox storage warning.

The full release suite passes **317 tests** after owned composition, using the
same command above. Its 14 new tests include 13 native/RVM execution cases and
one checking case containing multiple ownership/borrow rejections. The composed
example also ran through the normal CLI on both targets with matching output.
The small Brainfuck replacement/extraction/finalization check passes too.

The owned-composition regressions in
[`tests/composition.rs`](../../rfl/tests/composition.rs) cover storage replacement,
COW and result aliases, explicit cleanup disarming, nested resources, unpolled
and completed adapters, resource-valued outputs, join caching/order, race ties,
loser cleanup, nested parent wakes, lazy/wrapping timeouts, runner errors,
terminal-poll errors and ownership/borrow rejections. Child address checks verify
stable payload storage across parent state moves; repeated extraction exercises
managed environments and containers. The existing small Brainfuck finalizer test
also exercises replacement and extraction, including the skipped cleanup hook.

The task-scope milestone passes **329 release tests** using the same full-suite
command. [`tests/scopes.rs`](../../rfl/tests/scopes.rs) adds 12 regressions covering
heterogeneous/resource outputs, cancellation before polling, pending and completed
joins, dependency cancellation, waiter wakes, fairness, capacity/reuse, stale
callbacks, runner errors and ownership/unsafe rejection. Execution cases run on
native and RVM. The task-scope example ran on both normal CLI targets with matching
output. The bounded Brainfuck resource smoke check additionally verifies opaque
owner finalization; this does not claim a full task-scope or network run on RBF.
`cargo check -p rfl` also passes. The examples still use deterministic clocks.

Before general async, extend these ownership/cleanup regressions for
never-polled/dropped futures, nested cancellation, discarded successful resource
outputs, partial initialization and forbidden suspended borrows. Exercise two
requests where one completes while the other waits, then cancel and reuse every
resource. Test resource exhaustion and code size, not only successful output.

The network acceptance target remains RFL DNS/ping, HTTP client and the same
hardcoded-HTML server reached by an ordinary host browser, using guest TCP/IP.
Record native and RVM separately. Keep BF compatibility; use small explicit BF
checks where practical rather than requiring slow network runs each iteration.
TLS, rOS scheduling and runtime argument delivery remain separate projects.
