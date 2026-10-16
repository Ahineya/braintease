# High-level RFL networking: design direction

**Status: planned exploration after the C HTTP client/server milestones.** No
RFL networking module, future contract, executor or `async`/`await` syntax is
introduced by this document. The [networking roadmap](roadmap.md) records the
implementation order; the existing [traits proposal](../../rfl/traits.md) remains
a language draft.

The goal is pleasant RFL code for DNS, UDP, TCP and HTTP while reusing the guest
C protocol stack in `libruntime.par`. Native, RVM and RBF should observe the same
network behavior. Keep raw Ethernet on DEVICE_* and ordinary VM instructions;
host sockets must not replace guest TCP/IP for the native target.

## Recommended order

| Step | Deliverable | Dependency |
|------|-------------|------------|
| Concrete bridge | Typed addresses, errors, context/connection ownership and explicit polling operations over the C library | Working C APIs and a reviewed RFL/C boundary |
| Traits | Implement and validate the existing static trait system; use it for shared I/O interfaces | Generic checking, coherence, module resolution and ownership foundations |
| Associated output types | Extend the language design so an awaitable can expose its result type through a trait | Working static traits; projection/constraint rules and diagnostics |
| Polling I/O design | A consistent contract for pending work, partial reads/writes, EOF, deadlines and cancellation | Concrete operations; traits for generic stream consumers |
| Cooperative executor | Bounded tasks, explicit network/timer progress and a defined wakeup policy | Polling and resource-lifetime contracts |
| Async language support | State-machine lowering and an agreed awaitable result-type model | Executor plus suspension/cleanup semantics |
| High-level API | RFL DNS/ping, HTTP client and hardcoded-HTML HTTP server examples | The preceding layers and the working C HTTP stack |

The recommended public abstraction order is **static traits → associated output
types → polling I/O → executor → async/await → high-level networking**. Concrete C bindings can be prototyped
without waiting for traits, and the C networking milestones do not depend on
this language work. Do not add async syntax merely to make the first wrapper
call convenient.

## Why traits first

The draft uses static dispatch and monomorphization: trait methods become
ordinary calls, with no required vtable, trait object or ISA change. This is a
good fit for a small VM and the BF target. Generic I/O consumers could work
with a TCP stream, memory buffer or deterministic test stream through common
capabilities, rather than embedding TCP knowledge in every parser or formatter.

Implement the trait registry, implementation validation, explicit generic
bounds, module visibility, coherence and qualified method calls correctly.
Generic method selection must preserve the meaning of declared bounds when
specialized; it must not change because a concrete type has an inherent method
with the same name. The first complete milestone is [Printable-based library printing](../rfl/traits-printing.md):
replace compiler-selected print/println behavior with generic library functions,
validate user-defined and nested printable types, and prove diagnostics and
module resolution before networking relies on traits.

The names `Read`, `Write` and `Flush` are useful design vocabulary, not committed
standard-library declarations. Start with a small fixed I/O error/progress
vocabulary and shared bounded helpers. Review code size and specialization
cost, especially for BF, instead of assuming static dispatch makes every
abstraction free.

## What the current trait draft does not settle

