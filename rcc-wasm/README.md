# Direct Ripple IR → WebAssembly

W1, W2 and the W3 browser bridge are implemented. RCC and RFL compile directly
to Wasm with the C16 compatibility ABI, an in-Wasm runtime, managed values/closures, and typed floating
arithmetic. The browser DOM/event bridge and batched timer/fetch effects are available.
Owned RFL futures and the W4b mounted executor support ordinary browser async/await.

```sh
cargo build --release --jobs 2 -p rcc-wasm -p rcc-driver -p rfl
# Compile and run RFL with Node's WebAssembly engine:
target/release/rfl run rfl/examples/wasm.rfl --target wasm
# Save a module instead:
target/release/rfl compile rfl/examples/traits.rfl --target wasm -o /tmp/traits.wasm
node rcc-wasm/runner.mjs /tmp/traits.wasm
# C uses the same target and runtime:
target/release/rcc compile program.c --target wasm -I runtime/include -o /tmp/program.wasm
node rcc-wasm/runner.mjs /tmp/program.wasm
```

`NODE` selects the Node executable used by `rfl run`. `--jit` remains a VM option;
the Wasm engine compiles its own modules. The Node harness requires a zero-argument
`main`; argv/environment/WASI services are not supplied.

## IR linking and private ABI

```sh
target/release/rcc-wasm rcc-wasm/examples/sum.ir -o /tmp/sum.wasm
node rcc-wasm/runner.mjs /tmp/sum.wasm # exits with status 55
# Exported compute libraries need no main:
target/release/rcc-wasm library.ir --export initialize --export update -o /tmp/library.wasm
```

RCC/RFL can emit target-specific IR with `--emit-ir --target wasm`. Supply several
IR files to `rcc-wasm` for multi-unit linking. The Rust API's `Options.exports`
also names link roots. Private symbols are isolated per translation unit;
reachability follows global/function-pointer initializers. Unused runtime
functions and data are removed. User definitions take precedence over fallback
runtime functions; private symbols do not suppress another unit's runtime calls.
`__ripple_` and `__wasm_` are reserved implementation namespaces.

Each scalar parameter/result uses an i64 bit-pattern carrier; void returns zero.
This is a **private RCC/RFL ABI**, not the final public JS API. Calls between
RFL and imported C use the same C16 layout. Function pointers are opaque code
tokens, checked through a private Wasm table with arity-specific signatures;
they cannot be dereferenced as data. Browser applications receive typed i32
lifecycle/event wrappers, described below.

## Memory and runtime

A memory instance has 256 logical banks of 64000 16-bit cells: 500 Wasm pages,
32,768,000 bytes, fixed maximum. It does not grow in this ABI. Ordinary RAM uses
`byte_offset = 2 * (bank * 64000 + offset)`. Accesses reject bank zero, null/code
pointers, invalid encoded offsets, and ranges beyond logical memory. Wide
accesses and pointer arithmetic normalize bank crossings; small gaps in sparse
data are grouped into segments. BSS does not inflate the download.

Small globals use bank one; larger global layouts use up to 16 banks starting
at bank 32. The logical stack occupies banks two through four and reserves
constant allocas once per call, including C declarations skipped by `goto`.
The heap is one contiguous region starting at bank five or after extended
globals. Choosing a contiguous heap can leave unused banks below extended
globals. Exhausted allocation returns null; stack exhaustion traps explicitly.

The freestanding runtime implements malloc/free/calloc/realloc with reuse and
coalescing, memory operations and C16 string functions, abs/labs/llabs, buffered
putchar/puts, exit and RFL failure termination. It is compiled into the module,
with no native runtime, VM decoder, host heap, or JS reference-counting service.
`mmio_write(0, value)` supports text output; other device operations fail.

Console output uses `ripple.write(byte_address: i32, cells: i32)`.
It exists only when reachable output code needs it. Pure computation and
allocation modules have **zero imports**. Output buffers hold one UTF-8 octet
per 16-bit cell and flush on newline or at 8192 cells. The host borrows the
range only for that call, recreates its memory view, and performs no synchronous
guest reentry. `__ripple_flush()` flushes a final partial line; the Node harness
calls it after main. A 20,001-octet regression uses exactly three host calls.

The module exports `memory`, `__ripple_fault`, and `__ripple_exit_status` in
addition to requested functions. Fault codes are 1 (memory), 2 (pointer
arithmetic), 3 (stack), 4 (heap), 5 (unsupported device), 6 (indirect call),
7 (guest exit), and 8 (invalid browser operation/batch exhaustion). The runner
translates these into diagnostics or exit status.
A trapped instance must be discarded; restarting it does not unwind guest frames.

## Numeric and control-flow semantics

Integer operations preserve 1/8/16/32/64-bit widths and wrapping; shift counts
wrap at the IR width. Division/remainder by zero return zero. Signed MIN/-1
division wraps and remainder is zero, avoiding Wasm's division traps.

