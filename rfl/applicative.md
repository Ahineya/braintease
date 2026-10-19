# Sequential Applicative and traversal

`std/applicative` supplies constructor-polymorphic sequential combination.
`Apply<_>` combines successes without copying them; `std/fp::FromFactory<_>`
injects fresh values. Together they provide factory-based Applicative operations.
These are independent capabilities until the language has supertraits. The
existing unrestricted value-taking `Monad::pure` contract is unchanged.

```rfl
import {map2,sequence} from "std/applicative";
import {traverse} from "std/traverse";

fn main() {
    println(map2(Some(20),Some(22),fn(a:i16,b:i16)=>a+b));
    println(sequence<Option,i16>([Some(1),Some(2)]));
    println(traverse<Option,i16,i16>([1,2],fn(n:i16)=>Some(n+1)));
}
```

## Contracts and ownership

| Capability / operation | Policy |
|---|---|
| `Apply::map2`, generic `map2` | Left success before right; combine by moving both outputs |
| `zip` | Combines into an owned tuple |
| `ap` | Obtains a reusable function, then its argument, then invokes it |
| `ap_owned` | Obtains a consuming function, then its argument; effects can produce a fresh function per run |
| `ApplyOnce`, `map2_once`, `zip_once` | At most one pair with an exclusively captured consuming callback |
| `sequence` | Collects already-constructed computations in source order, including empty input |
| `cartesian_map2`, `cartesian_zip` | Vec Cartesian product, left-major order, explicitly requiring copyable inputs |

Option and fixed-error Result implement Apply and ApplyOnce. Effect implements
Apply, producing a new pair and invoking the reusable combiner on each execution.
OwnedEffect implements both, executing at most once. OwnedEffect now also
implements FromFactory. Outputs and errors may own exclusive resources.

Option None and Result Err skip the combiner. The first Result error wins.
Effects start the right only after the left succeeds; cancellation of a pending
right child releases that child before the retained left value. Enclosing
finalizers run afterward. Consuming computations release unused right programs
and combiner captures. Argument evaluation is still eager: constructing the
right argument can run ordinary code before map2 is called. Put I/O inside an
effect to defer it.

Vec deliberately has no unrestricted Apply instance. Cartesian combination
can reuse an input in many pairs, so silently giving it the same resource contract
would be wrong. The explicit Cartesian helpers require A:Copy and B:Copy; their
fresh output C can be move-only. Pairwise Vec zip would have different laws from
the existing singleton factory injection and is not substituted implicitly.

The usual identity, associativity and factory homomorphism laws apply to lawful
instances and factories/combining functions without extra observable effects.
RFL does not certify purity or laws. Factories with state or I/O retain their
documented evaluation count; reassociation must not invent extra factory calls.

## Traversing inputs

`std/traverse::TraverseVec<_>` describes a **destination constructor for a Vec
traversal**. This is a focused API, not a claim to implement the fully general
Cats Traverse abstraction over arbitrary source constructors.

| Operation | Inputs and execution |
|---|---|
| `traverse(values,f)` | Copyable input elements; eager for Option/Result, deferred and copied per run for Effect |
| `traverse_with(make,f)` | Fresh possibly exclusive inputs from a reusable factory |
| `TraverseVecOnce`, `traverse_once(values,f)` | Transfers an already-owned input vector to Option/Result/OwnedEffect |
| `std/effect::traverse_owned_with(make,f)` | Fresh inputs per run, with consuming child programs that can retain each input across awaits |

Every callback is invoked left-to-right, only after the previous child succeeds.
The first None/Err stops traversal without invoking f on the remaining inputs.
Partial outputs and unvisited exclusive inputs release on failure/cancellation.
On cancellation, an awaited child releases before the accumulator and remaining
inputs. Empty input produces a fresh empty output vector and calls no f.

The callback is reusable because a vector can have many elements. Its outputs
may be owned. A callback returning a reusable Effect cannot capture its exclusive
input in that recipe; use `traverse_owned_with` to retain it in a consuming child:

```rfl
import {Effect,traverse_owned_with} from "std/effect";
import {from_async} from "std/effect_once";
import {Environment} from "std/effect_env";
import {Future,yield_now} from "std/future";

async fn use_resource(value:Owned<i16>)->Result<Owned<i16>,String> {
    yield_now().await;
    Ok(move(value))
}
fn work()->Effect<unit,String,Vec<Owned<i16>>> {
    traverse_owned_with(
        fn()=>[owned(42,fn(n:&mut i16)=>{})],
        fn(n:Owned<i16>)=>from_async(once fn(env:Environment<unit>)=>use_resource(move(n)))
    )
}
```

Constructor-polymorphic calls sometimes need explicit partial slots and result
arguments, for example `traverse<Result<_,String>,i16,i16>(...)` or
`map2<Effect<unit,String,_>,i16,i16,i16>(...)`. Do not depend on inference guessing
the varying slot or the missing error argument of an Ok-only callback.

## Execution and performance

`std/effect` and `std/effect_once` also export direct map2/zip/sequence/traverse
helpers. Prefer their sequence/traverse helpers for large effect workloads:
they run an iterative async loop, rather than constructing a nested map2 graph.
Generic `std/applicative::sequence` derives a fold from Apply + FromFactory;
it is useful for user constructors but can allocate one composition node per item.

Effect traversal reverses inputs once using ownership-transferring pop, then
appends each successful result to one accumulator. It does not index-copy
exclusive elements. Reversal and collection yield between 64-item batches;
cancelled batches release all initialized owners. User factories/callbacks must
still do bounded synchronous work themselves. No parallel work is started.

The regression runs a reusable traversal twice with owned results, checking
values and exact cleanup counts. It enforces guest-instruction budgets and
sub-2.5x growth when doubling 128 to 256 inputs. The benchmark shares this fixture:

| Items per run | Instructions for two runs and full owned-result cleanup |
|---:|---:|
| 128 | 35,136,392 |
| 256 | 69,916,238 |
| 512 | 139,140,682 |

Doubling inputs costs approximately 1.99x the instructions in this workload.
These are guest-work measurements, not a general throughput guarantee. Run with:

```sh
python3 rfl/tools/bench_effects.py --case traversal --depths 128 256 512 --check
cargo test --release -p rfl --test applicative --jobs 2
```

Parallel Applicative operations, arbitrary-source Traverse, comprehensions,
service layers and asynchronous finalizers remain separate extensions. See
[the Console service](console.md) and [the roadmap](higher-kinded-types.md).
