# Rust → RCC prototype

Compile Rust through the real `rustc` frontend and typed MIR, legalize Rust
memory to **one 16-bit cell per Rust octet**, then reuse RCC's Ripple and native
backends to produce independent Riplix RXE and Fuse-X FXE applications.

This is an experimental compiler driver, not a complete Rust port or a Cargo
codegen-backend plugin yet. It lives in its own Cargo workspace so the normal
toolchain and existing C16 ABI are unaffected.

## Try it

The compiler-private API is pinned to **nightly-2026-05-07**, commit
`365c0e1d7a614ca94cb48431dcd2bc6d3b645db1`. Install the development components:

```sh
rustup toolchain install nightly-2026-05-07 --component rustc-dev,rust-src
```

The scripts also accept the installed `nightly` alias if it points to exactly
that commit. They reject other compiler versions rather than relying on a
compatible-looking private API. Python 3, Node, and the existing RCC tools are
also required. Build each OS normally first to provide its application SDK.

From the repository root:

```sh
python3 rust-rcc/compile.py rust-rcc/examples/guest.rs --target fuzix -o build/rust-rcc/guest.rxe
python3 rust-rcc/compile.py rust-rcc/examples/guest.rs --target fuse-x -o build/rust-rcc/guest.fxe
python3 rust-rcc/test.py
```

The first invocation builds the small driver and target `core`, `alloc`, and
`rustcell` support metadata; later invocations reuse them.
`--emit-ir` writes inspectable RCC IR instead of linking
an executable. `--build`, `--compiler`, `--fuzix-build`, `--fuse-x-build` and
`--toolchain` select alternate build locations/tools. Paths are resolved from
the caller's working directory. No command installs into a normal guest disk.

`test.py --system host` runs frontend diagnostics, the standalone native memory
probe, and enum/destructor/collection checks compared with the native Rust compiler.
`--system fuzix` and `--system fuse-x` select one guest;
`--skip-build` reuses existing guest probes. The default runs both. It creates
small disposable images, disables networking, checks the filesystems before and
after execution, and writes guest logs plus `results.json` under `build/rust-rcc`.

The guest example expects one `R` followed by Enter after printing
`RUSTCELL_WAITING`. The harness supplies this after withholding input, to test
a genuinely blocking read. A captured callback performs the read while a value
with a destructor remains live; its cleanup is checked after resumption. The
application keeps a heap-backed `Vec` across that read, then forks, changes the
child's vector, sleeps, waits for exit status 37, and checks that the parent's
vector and allocator state remain usable and independent.
The harness also supplies fixture files and invokes this diagnostic program as
`rustprobe /input /output 'two words' ''`; its file/argument checks require those
arguments. For a normal standalone program, use `examples/upper.rs` below.

## Cargo projects

`cargo.py` builds a Cargo binary and its Rust library dependencies into a guest
executable. The application keeps the same `#![no_std]`, `#![no_main]`, forwarding
panic handler, and `fn rust_entry() -> u16` contract as single-file programs.
The bundled `rustcell` crate is available automatically; it needs no Cargo
dependency declaration.

The example workspace includes a library with a generic trait call, generated
source from build scripts in both packages, and a host procedural macro:

```sh
python3 rust-rcc/cargo.py --manifest-path rust-rcc/examples/cargo/Cargo.toml \
  --target fuse-x --offline -o build/rust-rcc/cargo-example.fxe
python3 rust-rcc/cargo.py --manifest-path rust-rcc/examples/cargo/Cargo.toml \
  --target fuzix --offline --features loud -o build/rust-rcc/cargo-example.rxe
python3 rust-rcc/test_cargo.py
```

Use `--package` and `--bin` when a workspace has several applications. The helper
also accepts `--features`, `--all-features`, `--no-default-features`, `--release`,
`--locked`, `--offline`, and `--emit-ir`. Without `--output`, it prints the final
path inside `build/rust-rcc/cargo/`. Build the OS SDKs normally first, as for
`compile.py`. These commands never install into normal guest disks.

Cargo handles dependency resolution, feature selection, build scripts and
source invalidation. Its target compiler wrapper checks libraries as metadata,
with every function's MIR encoded. It checks the selected application as a
library because RCC supplies the allocator/entry shims later. Inside that same
Cargo invocation, the MIR driver lowers the application's reachable functions,
including functions from dependencies. Running there preserves `env!`, Cargo
package variables, build-script `rustc-env` values, and `OUT_DIR` without saving
the environment to disk. Host build scripts and procedural macros compile and
run with the pinned host rustc; target libraries supply MIR to RCC.

