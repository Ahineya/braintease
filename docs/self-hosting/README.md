# Self hosted development toolchain

Decision: build a portable C toolchain around a cproc-derived frontend that
emits RCC IR. Preserve the existing C16 data model and both Ripple and native
Fuse-X targets. Full compiler capabilities and independent guest builds are the
destination; restricted early implementations are milestones, not a permanent
language subset.

## Architecture

```text
C source -> portable C preprocessor -> C16 C frontend -> RCC IR
                                                        |
                                +-----------------------+------------------+
                                |                                          |
                         Ripple backend                         native hosted backend
                                |                                          |
                        assembler / linker                       code / FXE packaging
                                |                                          |
                         RXE executable                            FXE executable
```

The initial bootstrap used host preprocessing and Rust backends. Portable
preprocessing, baseline Ripple code generation, assembly and linking now run
inside the guests. Native FXE code generation and initial SDK preparation still
use host tools. One complete Ripple compiler generation now builds inside
Fuse-X. Comparing subsequent compiler generations and rebuilding the SDK remain
acceptance gates for independent self-hosting.

The frontend emits RCC IR directly, before QBE-specific ABI lowering. C16
layout, integer promotions, constants and pointer scaling belong in the
frontend. Bank crossing remains represented by RCC pointer operations. Both
targets retain 16-bit char/short/int, 32-bit long and logical pointers, 64-bit
long long, binary32 float and binary64 double. Aggregate layout matches RCC's
sequential cell layout. The current bank size is 64,000 cells.

The native backend must preserve logical memory, function tokens, resumable
frames, fork, syscall suspension and loop checkpoints. An ordinary platform C
object is not a Fuse-X executable. See [the executable ABI](../../fuse-x/EXECUTABLES.md).

## Language and compatibility goals

Reach current RCC feature parity, then expand toward C11. Track parsing,
lowering and runtime support separately: accepting a C11 construct upstream
does not establish that either Ripple or Fuse-X implements it.

The eventual toolchain includes full preprocessing; all integer widths and
floating point; aggregate initialization, passing and returns; function
pointers and variadic functions; volatile accesses; supported assembly and
builtins; useful diagnostics; separate compilation, libraries and optimization.
Atomic operations, thread-local storage and threading need explicit target and
OS contracts before being advertised. Unsupported lowering must fail clearly.

Use RCC's tests and real application builds as compatibility gates. Correct C
behavior takes precedence over copying an existing compiler bug. Use explicit
ABI tests for widths, signedness, aggregate offsets, pointers, callbacks and
variadic calls.

## Foundation choice

| Candidate | Decision |
| --- | --- |
| FCC | Useful reference for constrained systems, but its numeric/type representations and language limits require substantial expansion. |
| Reimplement the Rust frontend | Retains design knowledge but duplicates a large working frontend. |
| chibicc | Rich language support; whole-unit allocation and platform assumptions make memory adaptation less attractive. |
| cproc | Selected: C99 implementation, richer C support, backend callbacks and incremental expression disposal. |

cproc upstream is <https://git.sr.ht/~mcf/cproc>, pinned initially to
`d1c53ddf56571573a7025324c8dd5c6d547a4d1f` (2026-06-01). Preserve its ISC/MIT
notices and keep the upstream snapshot distinct from local adaptations.
Its QBE backend and incomplete preprocessor are not our target toolchain.

Feasibility experiments ran an adapted cproc inside Fuse-X and compared QBE
output with a host build for eight feature probes and the preprocessed
expression parser source. This establishes frontend portability, not C16 target
correctness or executable generation. Temporary probes are not a supported SDK.

## Stages and acceptance gates

1. **C16 frontend and RCC IR integration.** Vendor the pinned sources, make
   target layout explicit and implement direct IR emission. Execute vertical
   slices through existing native and Ripple backends. Cover C16 integer
   promotions, 64-bit values, floating point, calls, control flow, pointers and
   selected C11 frontend features. Maintain an explicit coverage list.
2. **Guest frontend and preprocessing.** Implement portable C preprocessing
   using RCC's behavior and regression tests. Build the tools as RXE and FXE,
   process substantial translation units and measure peak memory. Fix bootstrap
   compiler and libc gaps rather than hiding them in permanent workarounds.
3. **Independent Ripple toolchain.** Implement portable C code generation,
   assembler, linker and SDK installation. Rebuild the compiler with itself,
   compare subsequent bootstrap generations, run the compatibility suite and
   build libc, applications and the C kernel inside the guest.
4. **Independent native Fuse-X toolchain.** Implement native emission and
   packaging with the hosted continuation ABI. Build and execute FXE programs
   without host RCC, Cranelift or platform linker services. Exercise fork,
   scheduling, signals, exec and GUI applications.
5. **Optimization and language expansion.** Bring guest tool quality toward
   host RCC and expand tested C11 support. RFL continues to share RCC IR; a
   self-hosted RFL frontend is separate work using the same backend toolchain.

Compiler self-hosting does not imply support for Rust or the ability to rebuild
the macOS integration runtime inside Riplix. Those are separate requirements.
Increase process memory limits when measured workloads require it; do not
reduce the language goal to preserve today's limits.

## Implementation

Implementation lives in [rcc-portable](../../rcc-portable/README.md). Its coverage
and test instructions describe what actually works in each milestone. Existing
RCC remains the working compiler and reference during bootstrapping.

Stage 1 now includes direct C16 IR emission for scalar code and RCC-compatible
struct/union calls and returns, variadic definitions and symbolic global
initializers. Execution tests cover native and Ripple output, including nested
aggregate calls, ownership of returned objects, bank-crossing copies and static
addresses, default argument promotions, and linking the two frontends in both
directions.

