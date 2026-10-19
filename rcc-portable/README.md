# Portable C toolchain

The first implementation of the [self-hosting plan](../docs/self-hosting/README.md):
a cproc-derived C16 frontend and portable C preprocessor, emitting RCC IR
directly. The frontend, preprocessor and a baseline Ripple backend run inside
FUZIX/RVM and Fuse-X. A portable assembler, streaming linker and `cc` driver complete the Ripple
application pipeline inside either guest. Prebuilt SDK objects and native FXE
code generation remain dependencies; full compiler self-hosting and full C11
support remain later milestones.

One complete Ripple compiler generation now rebuilds from raw C inside Fuse-X
using guest Make, Tcl and the installed compiler tools. An unchanged second
build preserves its outputs. Those rebuilt RXE tools have run under FUZIX/RVM,
compiled and linked the toolchain and SDK interoperability fixtures, and
executed both successfully. Comparing subsequent compiler generations and
rebuilding the initial SDK remain separate acceptance gates.

## Build and use

Normal `scripts/run-fuzix.sh` and `scripts/run-fuse-x.sh` builds now install
`cc`, `rcc-c`, `rcc-ripple`, `rcc-as`, `rcc-ld` and the SDK automatically.
Inside the guest:

```sh
cc hello.c -o hello
./hello
```

The output is a **Ripple RXE executable**, runnable in FUZIX/RVM. Inside Fuse-X,
`cc` currently cross-compiles the same RXE format; native FXE generation still
requires the host compiler. Use the shared folder to transfer sources and results.
Headers, startup objects and libraries live under `/usr/lib/rcc`; no extra
environment variables are needed. `cc --bare hello.c -o hello.bin` instead
produces a standalone Ripple image.

To install the compiler without updating other programs, shut down the guest
and run:

```sh
python3 rcc-portable/package.py --os fuzix --install build/fuzix/fuzix.img
python3 rcc-portable/package.py --os fuse-x --install fuse-x/build/ROOTFS.IMG
```

The installer preserves user files and saves a backup before changing a disk.
It grows the root's inode table to at least 2,048 entries for the SDK.
`--no-build` installs already built artifacts. Normal launches cache build inputs
and compiler outputs; `--rebuild` forces a rebuild while preserving disk contents,
and `--run-only` skips both building and installation.

Build the existing backend tools (`rcc` and the native runtime), then:

```sh
python3 rcc-portable/build.py
python3 rcc-portable/cc.py rcc-portable/tests/strings.c --target native -o /tmp/portable-hello
/tmp/portable-hello

# Emit RCC IR, or lower it to Ripple assembly.
python3 rcc-portable/cc.py example.c --emit-ir -o example.ir
python3 rcc-portable/cc.py example.c -o example.asm
```

The wrapper accepts `-I`, `-D`, `-U`, `--preprocessed`, `--rcc` and `--rcpp`.
Portable preprocessing is the default; `--rcpp PATH` explicitly selects a host
preprocessor for comparison or bootstrap troubleshooting.
The frontend rebuilds when its inputs change. Native output is a standalone
host executable. IR can also
be supplied to the existing RCC linker alongside modules from other frontends.
The C executable `build/rcc-c` accepts raw C or preprocessed input directly,
plus `-I`, `-D`, `-U`, and `-E` for preprocessing output with line markers.

`upstream/cproc` is an unchanged pinned snapshot with its original licenses.
`adapt.py` generates local changes in `build/frontend`; `src` supplies the C16
target, driver and direct RCC IR backend. `preprocessor.c` replaces upstream's
incomplete macro engine; scanner adaptations add string tokenization and explicit
include boundaries. See [source provenance](UPSTREAM.md).

## Portable Ripple backend

```sh
python3 rcc-portable/build_ripple.py
python3 rcc-portable/cc.py example.c --backend portable -o example.asm
python3 rcc-portable/fuzix.py example.c --backend portable -o example.rxe

# Package the backend as either kind of guest executable.
python3 rcc-portable/build_ripple.py --os fuse-x
python3 rcc-portable/build_ripple.py --os fuzix

# Compile the backend's own sources using portable C -> IR -> Ripple assembly.
python3 rcc-portable/build_ripple.py --os fuzix --backend portable \
    -o rcc-portable/build/apps/rcc-ripple-self.rxe
```