[The draft's exclusions](../../rfl/traits.md#18-explicitly-excluded-features)
defer **associated types and generic trait parameters**, as well as trait
objects, default methods and supertraits. A generic awaitable that can return
arbitrary result types therefore needs an additional design decision. Do not
present a trait with an associated `Output` type as already supported by v0.2.

The recommended extension is **associated types**: each future implementation
chooses one output type, and generic consumers can name that output through the
trait. Implement the draft's static trait foundation first, then specify type
members, implementations, projections and any output-equality constraints the
async API needs. Test these across generic code and modules, with missing or
incompatible definitions reported at compile time. Syntax and constraints need
an explicit amendment; they are not already part of draft 0.2.

**Generic trait parameters** are useful for capabilities parameterized by an
input type, such as conversion or request-handler interfaces. They deserve a
separate extension, but async does not by itself require both parameters and
associated types. Require a concrete library use case rather than making every
trait feature a prerequisite for the first future. If introduced, implementation
identity and overlap checks must include trait arguments, not just the trait
name and receiver type.

A parameterized future trait is an alternative output model, not a second
mandatory layer on top of associated output types. Prefer one clear contract
initially. The fixed-result polling I/O prototype can still precede these
extensions; general futures must not silently rely on absent language features.
Generic associated types, dynamic trait objects and advanced lifetime bounds
are not automatically included by adding ordinary associated types.

Neither extension supplies suspended-borrow safety, stable task storage or
cancellation cleanup. Those remain separate semantic requirements for async.
The async proposal should make those guarantees explicit before syntax is
implemented. This networking plan records the direction without rewriting
`traits.md` or changing the compiler during concurrent work.

Static traits also do not automatically provide storage for heterogeneous tasks.
An executor needs an explicit representation, such as a bounded closed set of
task variants or internal poll/cleanup handles with well-defined ownership.
Document its dispatch and allocation costs; do not make public trait objects
an accidental prerequisite of basic async networking.

## Concrete bridge and ownership

Wrap the existing C functions and handles behind RFL modules. Keep packet
serialization, ARP, DHCP, TCP state and retransmission in the shared stack.
Use RFL types for IPv4 addresses, endpoints, validated octet buffers, errors,
connections and listeners. Managed RFL collections are not C ABI buffers;
define their marshalling and ownership explicitly. One wire octet still maps
to one compatibility cell, including for native code.

Separate a borrowed view of data valid during a poll/callback from an owned
buffer that survives suspension. In particular, the current C UDP callback
payload is valid only during that callback; an async operation cannot retain
its pointer after returning. Decide where data is copied or owned, and how
receive-buffer reuse interacts with a pending read.

Keep C-facing calls synchronous at their ABI boundary. An RFL operation may
remain pending between calls; that does not require keeping a C call frame
suspended or crossing the C ABI with a trait/future object. Audit callback
re-entry into RFL's existing driver before using it; polling through a private
bridge is a simpler initial integration path.

Define who owns a context, UDP binding, TCP connection and listener, and when
each is closed or cancelled. Do not assume ordinary struct destruction already
provides user-defined resource cleanup. Review actual reference/place, borrow
and cleanup support against the
[implementation audit](../../rfl/IMPLEMENTATION.md#remaining-specification-gaps)
when language work starts; the audit records gaps, not future guarantees.

## Polling before syntax

A pending result means the operation needs further network progress, timer
progress or capacity; it is neither EOF nor an error. Partial reads and writes
are normal. A library must expose completion, pending, EOF, cancellation,
timeout and failure distinctly, and document whether a write completion means
buffer ownership transferred, bytes entered a transport buffer, or bytes were
acknowledged by the peer. Avoid one ambiguous success result for all three.

Define these contracts first for concrete operations and test streams. Then
run multiple operations with a small cooperative executor that pumps the shared
NIC and protocol timers. A pending network request must not block every task.
The current mailbox provides frame polling, not a ready-made future wakeup API;
design how queue changes, timer deadlines and available send space make tasks
runnable. A bounded polling prototype is reasonable, but measure idle work and
avoid an unbounded busy loop on BF.

A single-thread executor is enough for the first milestone. It needs no host
thread per connection, OS scheduler, rOS daemon or new instruction. Preserve
mailbox transaction serialization, fairness between ready tasks and explicit
time semantics. Live tests must still respect real remote deadlines.

## Async/await design questions

Treat async as resumable state machines lowered to ordinary shared IR. RFL
already has resumable activation machinery, but that is not proof that escaping
tasks, suspended borrows or cancellation are safe. Resolve at least:

- Is an operation started when constructed, first polled or explicitly spawned?
  What happens to an operation that is never awaited?
- Which locals and owned values survive a suspension? How are reference counts,
  resource owners and cleanup represented without duplicated destruction?
- Can references or mutable borrows cross an await? How is concurrent mutation
  prevented, and are suspended objects movable? Prefer a deliberately restricted
  first version over an unimplemented lifetime promise.
- How do cancellation, timeout, error propagation and early return release UDP
  ports, TCP state and buffers exactly once? Define behavior during partial I/O
  and normal close versus forced cancellation.
- Who advances the NIC and timers, what wakes pending tasks, and how are task
  count, memory, scheduling fairness and idle CPU work bounded?
- Which awaitable/output contract is compatible with the chosen trait features,
  and how does it compose across modules and generic functions?

Specify these in a dedicated language/runtime design before finalizing syntax.
No particular `.await`, spawning, selection or error-propagation spelling in
another language is adopted implicitly here. Blocking convenience entry points
can drive the same polling engine for simple applications; async is not required
to support the first RFL DNS or HTTP call.

## Acceptance targets

- Concrete RFL wrappers reproduce the guest DNS/ping demonstration and UDP
  binary/empty payload behavior without a second protocol implementation.
- Trait-based I/O consumers behave identically with a memory/test stream and a
  TCP stream; missing bounds, overlapping implementations and ambiguous calls
  produce useful compile-time errors.
- Partial I/O, delayed readiness, peer EOF, queue pressure, timeouts, cancellation
  and repeated resource reuse are tested before adding syntax sugar.
- An executor can keep one request pending while another completes, advance
  timers with no incoming packets, and reclaim cancelled task/connection state
  with bounded memory and idle work.
- RFL runs an HTTP client and serves the same hardcoded HTML page to a normal
  host browser or `curl`, using the shared C TCP/IP stack.
- Native and RVM results are recorded separately. BF compatibility is retained
  with ordinary instructions and small explicit smoke tests where practical.

This work follows the C stack milestones, with traits as preparatory language
work. Runtime application arguments, TLS, broad concurrency and rOS integration
remain separate decisions; none should turn the first RFL wrapper into an
implicit rewrite of the compiler or protocol stack.