Typed `float.*` arithmetic, comparisons, conversions and explicit reinterprets
use Wasm f32/f64 instructions. Float-to-integer conversions saturate, including
NaNs and infinities. `native_float` is enabled; `host_math` stays disabled.
RFL sine/cosine polynomials and the existing large-angle integer reduction run
inside Wasm; floor/sqrt use Wasm instructions. No hot math operation calls JS.
RFL cfg uses `target = "wasm"` and `target_arch = "wasm32"`.

Ordinary lexical loop regions emit Wasm loops/blocks. Graphs that cannot use
those regions fall back to a function-local br_table dispatcher. The current
structurer can also use the fallback for unusual reducible block orderings.
`Artifact.statistics` reports both counts. Phi copies are simultaneous,
including cyclic copies and critical edges. SSA verification catches missing
or non-dominating definitions. Every emitted module passes wasmparser validation.

Variadics, stdin, files, guest networking/device services and Ripple inline assembly
remain unsupported and fail explicitly when reachable. Browser fetch is a separate
explicit host capability.
This is not a WASI target or the platform wasm32 C ABI. The existing host reactor
uses desktop DEVICE/clock I/O; browser applications use `std/browser_app` instead.

## Browser DOM and queued events

```sh
python3 rcc-wasm/examples/browser/run.py
# Open http://127.0.0.1:8000/rcc-wasm/examples/browser/
# Stateful C counter: add --c. Automated browser check: append ?test to the URL.
```

A reachable `ripple_browser_mount()` call opts main into the browser profile;
provide exported C hooks `ripple_browser_on_events` and
`ripple_browser_on_dispose`. The backend retains them as roots and emits typed
public i32 wrappers for startup, inbox access, event delivery and disposal.
The ordinary scalar ABI remains private. RFL uses `std/browser`; C uses
`rcc-wasm/browser.h`. The [RFL example](../rfl/examples/browser.rfl) works
without macros.

The static ES-module adapter `browser/bridge.mjs` applies versioned C16 patch
batches through the optional typed `ripple.dom_commit(byte_address, cells)`
import. A 500-node render uses one commit. Event callbacks snapshot and queue;
a frame delivers at most 64 ordered events in one Wasm dispatch. Stable node
identities, retired listener filtering, synchronous event policies and complete
normal disposal are tested. No model/diff/guest heap runs in JavaScript.

Counters expose crossings, batches, decoded/copied cells/octets, live nodes,
listeners, queued events and Wasm allocator activity. The adapter refreshes
memory views, streams UTF-8 output, preflights complete patch batches, rejects
oversized/malformed input explicitly, and cleans up on failure. Read the
[bridge protocol, limits and examples](../docs/rfl/browser-bridge.md).

The optional [browser effects ABI](../docs/rfl/browser-effects.md) batches timer/GET
starts and cancellation at the end of guest turns, then queues bounded completion
batches. `std/browser_effects` supplies owned lazy futures. Try the C transport
demo with `python3 rcc-wasm/examples/browser/run.py --effects`.

`std/browser_app` owns a persistent RFL task scope across browser callbacks and
supplies all lifecycle hooks. `app.spawn(future)` returns a cancellation/status
handle; `main` may return while tasks remain active. Standard `std/future.sleep`
and yield work without blocking the browser. Each active entry includes one clock
sample and one bounded scheduling turn; wake/deadline hints share effect batches.
No runnable work means no idle polling. Disposal cancels roots, releases captured
state, and drops the instance. Try the [RFL async demo](../rfl/examples/browser-async.rfl):

```sh
python3 rcc-wasm/examples/browser/run.py --async
# Open http://127.0.0.1:8000/rcc-wasm/examples/browser/
# Append ?test=async for the browser regression.
```

[Typed view reconciliation and public JSX](../docs/rfl/browser-ui.md) are available:
`std/html` builds `Html<Msg>`, `std/ui_command.program` runs Model/Msg/update/view,
and owned commands deliver async results as typed messages. Keyed nodes and
controlled inputs preserve identity/selection, and unchanged views make no DOM
import. Try `python3 rcc-wasm/examples/browser/run.py --ui`; append `?test=ui`
for the real-browser regression. Larger search/request applications and
component-local resource scopes remain the next milestone.
The Node console runner supplies no DOM or browser effects.

## Verification

```sh
cargo test --release --jobs 2 -p rcc-ir -p rcc-wasm
cargo test --release --jobs 2 -p rcc-driver --test wasm
cargo test --release --jobs 2 -p rfl --test wasm --test wasm_target --test wasm_browser --test pointer_index_inference
./rct test_wasm_runtime_core --timeout 30
```

Tests execute real Wasm in Node, including 3,795 integer cases, IEEE float edges,
C global/callback layouts, cross-bank memory, allocation reuse/coalescing,
malformed-pointer/signature/double-free/stack traps, and host crossing counts.
RFL checks cover Unicode, managed collections, trait printing, comprehensions,
owned drop callbacks, C imports and portable trigonometry. C output is compared
with native/RVM, and a shared call/loop/phi IR fixture runs on all three targets.

See the [backend and browser roadmap](../docs/rfl/wasm-browser.md) and the
[independent macro task](../docs/rfl/macros-agent-brief.md).