`--backend portable` removes host Rust code generation from application
translation units. `fuzix.py` still uses the existing host assembler/linker and
prebuilt startup/libc; its SDK preparation may invoke the existing tools.
The default backend remains `rust`. Native Fuse-X code generation still uses
the existing backend.

Inside either guest, after copying the tools and appropriate headers:

```sh
rcc-c -I/include example.c -o example.ir
rcc-ripple --unit example example.ir -o example.asm
```

The second command emits **Ripple assembly in either environment**. Its FXE
package is `build/apps/rcc-ripple.fxe`; RXE packages are `rcc-ripple.rxe` and
`rcc-ripple-self.rxe`. `--unit ID` controls private-symbol namespaces; use the
same stable ID to compare generations or builds on different hosts. Without
it, the input filename supplies that identity. Content and identity hashes
keep private symbols from ordinary separately compiled units distinct.

The reader accepts the IR emitted by this portable frontend: integer operations,
casts, memory access, fixed allocas, cell-based GEPs, direct/indirect calls,
returns, branches, phi nodes, selects, global data and address initializers,
variadic intrinsics and semantic floating intrinsics. Arithmetic covers all
16/32/64-bit integer operations. Fat pointers normalize at 64,000-cell bank
boundaries. Floating operations call the existing softfloat SDK through the
standard ABI. Aggregate calls/results use the frontend's explicit copies.

This baseline gives each SSA value a fixed stack home, stages parallel phi
copies on incoming edges, and saves the standard callee state. It prioritizes
ABI correctness over speed. Frames are capped at 32,000 cells; runtime checks
trap stack-bank exhaustion. Every function in an input unit is retained.
Dynamic alloca, general struct/index GEPs, named type definitions, extended
assembly and the complete general-purpose RCC IR grammar remain future work.
Unsupported constructs are diagnosed; this is not a drop-in replacement for
every input accepted by the Rust backend. The assembly retains metadata for the existing host assembler/linker. The
portable tools use the chunk packing and relocation format described below.
File output is published by rename only after successful generation.

## FUZIX and Fuse-X programs

These wrappers use the portable frontend for application sources and the
selected backend, then existing startup/libc and host packaging tools to create
a guest executable. Fuse-X currently uses the host native backend:

```sh
python3 rcc-portable/fuse_x.py example.c -o example.fxe
python3 rcc-portable/fuzix.py example.c -o example.rxe

# Build the frontend itself for either guest OS environment.
python3 rcc-portable/guest_build.py --os fuse-x
python3 rcc-portable/guest_build.py --os fuzix
```

Both accept multiple source files, `-I`, `-D`, `--compiler`, `--build` and
`--float-format`. FUZIX requires an existing MMU SDK build (normally
`build/fuzix`); Fuse-X defaults to `fuse-x/build`. Outputs replace an existing
package only after successful compilation and linking. No installed disk or
kernel is modified by these commands.

The resulting `build/apps/rcc-c.fxe` or `rcc-c.rxe` consumes **raw C or
preprocessed C** and writes textual RCC IR. After copying the compiler, source
and matching C16 SDK headers into the guest:

```sh
rcc-c -I/include --target ripple example.c -o example.ir
rcc-c -I/include --target native example.c -o example.native.ir
rcc-c -E -I/include example.c -o example.i
```

On Fuse-X it can run directly from a mounted Mac folder as
`/mnt/host/rcc-c.fxe`. RXE is the executable format for FUZIX under RVM. The
guest's execution environment and the IR target selected on the command line
are independent: either frontend build can emit either IR target. The portable
Ripple backend can lower Ripple IR inside either guest, and the portable assembler/linker can finish an RXE or bare Ripple image
inside either guest. The compiler does not install headers
or assume host filesystem paths at runtime; provide the C16 headers with `-I`.

