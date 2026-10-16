# High-level RFL networking and general-purpose async

**Status: polling foundation implemented; general-purpose async remains in progress.**
Static traits and associated types are implemented. `std/task`, `std/future`, and
`std/executor` provide a first executable polling layer. There is no async syntax,
automatic cancellation on arbitrary scope exit, network reactor, or RFL socket
wrapper yet. The C DHCP/DNS/TCP/HTTP milestones are already implemented; see the
[networking roadmap](roadmap.md).

**Saved checkpoint: 16 October 2026.** Work is parked at the user's request to
switch tasks. See the [roadmap checkpoint](roadmap.md#saved-checkpoint--16-october-2026)
for implemented files, recorded validation and the exact resume point. The next
language milestone is general place borrowing and resource ownership followed
by automatic deterministic cleanup.

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
General heterogeneous spawn storage, join handles, nesting/re-entry policy and
an event-aware runtime idle hook remain separate implementation work.

## Required path to pleasant async

These are required milestones, not optional polish after syntax:

1. **General places and ownership.** Support borrowing fields, indexes and child
   future state directly. Specify which resource/future values have identity,
   which can be copied, and which require an explicit move or clone. Implicitly
   copying an async state machine must not duplicate a socket operation, result
   delivery or cleanup obligation. Existing ordinary value/COW semantics must
   remain intact. The callback prototype's shared terminal flag is not a complete
   language-level ownership model.
2. **Automatic deterministic cleanup.** Introduce an owned-resource/future
   cleanup mechanism with precisely specified copy/clone behavior. It must cover
   scope exit, assignment/replacement, return, `?`, break/continue, container
   destruction and cancelled parent frames. Release children and captured owners
   exactly once, including never-polled futures and partial initialization.
   A plain Drop method on freely copied structs would double-close resources;
   settle ownership before adding such a hook. Cleanup is bounded and cannot
   await; graceful async close is a separate explicit operation.
3. **Composition and task ownership.** Add join/race/timeout with owned child
   states, cleanup of losing branches, deterministic tie handling, and no
   accidental copies of pending children. A bounded task registry owns each
   spawned frame; handles carry generation-checked identity. Structured task
   scopes cancel and reclaim children on exit. Detachment must be explicit.
   Completed resource-valued outputs discarded by a race/error need normal
   automatic resource cleanup too.
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

At the saved checkpoint, the full RFL release suite passed **271 tests**.
The eight new polling/inference tests include seven runtime cases exercised on
native and RVM. The example above also ran on both targets with identical output.
These are recorded results from the completed milestone, not a claim that async
syntax, automatic scope cleanup or a production network reactor has been tested.

`rfl/tests/futures.rs` tests the shipped modules on native and RVM, with fake
clock/event sources. Include coalescing, wake-during-poll, stale/reused slots,
capacity limits, lazy timers, clock wrap, pending work without repeated polling,
reactor stop, cancellation, unrelated output types and alternating scheduling.

Before general async, add ownership/cleanup regressions for all scope exits,
never-polled/dropped futures, nested cancellation, discarded successful resource
outputs, partial initialization and forbidden suspended borrows. Exercise two
requests where one completes while the other waits, then cancel and reuse every
resource. Test resource exhaustion and code size, not only successful output.

The network acceptance target remains RFL DNS/ping, HTTP client and the same
hardcoded-HTML server reached by an ordinary host browser, using guest TCP/IP.
Record native and RVM separately. Keep BF compatibility; use small explicit BF
checks where practical rather than requiring slow network runs each iteration.
TLS, rOS scheduling and runtime argument delivery remain separate projects.