Each workspace/application/output system has an isolated Cargo cache. MIR
artifacts are keyed by Cargo's feature-specific artifact names. Linked outputs
are keyed by IR plus runtime, compiler, linker and SDK inputs. Unchanged builds
reuse outputs and preserve their modification times; changing a dependency or
switching features regenerates the affected output. `test_cargo.py` exercises
base → feature → base and edit → restore transitions, failed builds preserving
previous output, and actual execution in both guests using disposable images.

This is a Cargo-assisted prototype, not a codegen-backend plugin for arbitrary
`cargo build`, `cargo run`, or `cargo test` invocations. Target dependencies must
be Rust `no_std` libraries within the translator's supported subset. Native
target libraries and linker arguments are rejected. The registry probes below
establish specific tested APIs, rather than general crate compatibility; the
architecture placeholder described below still applies. The helper fixes rustc optimization level 1,
panic abort, and overflow checks off; `--release` selects Cargo's release
profile while retaining those prototype settings. Inherited Rust flags and
compiler wrappers are overridden for this isolated build.

## Registry compatibility and a real utility

`examples/crates` pins direct dependency versions and includes its Cargo lockfile
for transitive dependencies. All upstream crate sources remain unmodified. The
same probes run through ordinary host Rust and both guest backends; a separate
filesystem reader compares every saved output byte after guest shutdown.

| Dependency | Tested subset |
| --- | --- |
| `byteorder 1.5.0` | Signed/unsigned integer encodings through 64 bits, odd offsets, big/little endian and variable-width integers |
| `heapless 0.9.3` | Inline `Vec`, wrapped `Deque`, Unicode `String`, capacity failures, removal/truncation and owned element destruction |
| `serde 1.0.228` + `postcard 1.1.3` | Derived record/enum serialization and deserialization, borrowed strings/bytes, owned vectors/strings, signed varint limits, truncated records, invalid enum variants and malformed UTF-8/primitive inputs |

The `postcard` feature isolates serialization and primitive decoding;
`postcard-derived` adds full record/enum round trips and deserialization errors.
Those error paths construct real `dyn de::Expected` values. Both guests run the
unmodified Serde/Postcard implementation, including its recoverable errors.
The `dynamic` probe separately exercises reference/mutable-reference dispatch,
multiple implementations, default methods, associated types, supertrait upcasts, wide arguments,
aggregate returns, static trait objects, virtual-method function pointers, function-pointer tables/optional
pointers, captured `dyn Fn`/`dyn FnMut` closures, noncapturing closure function
pointers, and `Box<dyn Trait>` destruction (including null drop slots).
Its guest-only callback test blocks through both a function pointer and virtual
method, resumes with heap state intact, then forks through a function pointer
and verifies separate parent/child trait-object state.

Features are tested individually within the translator's existing bounds;
combined applications still have to fit those bounds. Floating-point/128-bit
APIs, atomic collections, and other APIs outside this table are not covered.

The shared checks also exercise immutable reference tables, cyclic static data,
interior references, niche-encoded constant pointers, 512-cell alignment,
slice-tail struct coercions/dynamic layout, and signed overflowing multiplication
at all supported widths. Guest subprocess checks verify panic exit status 101
for failed unwraps and short byteorder buffers. A side effect in a formatted
panic argument must execute exactly once before the fixed abort bridge runs.

```sh
# Download the locked dependencies once; subsequent runs are offline.
python3 rust-rcc/test_crates.py --fetch
python3 rust-rcc/test_inspect.py
# Just the dynamic-dispatch/derived-deserialization milestones:
python3 rust-rcc/test_crates.py --crate dynamic --crate postcard-derived
# Optional: --system fuse-x, --system fuzix, or --crate heapless (crate suite).
```

Reports and separate host/build/guest logs are under `build/rust-rcc/crates/`
and `build/rust-rcc/inspect/`. Reports include dependency versions, transcript
hashes and executable sizes. RVM assembly generation can take several minutes
for these larger programs. Both suites use disposable disks and check filesystem
consistency before and after execution; usual guest disks are untouched.

`examples/inspect` is a small usable application built with `byteorder`. It
streams a file in 16-byte rows and prints offsets, hex bytes, readable text and
little-endian 32-bit words. It does not load the whole file into memory.

```sh
python3 rust-rcc/cargo.py --manifest-path rust-rcc/examples/inspect/Cargo.toml \
  --target fuse-x --offline --locked -o build/rust-rcc/binspect.fxe
python3 rust-rcc/cargo.py --manifest-path rust-rcc/examples/inspect/Cargo.toml \
  --target fuzix --offline --locked -o build/rust-rcc/binspect.rxe
# After installing the appropriate executable in a guest:
binspect FILE
```