Preprocessing covers quoted and angle includes, guards, `#pragma once` (by
resolved path spelling), object/function/variadic macros, recursive rescanning,
stringizing, token pasting including empty arguments, `#undef`, conditionals,
`defined`, short-circuiting 64-bit signed/unsigned conditional expressions,
`#line`, `#error`, `#warning`, `__FILE__`, `__LINE__` and `__COUNTER__`.
Include lookup uses the physical source location even after `#line`; conditionals
cannot cross include boundaries. This is not complete preprocessor conformance:
trigraphs/digraphs, `_Pragma`, date/time predefined macros, prefixed character
constants in `#if`, GNU named variadics and comma-swallowing remain unsupported.
Include nesting and nested macro-argument expansion are capped at 128 and 256.

The compiler builds include local `strtoull` and hexadecimal `strtod` support and a generated copy of
the SDK formatter with 64-bit integer formatting enabled. Formatter and
conflicting utility symbols are namespaced within these packages; the shared
SDK sources are unchanged. `--metrics` reports the peak live allocation requests
made by compiler code, in C16 cells (two physical octets each). This excludes
SDK allocations, allocation headers, stack, globals and code; it is not total
process memory.

Compiler-local `getc`/`putc` macros bypass function calls for buffered characters.
Refill, pushback, mode changes and flushing use the SDK functions. Conversion and
stdio tests run under both guests, including 16-bit characters and single
evaluation of the output character. The backend lexer also reuses its token
buffer and immutable scalar types to reduce guest allocator traffic.

Numeric conversion still needs a correctly rounded implementation for complete
host/guest floating-literal parity. For example, the SDK's decimal conversion
rounds `9223372036854775808.0` one ULP above the host result. The hexadecimal
bootstrap helper avoids mantissa overflow but retains only 14 hex digits;
exhaustive rounding/conformance coverage remains pending. Bootstrap probes use
exact bit patterns when checking large hexadecimal constants.

## Portable assembler, linker and driver

```sh
# Convert the existing SDK objects/archives into the compact portable format.
python3 rcc-portable/sdk.py --float
python3 rcc-portable/build_tools.py
python3 rcc-portable/build_tools.py --os fuzix --backend portable
python3 rcc-portable/build_tools.py --os fuse-x
```

The outputs are `build/rcc-{as,ld,cc}` and matching `.rxe`/`.fxe` packages in
`build/apps`. Install `rcc-cc` as `cc`, alongside `rcc-c`, `rcc-ripple`,
`rcc-as` and `rcc-ld` on the guest PATH. Copy the contents of `build/sdk` to
`/usr/lib/rcc`, or set `RCC_SDK` to its guest location. `RCC_TOOLS` selects a
separate tool directory; `--sdk` and `--tools` override these paths.

```sh
# Inside the guest: every compilation stage is a guest process.
cc hello.c -o hello
cc -c library.c -o library.rpo
cc hello.c library.rpo -o hello

# The stages can also be invoked separately.
rcc-c -I/usr/lib/rcc/include hello.c -o hello.ir
rcc-ripple --unit hello hello.ir -o hello.asm
rcc-as hello.asm -o hello.rpo
rcc-ld /usr/lib/rcc/entry.rpo hello.rpo /usr/lib/rcc/libc.rpa -o hello
chmod +x hello
```

The driver supports `-I`, `-D`, `-U`, `-E`, `-S`, `-c`, multiple translation
units, explicit object/archive inputs, and `-v`. `--bare` produces a bare
Ripple image instead of a FUZIX executable. Native Fuse-X tools currently
**cross-compile Ripple RXE programs**; independent native FXE emission is still
future work. The Python packaging wrappers remain host bootstrap tools.

Objects use RPO1 and archives use RPA1, rather than the existing host JSON
formats. `sdk.py` converts those formats once during bootstrap. `rcc-ld
--archive -o library.rpa a.rpo b.rpo` builds portable archives inside the guest.
Archive members are selected by unresolved symbol dependencies. The linker
keeps headers and symbols in memory and streams instruction/data payloads, so
a large SDK does not have to fit in the guest heap.
The assembler stores symbols in slabs and shares the repeated compilation-unit
prefix of generated private labels. Comparisons and unresolved references
preserve the full label spelling; object and executable formats are unchanged.

