# Lazy, typed effects

`std/effect::Effect<R,E,A>` is a **reusable recipe** requiring an environment R
and producing `Result<A,E>`. `run(program, environment)` creates an independent,
move-only `Async<Result<A,E>>` each time. Construction and composition run no user
callbacks. Execution starts on first poll or await; an unpolled future can be
dropped without starting its recipe.

```rfl
import {Effect,access,map,run} from "std/effect";
import {block_on} from "std/executor";
import {host_reactor} from "std/host";
import {Result} from "std/result";

fn main() {
    let recipe:Effect<i16,String,i16> = access(fn(n:&mut i16)=>*n+1);
    let recipe=map(recipe,fn(n:i16)=>{println(n);n});
    let mut first=run(recipe,41);
    let mut second=run(recipe,99);
    let mut reactor=host_reactor().unwrap();
    block_on(&mut first,&mut reactor).unwrap().unwrap(); // 42
    block_on(&mut second,&mut reactor).unwrap().unwrap(); // 100
}
```

See [the timer example](examples/effects.rfl). Both native and Ripple use the
same existing future/frame/ownership protocols; no ISA extension is needed.

## Recipes, exclusive programs and running futures

| Value | Ownership and execution |
|---|---|
| `Effect<R,E,A>` | Copyable recipe, with reusable `fn` factories and continuations |
| `OwnedEffect<R,E,A>` from `std/effect_once` | Move-only description with `once fn` captures; executes at most once |
| Future returned by either `run` | One owned execution, independently polled, completed or cancelled |
| `Shared<T>` from `std/shared` | Copyable handle to the same resource, released after its last owner |

Reusable does not mean pure or deeply cloned. Ordinary closures may share mutable
scalar captures, and shared handles intentionally preserve identity. A factory
must deliberately acquire fresh resources when independent resources are wanted.
RFL has no purity checker. Mutable capture boxing retains its existing scalar-only
restriction; update resource state through `with_shared` instead.

## Construction and composition

| Operation in `std/effect` | Behavior during each run |
|---|---|
| `succeed(value)` / `fail(error)` | Copies the captured value/error; requires `Copy` |
| `sync(make)` / `succeed_with(make)` | Calls `fn()->A` to construct a fresh success, including owned resources |
| `fail_with(make)` | Calls `fn()->E` to construct a fresh typed error |
| `attempt(work)` | Calls `fn(Environment<R>)->Result<A,E>` |
| `access(work)` | Calls `fn(&mut R)->A` with temporary exclusive environment access |
| `from_async(work)` | Calls the async Result factory, then awaits it |
| `from_future(work)` | Calls any Future factory, awaits it and wraps its output in Ok |
| `map(program,f)` / `flat_map(program,f)` | Transforms/sequences successful values; errors skip f |
| `map2(left,right,f)` / `zip(left,right)` | Runs left before right, transferring fresh results without copying |
| `sequence(programs)` | Iteratively runs existing recipes in source order |
| `traverse(values,f)` / `traverse_with(make,f)` | Defers ordered traversal; factory form supports fresh exclusive inputs |
| `traverse_owned_with(make,f)` | Transfers fresh inputs to consuming children that may retain them across awaits |
| `map_error(program,f)` / `catch_all(program,f)` | Transforms/recovers typed errors |
| `provide(program,value)` | Copies an R for this subprogram; requires `R:Copy` |
| `provide_with(program,make)` | Calls `fn()->R` on first poll for a fresh environment |
| `ensuring(program,finalizer)` | Registers a fresh synchronous finalizer on first poll |
| `scoped(acquire,use,release)` | Acquires a resource, runs use with a checked capability, releases it |

Ordinary argument evaluation is eager: `succeed(expensive())` evaluates
`expensive` during construction. Use `sync(fn()=>expensive())` to defer it.
`from_future` treats every output as success, including Result outputs: use
`from_async` when an async Result should become the effect's error channel.
Unconstrained R/E types sometimes need an annotation or explicit type arguments.

`std/value::Copy` is a compiler-derived marker for ordinary RFL copy/COW semantics,
not a bitwise-copy or deep-clone promise. Strings, ordinary collections and
recursive aggregates qualify if their stored fields qualify. Owned values,
consuming callbacks, futures and containers storing them do not. Reusable
function signatures and phantom type parameters do not own their argument/result
types. `Shared<Owned<T>>` and `Effect<Owned<R>,Owned<E>,Owned<A>>` are copyable.
Generic code must declare the bound; manual impls cannot bypass the check.