Its integration suite compares native and guest output for empty files, row
boundaries and a 20,009-byte binary file, including NUL and non-UTF-8 bytes.
The native reference deliberately uses short reads to check row assembly.
Missing files return 1, incorrect usage returns 2, and success returns 0.

## The ABI being tested

Rust retains its usual sizes and little-endian byte representation:

| Rust object | Rust bytes | Physical cells | RCC scalar carrier |
| --- | ---: | ---: | --- |
| `u8` / `i8` / `bool` | 1 | 1 | 16 bits |
| `u16` / `i16` | 2 | 2 | 16 bits |
| `u32` / `i32` / `char` | 4 | 4 | 32 bits |
| `u64` / `i64` | 8 | 8 | 64 bits |
| `usize` / thin pointer | 4 | 4 | 32 bits |
| Slice/string reference | 8 | 8 | pointer to a private aggregate copy |
| Trait-object reference | 8 | 8 | private aggregate: data pointer + vtable pointer |
| Rust function pointer | 4 | 4 | 32-bit application-local function token |

Every stored octet occupies a cell's low eight bits. Loads assemble octets into
registers; stores split them back out. Signed `i8` loads explicitly sign-extend.
Rust layout queries supply field offsets, padding, array strides and alignment.
Stack objects are aligned in cells according to that Rust-byte alignment.
Transmutes and aggregate copies operate on this octet-cell representation.

Immutable constant allocations are cached by rustc allocation identity.
Pointer-free byte data can be emitted directly. Other immutable data uses
aligned backing globals and an entry-time initializer: establish every address,
copy raw octets, then fix up four-octet data pointers. This preserves reference
identity and cycles without changing the existing C16 global initializer ABI.
Constant backing storage is limited to 1,000,000 octet-cells per allocation and
alignment at most 512; source bytes and alignment padding add storage overhead.
Rustc vtable allocations use the same data relocation mechanism; method/drop
slots contain Rust function tokens. Mutable statics, thread-local storage and
type-id constants remain explicit compiler rejections.

### Dynamic calls

Rust function pointers are application-local tokens (zero is invalid), stored
as four octet-cells. They are distinct from C16 code pointers and host machine
addresses. Each reachable dynamic-call signature gets a typed dispatcher whose
branches call the matching monomorphized Rust functions through the normal RCC
ABI. This preserves hidden aggregate results, scalar widths, argument copies,
blocking syscalls, and resumable frames on both backends. A fixed-point pass
discovers targets whose bodies introduce further dynamic calls. Vtable entries
that no reachable call signature can use need no function body; this avoids
pulling formatting implementations into Serde errors that never format them.

Trait objects retain rustc's slot order: drop, Rust-byte size, alignment, methods,
and any supertrait vtable pointers. Virtual calls remove the receiver's metadata
before calling the concrete method. Supertrait upcasts follow rustc's selected
vtable slot; dynamic `size_of_val`/`align_of_val` read the header. Drop calls the
concrete destructor or skips a null drop slot, after which normal `Box` glue
deallocates storage using the dynamic layout.

The tested subset supports reference/raw-pointer receivers, owned boxed objects
with borrowed methods, and Rust/RustCall function signatures. Consuming trait
methods (such as `self: Box<Self>`), structs ending in `dyn Trait`, C callbacks,
signature-changing function-pointer calls/transmutes, and arbitrary code-address
interoperability are outside the prototype ABI. Wrap approved C16 bridge calls
in a Rust function when passing one as a callback. Foreign function-pointer
declarations are rejected before asking rustc for a nonexistent function body.

Thin pointers carry the existing logical C16 bank/offset pointer bits, stored
as four octet-cells. Pointer offsets use RCC's bank-aware GEP; pointer differences
use linear cell addresses so the gaps in packed bank/offset integers do not
count as Rust bytes. General pointer provenance remains a prototype limitation.

Scalar Rust arguments/results travel in RCC carrier registers. By-value
aggregates receive private copies. Aggregate results use a caller-owned hidden
return pointer, so no result points into a discarded callee frame. MIR loop
headers are marked for the native backend's scheduling checkpoints.

Enums use rustc's chosen variant fields, offsets and discriminants. This includes
direct tags and niche layouts such as the null pointer in `Option<&u16>`, unused
`bool` values, and tags stored inside a payload field. Narrow niche arithmetic
wraps at the tag's actual width.