Each object is independently packed into 256-instruction chunks. The first
254 slots hold payload; the remaining two support an absolute jump to the next
chunk. Conditional branches expand into an inverse short branch plus an
absolute jump. Because 64,000 is divisible by 256, adding an object's output
base preserves bank-safe control flow without a whole-program address map.
The linker emits the existing RXE1 or RLINK executable formats, resolves
symbolic addresses and synthesizes the global-initializer dispatcher.

The assembler accepts the portable backend's instruction/directive subset,
including symbolic branches, `.code`, `.data`, `.space`, octet `.word`, and
code/data projections. Numeric branch displacements and the complete legacy
assembler grammar are not supported. Object inputs are validated and outputs
are published by rename after successful generation.

## Finding and extending the C implementation

| Location | Responsibility |
| --- | --- |
| `preprocessor.c` | Include handling, macro expansion and conditional directives. |
| `src/main.c`, `src/target.c` | Frontend invocation and the C16 target layout. |
| `src/ir.c` | C expressions/statements, ABI lowering and textual RCC IR emission. |
| `ripple/parse.c` | RCC IR reader and its owned in-memory representation. |
| `ripple/emit.c` | Stack layout, pointer ABI, calls, instruction selection and globals. |
| `ripple/helpers.c` | Arithmetic helper assembly for 16/32/64-bit operations. |
| `asm/tools.h`, `asm/common.c` | Shared object model, wire encoding and file handling. |
| `asm/assemble.c` | Symbol collection and assembly-to-object scans. |
| `asm/link.c` | Archive selection, layout, validation and streamed output stages. |
| `driver/cc.c` | Options and orchestration of guest compiler processes. |

`python3 rcc-portable/check_style.py` checks maintained sources; add `--format`
to apply the style or `--clang-format PATH` to select a formatter.
`.clang-format` defines the local C style: four-space indentation, 100-column
lines and expanded control flow. Apply it to maintained C sources, keeping
`upstream/` unchanged. Prefer named stages and small helpers over compressed
expressions; document ABI, ownership and file-format invariants near their
implementations. Unsupported inputs should produce a diagnostic and preserve
an existing output file.

Remember that guest C uses 16-bit `int` and 32-bit `long`/`size_t`. File formats
use explicit little-endian octets, not raw C structs, and addresses count C16
cells. Runtime pointer tags and compile-time bank tags are separate concepts:
the SDK's null-bank tag is 65533, whereas concrete bank zero remains valid.

## Current coverage

Execution tests cover both native and Ripple backends:

- C16 sizes, sequential aggregate layout, signed char, integer ranks and
  promotions, enum constants, 16/32/64-bit arithmetic and conversions.
- Binary32/binary64 arithmetic, comparisons, constants and numeric conversions.
- Floating negation and truth conversion, including signed zero and NaN payloads.
- Functions, scalar and aggregate arguments/results, recursion, local callbacks,
  separate compilation and linking with the existing RCC frontend.
- Variadic definitions/calls, default argument promotions, `va_start`, `va_arg`,
  `va_copy` and `va_end`, including pointer, 64-bit and floating arguments.
- Conditionals, short circuiting, loops, switch, break/continue and goto.
- Arrays, structs, designated initializers, local compound objects and strings
  stored as sixteen-bit cells, including sixteen-bit character escapes.
- Pointer arithmetic, field access and pointer subtraction across bank
  boundaries, plus explicit pointer/integer casts.
- Static address initializers: strings, arrays and members, function pointers,
  nested tables, circular structures, static locals and bank-crossing offsets.
- Volatile loads/stores, `_Static_assert`, `_Generic`, and ordinary `_Alignof`.