## Fresh, shared and transferred resources

Use a reusable factory to acquire an exclusive resource on each execution:

```rfl
let fresh:Effect<unit,String,Owned<i16>> = sync(fn()=>
    owned(42,fn(n:&mut i16)=>println("closed fresh resource")));
```

Use a shared handle when executions should return the **same** resource:

```rfl
import {Shared,share,with_shared} from "std/shared";
import {Socket,SocketError} from "std/socket";

fn socket_recipe(socket:Socket)->Effect<unit,SocketError,Shared<Socket>> {
    succeed(share(move(socket)))
}
// Repeated run(recipe,{}) results retain this same socket.
// with_shared(handle,once fn(socket:&mut Socket)=>socket.descriptor())
// takes temporary access while keeping an owner alive.
```

This socket adapter uses the guest OS build path (`rfl build --os fuzix|fuse-x`);
other owned resource types use the same sharing pattern.

`shared(value,cleanup)` transfers value into stable storage; `share(value)` uses
an empty extra cleanup callback. Every copied handle retains that storage. The
last handle revokes access, runs cleanup, then releases nested resources exactly
once. Release a named handle with `drop(move(handle))`; `drop(handle)` only drops
a copy. Recipes, closures, collections and running frames holding handles also
count as owners. When shadowing a recipe, use `let p=map(move(p),f)` to
transfer its old handle; `map(p,f)` also leaves the old binding alive until its
scope ends. The new recipe remains reusable. Hidden call snapshots end after return and lexical aliases end
at scope exit; they do not postpone last-handle cleanup until function exit. Implicitly borrowed temporary receivers conservatively
retain their owner through the enclosing lexical scope.

`with_shared(handle,once fn(value:&mut T)=>...)` retains an owner throughout its
synchronous exclusive visit. References cannot escape or cross await. Reentrant
access faults. A future can retain a handle and take fresh visits between polls.
This is a single-threaded ownership/access foundation, **not an async mutex**:
protocol operations sharing a TCP connection still need explicit serialization
or a queue for whole transactions. Sharing does not duplicate the connection or
its stream position. Reference cycles can leak; weak handles/cycle collection
are not implemented. Sixteen-bit refcount limits fail explicitly.

Use `std/effect_once::OwnedEffect` to transfer an already acquired exclusive
resource once. Its API retains consuming callbacks and unrestricted
`succeed(move(value))`, `fail(move(error))` and `provide(...,move(environment))`.
The reusable adapters are:

- `from_owned(fn()=>fresh_owned_program)` constructs a consuming child per run.
- `flat_map_owned(recipe,fn(value)=>owned_program)` lets each execution's fresh
  owned result move into a consuming continuation.
- `catch_all_owned(recipe,fn(error)=>owned_program)` does the same for fresh
  owned errors during recovery.
- `into_owned(recipe)` converts one execution to the consuming form.

The support operation `std/effect_once::start_in(program,environment)` starts a
consuming child using an existing checked environment capability. That capability
must remain live for the child; root `run` owns its environment. The adapters
invoke user factories only while polled.

## Environment access and cleanup

`run` owns one R throughout each execution, including owned R. `Environment<R>`
is a copyable, nonowning access capability. `with_environment(env,once fn(r:&mut R)
=>...)` permits synchronous exclusive access. An async adapter may keep the
capability across await and request a new access afterward; no borrowed reference
may survive suspension. After the run or scope ends, retained capabilities fault
instead of accessing freed storage. Missing provider visits and reentrant visits
also fault. `std/effect_env` contains the checked storage implementation; raw
borrowing remains unsafe. `SharedOwner` is an opaque compiler primitive retaining
typed destruction, with no payload borrow or `into_inner` operation.

Typed errors are distinct from cancellation, executor failures and runtime faults.
`catch_all` only sees Err. Once registered, ensuring finalizers run once on
success, typed failure, cancellation or pending destruction. Children cancel
before enclosing finalizers and environments release. Nested finalizers run from
inside out. Unstarted recipes and unpolled roots register no finalizers; ordinary
owned captures still release. Repeated cancellation is safe.

`scoped` calls use only after successful acquisition. Release runs after child
cancellation, before nested resource destruction. An escaping resource capability
expires at scope exit. Cleanup is synchronous; asynchronous finalizers require a
separate cancellation design. Runtime faults halt without promised unwinding.

