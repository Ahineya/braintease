# Self hosted development toolchain

Decision: build a portable C toolchain around a cproc-derived frontend that
emits RCC IR. Preserve the existing C16 data model and Ripple/native targets.
The native guest-toolchain priority is now freestanding x86_64 Riplix; hosted
Fuse-X output follows later. Full capabilities and independent guest builds are the
destination; restricted early implementations are milestones, not a permanent
language subset.

The C16 contract remains the basis of the implemented bootstrap stages below.
The accepted [native x64 migration](../arch/native-x64.md) adds a separate LP64
application ABI, then a native kernel build, while retaining Ripple/C16.
Conventional x64 applications initially use host Clang; this lets native process
and syscall acceptance proceed independently of RCC/RFL layout changes.

Hosting RFL is also distinct from its output ABI: a cross-compiled native RFL
CLI can initially emit the existing compatibility IR and use guest backend
tools. It needs Rust platform/standard-library support and feature separation;
the current Rust-to-RCC prototype is not a general compiler-hosting solution.
Running that CLI does not require guest rustc/Cargo. Rebuilding RFL itself
inside the guest requires a further Rust toolchain milestone.

## Architecture

Application distribution is separate from compiler bootstrapping. The accepted
[package-management plan](../packages/README.md) adds shared host/guest package
creation and repository installation, so individual application updates do not
require reinstalling a complete OS image. It preserves explicit target/ABI
identity while the native x64 migration proceeds.

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
4. **Independent native x86_64 toolchain, then hosted Fuse-X.** Lower RCC IR
   through the checked C16 continuation ABI into QBE, followed by a guest x86
   assembler and linker. Build and execute native Riplix applications without
   host RCC, Cranelift or platform linker services. Exercise process exit,
   scheduling, fork, signals, exec and applications. Hosted native output and
   GUI acceptance follow the x86 milestone.
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

## Native x86 feasibility milestone

Latest OS milestone (2026-10-20): a guest-built C frontend compiles the current
71-unit C16 kernel/embedded-program inventory inside x64 Riplix. Its IR matches
the host reference exactly and boots successfully after host backend emission
and linking with recorded native platform objects. Process, pipe, shell, timer
and two-boot persistence checks pass. This does not yet replace the host kernel
backend, platform objects, independent application packages or EFI/image tools.
See the [guest-assisted OS bootstrap and remaining gates](../../rcc-portable/native-sdk/OS_BOOTSTRAP.md).

The portable frontend, an initial RCC IR-to-QBE lowering tool, C16 QBE 1.3,
a standalone ELF64 x86 assembler, an RXE1 linker and a native driver now have
freestanding guest builds. QEMU acceptance runs raw C with headers/macros
through all stages, final linking and application execution inside the x86
guest. `rcc-native-cc source.c -o program` orchestrates this path; seed tools,
kernel and SDK are still host-built. The installed Ripple `cc` stays unchanged.
Relocatable object metadata, separate C compilation, cross-file callbacks and
logical globals/initializers are now implemented. Archive creation/selection and packed numeric initializers are implemented. Full
language/SDK integration and rebuilding native compiler tools remain the next
gates. See the
[implementation and commands](../../rcc-portable/README.md#native-x86-feasibility-tools).

The lowering retains logical memory, function tokens, compact live continuation
slots, parameter reloads and loop checkpoints. This is a scalar/FP/control-flow
vertical slice with relocatable globals and scalar/array/struct/string initializers;
variadic scalar calls/definitions and va_list are implemented; aggregate
call/SSA lowering and numeric FP conversions remain explicit errors. Its minimal startup uses the real process
exit syscall. It does not activate cproc's ordinary platform ABI backend or
claim an LP64 migration. QBE supplies native instruction selection/register
allocation below these platform rules.

Broader performance work must add portable IR optimization and direct leaf
calls, not just tune QBE: checked logical-memory helpers conceal scalar-local
promotion opportunities from an ordinary native optimizer. Retain the current
host backend as a correctness/performance comparison. Full self-hosting also
needs the bootstrap compiler defects and numeric-conversion gaps listed in the
implementation README resolved, plus larger real translation units.

The native driver accepts `-c`, multiple sources and reusable ELF objects.
`RCCMOD01` metadata survives QBE/assembly inside each object; the linker assigns
final function tokens and global addresses, scopes private names by owning
object, validates represented IR ABIs and emits existing RXE1 initializer rows.
Only the unit defining main supplies startup. Globals retain the existing small
bank-1 or large bank-32..47 layouts; function initializer tokens are remapped by
the existing loader. There is no new executable/runtime ABI. Aggregate source
declarations are erased to storage extents in RCC IR, so aggregate ABI checking
can validate those extents, not complete source field/pointee/qualifier types.

Packed numeric initializer runs and C16-aware archive extraction now remove
much of the data overhead and keep unused library members out of executables.
The guest can create deterministic ordinary `.a` files and link them without
host services. Main startup accepts argc/argv/envp using the existing kernel
argument block. A 4,096-cell table's object shrank from 300,696 to 14,048 octets.

The native variadic ABI now uses caller-owned packed C16 cells and a hidden
logical cursor, matching the existing continuation ABI. Mixed scalar/pointer/
callback/64-bit/double tails, default float promotion, copied and forwarded
lists, recursion, empty tails, more than 64 extra arguments and bank boundaries
have focused coverage. Function metadata distinguishes variadic declarations
from an explicit pointer parameter.

`rcc-portable/native-sdk` prepares a source SDK, its guest Makefile, ordinary
`libc.a` and three startup variants. `rcc-native-cc --sdk DIR` selects its headers,
existing bounded heap profile and libc startup. The entry initializes stdio and
arguments/environment, then uses libc exit/atexit flushing. The syscall bridge
yields the existing process request; no new runtime import or package ABI is
introduced. Dead CFG blocks and their phi inputs are pruned before lowering,
and the scanner normalizes CRLF before line splicing.

Host assessment compiles all 78 bootstrap libc/startup units and all 22 native
tool units (with the existing private strtoull declaration shim). This is not
full libc or a compiler self-rebuild. Math/decimal floating formatting, numeric
conversions, aggregate lowering and nonlocal jumps remain coverage gates. The
private QEMU SDK workflow rebuilds every listed raw source, creates the archive,
links and executes programs, compares host/guest linking of identical objects
and reexecutes after a fresh boot. Reports state completion separately from
host bootstrapping.

That complete bootstrap SDK acceptance now passes: all 78 units are rebuilt
from raw C in x86, archived and used by executed programs, including process,
variadic, argument, cell-memory and qsort checks. Fresh-boot execution and
independent same-object host-link parity pass. The frontend now buffers and
closes included headers, so include nesting no longer consumes one open stream
per header; the nested-include regression passes in the real guest as well.

`native-sdk/source_tools.py` now prepares a movable source bundle and guest
Makefile for four native tools, including the parser conversion support. Its
23 translation units build through the portable pipeline on the host and give
identical executables to the four already run in x86. Guest Make header lists
use bounded stamp groups, preserving incremental builds without its macro
buffer overflow. Full native guest source rebuilding and generation comparison
remain acceptance gates. Native installation and kernel rebuilding remain
subsequent milestones. Native ELF storage and logical C16 globals remain
separate even though both have relocation/initialization support.

Actual x86 guest Make also passes both generated rule sets and rebuilds a
working archiver from raw C against the bootstrap SDK. It creates a library,
does no work on an unchanged rebuild and executes after a fresh boot. The
remaining tool stages and subsequent generations still need guest source
rebuilding; this single-tool check does not establish full self-hosting.