Destructors call rustc's instantiated drop glue, including cleanup for fields,
arrays, slices, active enum variants and zero-sized values. Closure captures use Rust
field layouts; Rust's closure calling convention spreads the final argument
tuple at calls and reconstructs it where the MIR requires a tuple local.

The hand-written C bridge permits only explicitly checked signatures. For
example, Rust `u16` and C16 `unsigned short` can share a scalar carrier, but
**a pointer to one is not a pointer to the other**: their objects occupy two
and one cells respectively. Rust `repr(C)` does not make a structure directly
compatible with our existing C16 structures. Strings/buffers cross as `u8`
octet-cell pointers plus a 32-bit length; arbitrary libc FFI is rejected.

## Heap and collections

`extern crate alloc` enables the pinned compiler's real `Box`, `Vec`, and
`String` implementations. Their reachable MIR goes through RCC like application
code. No host Rust collection implementation is linked into a guest.

The prototype supplies a fixed default allocator. Calls from `alloc`'s internal
allocator declarations lower to a small C16 bridge over guest `malloc`/`free`.
Sizes count Rust octets (physical cells), alignments count cells, and returned
pointers retain the logical bank/offset representation. The bridge stores the
original allocation before its aligned payload. Reallocation allocates, copies
the preserved prefix and then frees; failure leaves the old allocation intact.
Zeroed allocation clears the full requested range. Guest memory, including
allocator metadata, follows the existing process memory and fork machinery.

The current policy limits an allocation **including alignment overhead** to
1,000,000 cells and supports power-of-two alignments through 32,768. Actual guest
heap capacity can be smaller. Banks contain 64,000 cells, whereas packed pointer
bits use a 65,536 stride. Alignments above 512 therefore stay wholly inside one
bank; requests needing larger spans with those alignments return null. Ordinary
alignments can span banks. `try_reserve` reports failure; infallible allocation
failure uses the fixed panic bridge (exit 101). Custom `#[global_allocator]`
implementations are rejected explicitly rather than silently bypassed.

Dynamic copy/move/fill intrinsics use C16's bank-aware memory routines, with
element counts multiplied by Rust element sizes. The 1,024-byte limit on
individual stack objects and unrolled aggregate copies does not limit these
dynamic heap transfers. Union fields (including `MaybeUninit`) and slice drop
glue are supported for these collection paths. This remains a tested subset of
`alloc`, not a claim that every collection or method compiles.

## Arguments and files

The bundled `rustcell` crate is available to applications automatically. It is a
small `no_std` support library compiled through the same MIR backend, not a
replacement implementation of Rust's `std`.

| API | Behavior |
| --- | --- |
| `env::args()` | `Result<Vec<Vec<u8>>>`, including argument zero; empty and quoted arguments preserved |
| `fs::File::open` / `create` | Owned descriptor; automatic close on `Drop` |
| `fs::OpenOptions` | Read/write, create, truncate, append, exclusive creation; builders return `Self` |
| `File::read` / `write` | May transfer fewer bytes than requested |
| `File::read_exact` / `write_all` | Loop until complete or return an error |
| `File::read_to_end` | Append to an existing `Vec<u8>` |
| `File::seek` | `SeekFrom::Start`, `Current`, or `End`; signed 32-bit guest file offsets |
| `File::close` | Consume the file and report close errors; `Drop` ignores them |
| `fs::read` / `write` / `remove_file` | Whole-file helpers and unlink |
| `io::read_stdin` / `write_stdout` / `write_stderr` | Byte-oriented console/pipe I/O |

Paths and arguments are byte strings. Paths must be nonempty, contain no NUL,
and fit in 511 bytes; the guest filesystem can impose shorter component limits.
Files open relative to the guest's working directory and go through its normal
filesystem calls, including existing mounts. New files request mode `0666`,
subject to the guest umask. Raw C16 flags, descriptor pointers and `argv` arrays
do not cross into Rust. The bridge translates open options, copies arguments,
retries interrupted open/read/write/seek calls, and captures errors immediately.
`Error::Os(code)` contains the guest C16 errno; other variants distinguish invalid
input/data, unexpected EOF, and a zero-progress write. Collection growth still
uses the prototype's infallible allocator, so allocation failure can exit 101
rather than produce an I/O error.

`examples/upper.rs` is a complete utility using these APIs:

```sh
python3 rust-rcc/compile.py rust-rcc/examples/upper.rs --target fuse-x -o build/rust-rcc/upper.fxe
python3 rust-rcc/compile.py rust-rcc/examples/upper.rs --target fuzix -o build/rust-rcc/upper.rxe
# In a guest after installing its executable as /bin/upper:
upper INPUT OUTPUT
```