Overlapping runs own separate frames/environments; cancelling one leaves the
recipe and other runs usable. TaskScope/join/timeout schedule these ordinary
futures. Parallelism and yielding remain explicit. The async driver and direct
await cancellation walk avoid host recursion for long sequential chains, without
promising constant space or arbitrary destructor-graph depth. Automated reusable
chain coverage uses 6,000 native stages and 128/256 interpreter stages, each
executed twice and cancelled twice. The interpreter checks instruction budgets
and requires doubling depth to cost less than 2.4 times as many instructions.

### Allocation performance

Instruction-by-instruction profiling of the 128-stage repeated-chain workload
found **93% of instructions in malloc/free alone**. Both scanned live blocks
from the heap's beginning. Relinking the same guest objects with the segregated
free-list allocator in `runtime/malloc.c` gives:

| Stages | Previous allocator | Current allocator | Reduction |
|---:|---:|---:|---:|
| 128 | 527,239,945 | 53,780,554 | 9.8× |
| 256 | 1,956,742,803 | 106,565,682 | 18.4× |

These are guest instructions for construction, two executions, two cancellations
and destruction, including retained recipe graphs. Doubling depth now costs
1.98× instead of 3.71×. These measurements concern this workload, not a general
effect-throughput guarantee. Native effects use the host allocator and do not
receive this guest-allocator speedup.

The allocator searches only free blocks, uses direct headers for free/realloc,
and coalesces through physical neighbors. Headers grow from five to eleven C16
cells; each live allocation trades six cells for avoiding whole-heap scans.
Size classes use comparisons/addition, with no new ISA or expensive bit-scanning
requirement. Large size classes can still search multiple free blocks; this is
not a constant-time allocation guarantee under arbitrary fragmentation.

The remaining profile is distributed across allocation, reference counting,
generated bind bodies and the frame driver. Pools and reducing intermediate
allocations remain candidates, but the quadratic live-block scans are removed.

Run the shared test fixture at other depths with:

```sh
python3 rfl/tools/bench_effects.py --depths 128 256 512 --check
```

This reports instruction counts and execution time separately from compilation;
`--report path.json` saves the measurements. Build release tools and the runtime
first. The benchmark's explicit value check adds a small constant cost compared
with the original objects in the table. A measured 512-stage fixture executes
212,192,064 instructions, continuing the near-linear growth.

## Generic FP and the roadmap

`Effect<R,E,_>` implements `Functor`, `FlatMap`, `FromFactory` and `Apply`. Use `std/fp::map`,
`bind` and `pure_with`. The unrestricted `Monad::pure<A>(value:A)` contract cannot
be implemented by a repeatable recipe for arbitrary exclusive A: that would
require duplicating A on later runs. Factory injection makes allocation explicit
and is not claimed to be that universal value-injection contract.

Option, Vec and fixed-error Result retain their existing Monad/Functor contracts
and also implement FlatMap/FromFactory. `OwnedEffect<R,E,_>` implements
FunctorOnce/MonadOnce, Apply/ApplyOnce and FromFactory. These capabilities are
independent until supertraits exist.

Sequential Apply/ApplyOnce, map2/zip/ap/sequence and Vec traversal are implemented
with explicit ownership policies; see [Applicative and traversal](applicative.md).
The direct effect sequence/traverse helpers use iterative async loops, yielding
between batches. `traverse_owned_with` supports exclusive inputs retained by
consuming children. [Console effects](console.md) now inject real or recording
services through an environment capability, with owned futures across suspension.

Sequential [comprehensions](comprehensions.md) now derive from these checked
operations: use `for<Effect<R,E,_>>` or `for once<OwnedEffect<R,E,_>>`. The final
yield maps a fresh value within execution, including exclusive resources.
See [the FP roadmap](higher-kinded-types.md).
Service/layer construction, fiber supervision, async mutexes and async finalizers
remain later work.

## Validation

`tests/reusable_effects.rs` exercises repeated and overlapping execution, fresh
and shared resources, copy proofs/rejections, COW containers/closures, per-run
provision, consuming adapters, cancellation, scopes, finalizers, stale/reentrant
capabilities and long chains on native/RVM. `tests/effects.rs` preserves the
consuming suite against `std/effect_once`. LSP tests cover reusable metadata and
consuming ownership diagnostics. A bounded compiled Brainfuck CPU test also
checks opaque shared-owner copies, argument snapshots and lexical release.

```sh
make -C runtime -j2
cargo test --release -p rfl --test reusable_effects --test effects --jobs 2
python3 rfl/editors/intellij/tests/lsp_effects.py target/release/rfl
```