Structs and unions follow RCC's existing ABI: arguments are pointers to copies,
and the callee makes its own local copy. Aggregate results return an object
pointer, which the caller immediately copies into its own frame before another
call can reuse the returned storage. Tests cover nested calls, recursive
returns, union returns, member access on returned objects, aggregate conditionals,
volatile objects/members and copies across memory banks. Structural return
signatures allow linking these functions with RCC in either direction.

Variadic calls use RCC's existing packed argument area and `va_list` cursor.
Aggregate extras keep the pointer carrier used by aggregate arguments; scalar
promotions follow C16 rules, and `float` extras become `double`. Global
initializers preserve typed scalar fields and symbolic addresses until link
time, including addresses embedded inside arrays, structs and unions.

This coverage is an initial vertical slice, not a conformance claim. Pending
lowering includes bitfield access, VLAs, aggregate constant copies and overlapping
string/designated initializers, extended alignment, prefixed character/string
encodings, long double, inline assembly and further builtins. Atomics and thread
storage require target/runtime contracts. These paths fail with diagnostics
instead of being advertised as working C11 support.

## Guest source builds

Normal compiler packages now include an initial source snapshot at
`/usr/src/rcc`, its explicit Makefile and a Tcl build coordinator. After updating
the image with the normal launcher, build one complete Ripple compiler generation:

```sh
tcl /usr/src/rcc/bootstrap.tcl build /tmp/rcc-stage1
```

On Fuse-X, use `/mnt/host/rcc-stage1` to retain output in the shared Mac folder.
These builds produce RXE programs for FUZIX/RVM; Fuse-X cannot execute them yet.
`tools` builds only the assembler/linker and compiler driver. `check OUTPUT`
uses `OUTPUT/bin` to compile and execute the toolchain and SDK interoperability
fixtures, so run it under FUZIX/RVM. All stages run ordinary guest processes;
Python and host compiler services are absent from this guest build path.

```sh
# FUZIX/RVM: three generations, program execution and stage-2/stage-3 comparison.
tcl /usr/src/rcc/bootstrap.tcl bootstrap /tmp/rcc-bootstrap
```

The three-generation command is the acceptance workflow, not a claim that this
full bootstrap has passed. The initial SDK remains host-prepared; rebuilding
libc/startup and adding native FXE code generation are subsequent milestones.
Compiler-sized rebuilds remain slow with the baseline backend.

The coordinator keeps unchanged outputs and records source/SDK/installed-seed
identities, seed tool mtimes/sizes and `CFLAGS`. Changed configuration discards its generated objects and tools,
including when updates share the same one-second guest timestamp. Ordinary
source edits use Make's timestamps; use a fresh output directory to force a
complete rebuild. Output and seed paths must not contain whitespace, because
the Makefile represents prerequisites as space-separated words.

The snapshot is generated from maintained sources and adapted cproc files by
`source.py`, with upstream license notices retained. Edit maintained repository
sources for permanent changes: a later image update refreshes this snapshot.
SDK and seed tools reside at `/usr/lib/rcc` and `/bin`; `RCC_SDK` and the optional
third positional argument select alternatives. This is the documented Picol Tcl
subset (`man tcl`), with build helpers added as needed rather than full Tcl/Tk.

## Verification