It uppercases ASCII letters and preserves every other byte. The integration
suite runs the actual utility in both guests, checks success/missing-file/usage
exit codes, and verifies the saved output independently after shutdown. Its
20,009-byte binary fixture includes NUL and non-UTF-8 bytes and exceeds a single
bridge write (16,384 bytes). Additional guest checks cover seek/append/truncate,
exclusive creation, path validation, saved errno, EOF, and descriptor cleanup.

## How compilation works

1. A metadata-only sysroot is built from the pinned compiler's real `core` and
   `alloc` sources and the bundled `runtime/rustcell` crate, with cross-crate MIR
   encoded. The empty `compiler_builtins` metadata
   marker satisfies rustc's bootstrap requirement; it supplies no arithmetic
   implementation. Supported arithmetic lowers through RCC; unsupported
   intrinsics and widths produce errors.
2. `rustc_driver` performs parsing, macro expansion, type checking and borrow
   checking. After analysis, this driver starts at `fn rust_entry() -> u16`,
   resolves reachable generic/trait calls and instantiates their typed MIR,
   including reachable `core`, `alloc`, and `rustcell` methods.
3. `layout.rs`, `memory.rs` and `lower.rs` produce normal RCC IR with the explicit
   memory legalization described above. This is not a textual MIR parser or a
   Rust-to-C transpiler.
4. Riplix uses the existing RCC assembler backend, assembler, RXE linker and
   guest libc. Fuse-X uses its existing `cc.py` SDK and FXE linker, preserving
   logical memory and resumable guest execution. No Rust-generated LLVM machine
   code or LLVM-built `core`/`alloc` object is linked into either executable.

**Architecture placeholder:** `targets/ripple-fuzix.json` currently uses `x86`
and an `i686` LLVM triple only to satisfy upstream rustc's architecture/foreign
ABI checks while preparing target metadata and performing constant evaluation.
Its OS is `fuzix`, its environment is `rustcell`, and pointers are 32 bits for
both output formats. It is not a real x86 executable target, but it currently
reports `cfg(target_arch = "x86")`. Architecture-dependent crates/code are
outside this proof of concept. An honest `ripple` architecture requires further
rustc target/call-ABI integration; merely changing that JSON field currently
causes an upstream compiler panic. A future codegen-backend integration should
remove this placeholder before general crate support.

## Scope and next work

The probes exercise byte reinterpretation and mutation, dynamic array indexing,
signed extension and shifts, eight-bit wrapping, 32/64-bit values, generic
specialization, static trait dispatch, struct arguments/results, strings/slices,
explicit C16 I/O, syscall suspension, and process memory separation.

`examples/values.rs` checks `Option`, `Result`, `?`, nested matches, signed enum
tags, reference and integer niches, captured and mutable closures, destructor
ordering, moves, partial moves, conditional cleanup, and `mem::forget`. The same
checks run through native rustc, RCC's standalone native backend, and both guest
formats. Compiler rejection tests also cover invalid borrows, use after a move,
unsupported 128-bit values, unapproved FFI, incorrect bridge signatures,
variadic declarations, link-name overrides, and unsupported custom allocators.

`examples/heap.rs` compares collection behavior against native Rust and checks
zeroed allocation, alignment, growth/shrink, overlapping cross-bank copies,
pointer differences, allocation failure recovery, and actual reuse of freed
space. Collections containing owned values check element drop counts, truncation,
boxed slices, and zero-sized elements. Guest-only checks also cover the bridge's
allocation limits and heap isolation across `fork`.

The current translator is intentionally bounded to 1,024-byte individual stack
objects/copies and 1,024 reachable function instances. It rejects unsupported MIR
instead of delegating it to host machine code. It does not provide `std`, general
unsized types, floating point, 128-bit arithmetic, atomics, inline assembly,
mutable statics, unwinding, or general Cargo codegen-backend integration.
Slices, structs ending in slices, and the trait-object/function-pointer subset
above are supported. Panic handling currently uses the fixed
`rustcell_panic` bridge and exit code 101; examples must provide the forwarding
panic handler shown in `examples/probe.rs`. Custom panic handlers/formatting are
outside the prototype's supported contract. Pure core formatting constructors
feeding only an abort are elided, while calls that evaluate argument expressions
and memory side effects are preserved. Ordinary integer arithmetic uses
release-style overflow settings.

The next steps are broader library/MIR coverage with differential tests, a
production pointer/allocator ABI, and more OS bindings. A proper
target/codegen-backend interface and broader Cargo compatibility are also still needed.
Running rustc/Cargo inside either guest remains a separate self-hosting project.