Stage 2 now has guest frontend builds for both environments: FXE for native
Fuse-X and RXE for Riplix under RVM. The portable frontend compiles its own C16
sources for these packages. Portable preprocessing is now the default, and runs
inside either guest: `rcc-c` accepts raw C and headers and can emit either Ripple
or native RCC IR, or preprocessed text with `-E`. This stage originally relied
on the host backend, startup, libc and executable packaging. The portable
Ripple pipeline described below removes the host backend from guest builds;
the initial SDK and native FXE emission still require host tools.

Fuse-X checks run five upstream utilities built by this frontend, shared-folder
I/O, fork/exec and compiler support routines. They compare guest-generated IR
with the host build, link and execute the resulting programs, and compile the
frontend's own expression parser. Function-local IR arenas reduced allocator
overhead; that parser workload requested roughly 352,000 live C16 cells for
preprocessed input and 446,000 cells for raw C with headers, excluding SDK
allocations, allocation metadata, stack, globals and code.
The Riplix/RVM package spans multiple code banks and has a separate private-disk
execution/parity test. See the implementation README for commands and reports.

The RVM bootstrap also exercises backend cases missed by the earlier small
fixtures: fixed homes introduced by inlining, full-width unary negation and
complement, and block layouts that put a dominating definition after its use.
The frontend also preserves floating-point signed zero and passes typed floating
constants at the helper-call ABI. Regression tests cover these cases.

The local portable preprocessor replaces cproc's incomplete implementation.
It handles includes, macros with rescanning/stringizing/pasting, variadics,
conditional directives with short-circuit 64-bit expressions, line tracking
and file/line builtins. Thirty macro/conditional probes compare its output
against Clang. All compiler translation units compile from raw source and match
their preprocessing round trips. Both guests exercise raw source, real C16
headers, exact host IR parity and `-E` round trips; Fuse-X additionally processes
the raw expression parser. Broader compatibility and remaining preprocessing
conformance work are tracked in the implementation README.

Stage 3 now includes a portable RCC IR reader and baseline Ripple backend in
`rcc-portable/ripple`. It lowers integer code at all widths, bank-aware pointers,
globals and symbolic initializers, control flow and parallel phi copies,
direct/indirect and variadic calls, frontend aggregate copies and floating-point
helper calls. It interoperates with existing Rust-generated objects. SSA values
use stack homes and individual frames are limited to 32,000 cells; register
allocation and broader workloads remain necessary. This is the portable
frontend's IR subset, not yet the complete general RCC IR grammar.

The backend builds as a host tool, an FXE and an RXE. A further RXE build uses
the portable frontend and backend to generate its own code. Host tools still
assemble/link that build and provide startup/libc, so this establishes a backend
bootstrap generation rather than full guest self-hosting. Guest tests compare
generated assembly byte for byte with the host tool and execute packaged output.
Portable assembly and linking now run inside both guests as well. The compact
RPO1/RPA1 formats allow the linker to stream code/data while retaining only
symbols and dependencies in memory. A guest `cc` driver runs preprocessing,
frontend, Ripple backend, assembly and linking as ordinary guest processes.
Riplix has compiled and executed an RXE entirely through that pipeline; Fuse-X
has cross-compiled RXE and bare Ripple programs. The initial SDK is converted
from existing host objects by a bootstrap script. SDK rebuilding, subsequent
compiler bootstrap generations and independent native code generation remain
the next major gates.

A further bootstrap check rebuilt the assembler/linker from raw C entirely
inside Fuse-X, using the portable frontend, Ripple backend, assembler and
linker. That resulting RXE then ran under Riplix/RVM, assembled and linked
another program, and executed it successfully. This verifies a guest-built
generation of these tools. The complete generation described below extends
that check to the frontend, preprocessor, backend and driver as well.

Maintained C now follows a shared formatting policy, with named assembly/link
phases and a source map in the implementation README. Vendored upstream code
remains separate. Tagged null-pointer interoperability is covered explicitly:
the portable backend must interpret the SDK's null tag and resolve GP/SB tags
before pointer-to-integer conversions.

Guest build coordination now uses the existing MWC Make and Picol Tcl subset.
Make supports phony targets, optional includes, conditional/append assignments,
complete prerequisite lists and failed-output cleanup. Tcl adds file metadata
and path operations, globbing, flat dictionaries, environment variables, list
argument expansion and streamed child output with exit-status reporting.
Both kernels now accept a combined 4,096 C16 characters of exec arguments,
environment strings and pointer vectors, replacing the old block-sized limit.
Concurrent exec and oversize-rejection checks pass in both guests.

Compiler packages include `/usr/src/rcc` with raw sources, a Makefile and
`bootstrap.tcl`. It can build complete Ripple tool generations, check compiled
programs and compare generations two and three. Source/SDK/seed identities and
flags invalidate cached outputs; unchanged builds retain their files. The
complete portable-generated compiler has executed and compiled working programs
under Riplix/RVM. One full generation now rebuilds entirely inside Fuse-X, and
an unchanged second build preserves every generated file. Those newly rebuilt
RXE tools then ran under Riplix/RVM, compiled and linked the toolchain and SDK
interoperability fixtures, and executed both successfully. The rebuild exposed
missing hexadecimal float conversion and excessive small-allocation overhead;
compiler-local support, IR slabs, shared values/names and compact assembler
symbols address those cases within the existing guest heap. Compiler-local
buffered I/O reduces call overhead. A native timer checkpoint also needed to
wait until the caller copied an aggregate return; a deterministic runtime test
and guest stress test cover that boundary. The complete guest-built
three-generation bootstrap remains unverified.

Further gates include correctly rounded floating-literal conversion (the SDK's
decimal parser can differ from the host by one ULP), response files for links
that exceed the exec argument budget, and rebuilding startup/libc from source.