```sh
# Both backends; requires runtime/crt0.pobj, runtime/libruntime.par,
# src/ripple-asm/target/release/{rasm,rlink} and target/release/rvm.
python3 rcc-portable/test.py

# Host native backend only.
python3 rcc-portable/test.py --native-only

# The portable Ripple backend plus calls to/from Rust-generated objects.
python3 rcc-portable/test.py --portable-backend

# Backend execution, exact guest assembly parity and the bootstrap RXE.
python3 rcc-portable/test_ripple.py

# Portable assembly/linking, bank boundaries, archives and complete guest builds.
python3 rcc-portable/test_asm.py
python3 rcc-portable/test_toolchain.py
# Check packages installed by the normal image builders, using default PATH/SDK.
python3 rcc-portable/test_toolchain.py --skip-build --installed
# Rebuild tools inside Fuse-X, then run those tools under FUZIX/RVM.
python3 rcc-portable/test_toolchain.py --bootstrap-hosted
# Slower: also perform the rebuild itself under FUZIX/RVM.
python3 rcc-portable/test_toolchain.py --guests fuzix --bootstrap

# Build helpers and source/configuration invalidation.
python3 ports/fuzix/tools/test_build_tools.py --guests all
python3 rcc-portable/test_source.py
python3 rcc-portable/test_support.py
# Slow: a complete compiler generation inside a disposable Fuse-X guest.
python3 rcc-portable/test_bootstrap.py --workdir /tmp/rcc-guest-build
# Execute that RXE generation and compile/run fixtures in a private RVM guest.
python3 rcc-portable/test_bootstrap.py --verify-rvm /tmp/rcc-guest-build/share/stage1 --workdir /tmp/rcc-generation-check

# Preprocessor regressions, Clang token comparisons and raw compiler sources.
python3 rcc-portable/test_preprocessor.py

# Real utilities and the compiler itself in disposable guest installations.
python3 rcc-portable/test_hosted.py
python3 rcc-portable/test_rvm.py
```

The complete toolchain checks compile and run a program with aggregates,
variadic calls, 64-bit arithmetic and bank-crossing globals inside FUZIX.
They also check SDK tagged-null interoperability. `--bootstrap-hosted` rebuilds
the assembler/linker entirely inside Fuse-X, then executes that resulting RXE
under FUZIX to assemble, link and run another program. The verified guest-built
tool is saved as `build/apps/rcc-as-guest.rxe`. This is a rebuilt assembler/linker
generation. `test_bootstrap.py` extends this to the full compiler, backend,
assembler/linker and driver, including an unchanged incremental rebuild.
The three-generation compiler comparison remains pending.
The full `test_bootstrap.py` generation and its `--verify-rvm` execution check
have passed: every compiler stage rebuilt in Fuse-X, then the resulting tools
compiled and executed both fixtures in FUZIX/RVM.

Tests use disposable output directories and disks. They verify nonzero main
exit statuses, mixed-frontend scalar/aggregate/variadic ABI linking and global
relocations, existing RCC regressions, and rejection of unsupported features
without replacing an existing output. Interoperation runs on both backends with
the portable frontend as caller and as callee. Tests do not modify FUZIX/Fuse-X disks.
Backend regressions also check inlined stack homes, full-width integer unary
operations and values whose defining block appears later in the input layout.

The hosted checks build and run upstream `cat`, `cmp`, `sort`, `grep` and `sum`,
exercise shared-folder I/O and fork/exec from portable-frontend code, and test
the compiler's extra libc support. Guest compiler output is compared byte for
byte with host compiler output, then linked and executed. Inputs include
aggregate, variadic, relocation and bank-crossing fixtures and, on Fuse-X, the
compiler's own expression parser. Both guest tests also compile raw C using
guest-resident headers, compare direct compilation with an `-E` round trip, and
execute the resulting code through the host backend. Fuse-X additionally compiles
the expression parser from raw C. Reports and transcripts are saved under the
ignored `build` directory. The tests leave the user's disks untouched.

Function-local IR objects use an arena, released after each function. This
avoids thousands of small allocator operations in the guest. On the measured
Fuse-X expression-parser runs, compiler allocation requests peaked around
352,000 cells for preprocessed input and 446,000 cells for raw C with headers;
these are workload measurements, not worst-case limits.
The portable Ripple reader retains a module until emission. Its nodes use
zeroed slabs, names and operation strings are interned, and immutable SSA
references share nodes. This reduces the memory required for compiler-sized
inputs without changing generated code. Output filenames and temporary call
scratch buffers remain individually allocated and freed.
SDK rebuilding, subsequent compiler bootstrap generations and independent native
code generation remain the next stages. Large in-guest rebuilds remain slow with
the baseline backend. Compiler-local buffered I/O avoids a resumable function
call for every character. The Ripple backend needs register allocation, broader workloads
and richer IR support. The portable preprocessor
also needs broader application coverage and the conformance additions listed above.
