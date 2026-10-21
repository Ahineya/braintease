# Portable C toolchain

The first implementation of the [self-hosting plan](../docs/self-hosting/README.md):
a cproc-derived C16 frontend and portable C preprocessor, emitting RCC IR
directly. The frontend, preprocessor and a baseline Ripple backend run inside
Riplix/RVM and Fuse-X. A portable assembler, streaming linker and `cc` driver complete the Ripple
application pipeline inside either guest. An experimental x86 pipeline also
compiles, links and executes scalar C inside freestanding Riplix. Prebuilt SDK
objects and seed tools remain dependencies; full compiler self-hosting and full C11
support remain later milestones.

One complete Ripple compiler generation now rebuilds from raw C inside Fuse-X
using guest Make, Tcl and the installed compiler tools. An unchanged second
build preserves its outputs. Those rebuilt RXE tools have run under Riplix/RVM,
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

The output is a **Ripple RXE executable**, runnable in Riplix/RVM. Inside Fuse-X,
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

## Riplix and Fuse-X programs

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
`--float-format`. Riplix requires an existing MMU SDK build (normally
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
`/mnt/host/rcc-c.fxe`. RXE is the executable format for Riplix under RVM. The
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
Included files are buffered and closed immediately, so active include depth
does not consume one stdio stream/file descriptor per header. Named root input
and stdin retain streaming input. Regression coverage includes 32 nested
includes under a 12-descriptor host limit, large CRLF headers and read errors;
the focused nested-include probe also passes in x86 with the corrected seed.

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
Ripple image instead of a Riplix executable. Native Fuse-X tools currently
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
| `native/` | Checked C16 RCC IR-to-QBE continuation lowering. |
| `qbe/` | Pinned QBE's C16 adaptation and guest acceptance. |
| `x64/` | Standalone x86 assembler and ELF64 object emission. |
| `x64-link/` | ELF validation, symbol resolution, relocations and RXE1 output. |
| `native-driver/` | Experimental x86 guest build orchestration. |

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
- Signed/unsigned bitfield reads, updates, neighboring-field preservation and
  automatic/static initialization across C16 storage cells.
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
lowering includes VLAs, aggregate constant copies and overlapping
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
These builds produce RXE programs for Riplix/RVM; Fuse-X cannot execute them yet.
`tools` builds only the assembler/linker and compiler driver. `check OUTPUT`
uses `OUTPUT/bin` to compile and execute the toolchain and SDK interoperability
fixtures, so run it under Riplix/RVM. All stages run ordinary guest processes;
Python and host compiler services are absent from this guest build path.

```sh
# Riplix/RVM: three generations, program execution and stage-2/stage-3 comparison.
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
# Rebuild tools inside Fuse-X, then run those tools under Riplix/RVM.
python3 rcc-portable/test_toolchain.py --bootstrap-hosted
# Slower: also perform the rebuild itself under Riplix/RVM.
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
variadic calls, 64-bit arithmetic and bank-crossing globals inside Riplix.
They also check SDK tagged-null interoperability. `--bootstrap-hosted` rebuilds
the assembler/linker entirely inside Fuse-X, then executes that resulting RXE
under Riplix to assemble, link and run another program. The verified guest-built
tool is saved as `build/apps/rcc-as-guest.rxe`. This is a rebuilt assembler/linker
generation. `test_bootstrap.py` extends this to the full compiler, backend,
assembler/linker and driver, including an unchanged incremental rebuild.
The three-generation compiler comparison remains pending.
The full `test_bootstrap.py` generation and its `--verify-rvm` execution check
have passed: every compiler stage rebuilt in Fuse-X, then the resulting tools
compiled and executed both fixtures in Riplix/RVM.

Tests use disposable output directories and disks. They verify nonzero main
exit statuses, mixed-frontend scalar/aggregate/variadic ABI linking and global
relocations, existing RCC regressions, and rejection of unsupported features
without replacing an existing output. Interoperation runs on both backends with
the portable frontend as caller and as callee. Tests do not modify Riplix/Fuse-X disks.
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
inputs without changing generated code. Immutable pointer, array and structural
types are also shared; equal storage size alone does not merge different types.
Output filenames and temporary call
scratch buffers remain individually allocated and freed.
SDK rebuilding, subsequent compiler bootstrap generations and full native
language/SDK support remain the next stages. Large in-guest rebuilds remain slow with
the baseline backend. Compiler-local buffered I/O avoids a resumable function
call for every character. The Ripple backend needs register allocation, broader workloads
and richer IR support. The portable preprocessor
also needs broader application coverage and the conformance additions listed above.

## Native x86 feasibility tools

The first native output target is **freestanding x86_64 Riplix**; hosted Fuse-X
output is deferred. The experimental pipeline is:

```text
raw C -> rcc-c --target native -> RCC IR -> rcc-qbe -> QBE IL
                                                         |
                                                        qbe
                                                         |
                                              x86_64 AT&T assembly
                                                         |
                                                    rcc-x64-as
                                                         |
                                                   ELF64 object
                                                         |
                                                    rcc-x64-ld
                                                         |
                                               x86_64 RXE1 executable
```

Every stage runs as an ordinary x86 guest process, including final linking and
packaging. `rcc-native-cc` orchestrates the complete path without Python, host
RCC, Cranelift or a host platform linker during the guest build. The initial
tools/kernel/SDK are bootstrapped with existing host compilers. Full native SDK
support and compiler self-rebuilding remain subsequent gates. The working
Ripple `cc` default is unchanged. These
commands create private outputs, without updating a user's image:

```sh
python3 rcc-portable/guest_build.py --os x86_64 --metrics \
  --build build/guest-native-assessment/x64-sdk
python3 rcc-portable/native/build.py --guest
python3 rcc-portable/qbe/build.py --metrics --heap-profile bounded \
  --linker tools/x86_64/app_link.py --build build/guest-native-assessment/x64-sdk \
  -o build/guest-native-assessment/qbe-x86.fxe
python3 rcc-portable/x64/build.py --guest --heap-profile bounded \
  --linker tools/x86_64/app_link.py --build build/guest-native-assessment/x64-sdk \
  -o build/guest-native-assessment/rcc-x64-as-x86.fxe
python3 rcc-portable/x64-link/build.py --guest
python3 rcc-portable/x64-link/build.py --guest --driver
python3 rcc-portable/x64-link/build.py --guest --archiver

# Host reference/oracle tools; SDKROOT can select an installed macOS SDK.
python3 rcc-portable/qbe/build.py --host
python3 rcc-portable/native/build.py
python3 rcc-portable/x64/build.py
python3 rcc-portable/x64-link/build.py
python3 rcc-portable/x64-link/build.py --driver
python3 rcc-portable/x64-link/build.py --archiver

# Fresh private QEMU image: raw C through native linking and execution
# entirely in the guest; includes the single-command driver.
python3 rcc-portable/qbe/test_x86.py
```

Inside that test guest:

```sh
rcc-c --target native example.c -o example.ir
rcc-qbe --relocatable example.ir -o example.ssa
qbe -t amd64_sysv -o example.s example.ssa
rcc-x64-as example.s -o example.o
rcc-x64-ld example.o -o example
./example

# Or use the guest driver, with those five stage tools on PATH.
rcc-native-cc example.c -o example
./example

# Independent compilation, followed by linking reusable objects.
rcc-native-cc -c main.c -o main.o
rcc-native-cc -c parser.c -o parser.o
rcc-native-cc main.o parser.o -o example
# Sources and existing objects can also be mixed in a single invocation.
rcc-native-cc main.c parser.o -o example

# Create a library, then extract only members needed by preceding inputs.
rcc-x64-ar -o libparser.a parser.o lexer.o
rcc-native-cc main.o libparser.a -o example
```

QBE takes options before input filenames. The guest linker emits the existing
x64 RXE1 container, marks it executable according to umask and publishes it by
rename after successful generation. It accepts multiple ELF64 objects, local
and strong global symbols, PROGBITS/NOBITS, PC32/PLT32, aligned absolute native
pointers and absolute integer constants. Mergeable constants are retained
without deduplication. Runtime helper imports use an explicit name/arity table.
Narrow relocatable addresses, TLS, GOT, weak/COMMON symbols, section groups,
unknown relocations and malformed tables fail with diagnostics. Objects and
final packages are capped at 16 MiB, section alignment at 4,096 octets, and a
link accepts at most 256 selected objects. Selected objects retain symbol names
and compiler metadata, while allocated section payloads stream into
the final image from their original file/member spans. Overlapping input spans
share compact resident storage. A bounded checksum scan rejects input changes
before publication, and only one object is reopened at a time. Relocation
tables are buffered only while applying each table, verifying its saved
checksum before use. Parsing still
temporarily buffers one complete object and the final image remains resident;
the 16 MiB format cap does not promise that every such link fits a bounded heap.
Final image storage uses 64 KiB chunks so a fragmented heap does not need one
large contiguous free block. Copies and relocations may cross those boundaries;
native addresses and the executable format remain unchanged. Buffered chunk
writes also replace the former per-octet output loop. On the actual 28 QBE
guest objects, the instrumented host peak drops from 25,390,471 to about
14.2 million bytes and the complete executable remains byte-identical.
Archives are
catalogued one member at a time, retaining export names/file spans rather than
all unused ELF data.

`--relocatable` emits versioned `RCCMOD01` records in a local QBE data object.
QBE and the ordinary assembler preserve this data in ELF; each `.o` is
self-contained. The wire contract is in `native-format.h`. It includes
definitions/references, scalar function ABI signatures, global storage extents,
initializers and an optional startup record. ELF-local scope and owning-object
metadata keep `static` functions and globals distinct between translation units.
Without an SDK, only the unit defining `main` emits minimal startup. With
`--sdk`, units use `--no-start` and the linker selects the matching libc startup.
The linker assigns final function
tokens and global addresses, resolves cross-file calls/callbacks, checks ABI
signatures and patches internal table slots used by generated code. It rejects
undefined/duplicate definitions, incompatible ABIs, missing/multiple entries,
malformed metadata and overlapping initializers before publishing output.

Scalar ABI checking retains IR kinds/widths and parameter counts; integer
signedness and pointer pointee types are erased by this IR. Aggregate external
declarations are flattened cell arrays, while definitions describe initializer
storage chunks. Their checked contract is the storage extent (`m<cells>`),
not erased source C fields or qualifiers. Incomplete external arrays (`m0`)
receive their extent from the defining unit. This is ABI validation, not a
complete source C type-compatibility checker. Unused external declarations do
not create imports; definitions in supplied objects are retained. Archive member
selection is implemented; function/section garbage collection remains future work.

Logical globals use the existing bank-1 region when they fit, otherwise banks
32..47, up to 1,024,000 cells including the reserved first cell. Zero storage
and typed scalar/array/struct/string initializers become existing RXE1 template
records, including cross-file data addresses, cyclic addresses and function
tokens. Native ELF data remains separate from logical C16 storage. The existing
loader remaps function initializers and restores each program's global template.
No new runtime import or executable ABI is required.
Numeric initializers use packed `NM_CELLS` runs: contiguous C16 cells are
little-endian octets after the record strings. Entire zero-valued scalars need
no payload; symbolic data/function pointers retain their typed records. The
linker expands runs into existing RXE1 rows of at most four cells, including
bank-crossing rows. Legacy per-scalar records remain readable. Bounded escaped
QBE byte strings and buffered writes avoid per-octet formatting calls.
A 4,096-cell initialized table with a zero tail shrank from 300,696 to 14,048
object octets, and from 399,465 to 39,097 executable octets. This is a size
comparison, not an execution-speed benchmark. The metadata payload still stays
in native ELF data as well as expanded RXE1 rows; eliminating that duplication
requires further layout/relocation work.

`rcc-x64-ar -o LIBRARY.a INPUT.o...` recreates a deterministic ordinary ar
container from the complete member list, with atomic output replacement. It
writes short SysV names and BSD extended names, rejects duplicate basenames,
and does not generate an index. It is a creator, not a general `ar rcs` CLI.
The linker accepts BSD extended names, GNU string-table names and ordinary
symbol indexes, including host-produced archives. It builds its own C16-aware
export catalog: native ELF indexes cannot describe logical global symbols.
Archives resolve references from earlier inputs, repeatedly extracting needed
members until that archive reaches a fixed point. Later inputs do not rescan
an earlier archive automatically; repeat the library when needed. Unused members
do not contribute code, globals, duplicate definitions or unresolved imports.
Malformed members are still rejected during cataloguing. Limits are 128 MiB per
archive, 4,096 members, 1,024-octet member names and a 1 MiB GNU string table;
thin archives, linker groups and `-l`/`-L` lookup are not implemented. The
container reference is [FreeBSD ar(5)](https://man.freebsd.org/cgi/man.cgi?query=ar&sektion=5).

For legacy standalone QBE probes, omitting `--relocatable` retains the earlier
whole-module output, and `rcc-x64-ld --metadata example.ssa example.o -o example`
supplies its explicit `# rcc-entry`/`# rcc-function` records. Legacy and embedded
metadata cannot be mixed. Legacy directive lines are limited to 1,023 characters
and symbols to 255; ordinary QBE lines are skipped without that limit. Padding inside the
opaque code blob preserves a page-aligned load bias despite eight-byte-aligned
RXE1 metadata, including both native-pointer fixups and aligned SSE constants.
`native/package.py` remains a host bootstrap/reference alternative.

The experimental driver accepts multiple C sources, ELF objects and `.a` libraries,
`-I/-D/-U`, `-o` and `-v`. `-c` compiles one source into a self-contained `.o`,
defaulting to its source basename. `-E`, `--emit-ir`, `--emit-qbe` and `-S`
also stop after one source. Use
`RCC_NATIVE_TOOLS` or `--tools` for an alternative guest tool directory.
It creates a private temporary directory, propagates stage failures and cleans
intermediates. `--sdk DIR` (or `RCC_NATIVE_SDK`) supplies headers, libc and startup, and
selects the existing bounded native heap profile. Without it the minimal
no-libc path remains available. Normal image installation is a separate step. The QEMU test installs seed
tools into its disposable image only.

The lowering preserves logical C16 pointers/memory, IEEE carriers at memory and
call boundaries, relocatable function tokens, stackless calls and loop
checkpoints. Only live non-parameters spill to dense continuation slots;
parameters reload from immutable frame arguments. Phi copies are parallel and
run on predecessor edges. Dominance validation rejects undefined/non-dominating
SSA uses. Dead CFG blocks are removed before metadata/frame analysis, including
dead phi inputs; malformed graph edges still fail validation. Minimal startup
exits through the real kernel syscall packet. SDK startup initializes libc,
forwards arguments/environment and runs exit handlers and stdio flushing.
Liveness snapshots are retained only after calls and checkpoint branches,
which consume them during continuation emission. Ordinary instructions no
longer retain an unused SSA-sized snapshot. On the captured frontend `pp.c`
IR this lowers the instrumented host allocation peak from 18,466,513 to
8,203,752 bytes with byte-identical QBE output. Host bytes are not C16 cells;
the same large lowering also passes in the bounded x86 guest.

The vertical slice supports scalar integer operations, checked loads/stores,
fixed logical frames, one cell-offset GEP, scalar direct/indirect calls,
branches, phis, selects, FP reinterpretation and binary32/binary64 arithmetic
and comparisons. Scalar guards preserve zero-divisor results, wrapping signed
division, masked shift counts and width-correct continuation reloads. Inputs
require `target native`; the linked program requires one `int main(void)`,
`int main(int,char**)`, or `int main(int,char**,char**)`. The arity-one startup
reads the existing kernel argument block through checked logical loads and
forwards argc/argv/envp. The SDK startup also initializes libc.
Relocatable units support logical globals and initializers. Integer-to-pointer
casts preserve the low 32 logical pointer bits, including null; memory helpers
still validate dereferences. Variadic calls/definitions use the existing packed C16
argument area: extra scalar carriers occupy 1..4 cells in the caller frame,
with a hidden logical cursor passed to the callee. `va_start`, `va_arg`,
`va_copy`, forwarding, recursion and float-to-double promotion are supported.
The signature suffix `...` distinguishes this ABI from an explicit pointer
parameter. The runtime still receives at most 64 arguments, while the packed
tail can contain more. Aggregate arguments/results use the frontend's existing
logical pointer carriers and explicit object copies. Relocatable call signatures
retain their structural aggregate layouts; equal-sized incompatible results
are rejected at link time. General aggregate SSA/load/store operations, dynamic
alloca and general GEPs remain explicit errors.
Pointer-to-linear, f32/f64 precision and numeric integer/float conversions are
supported. Float-to-integer lowering preserves RCC native's saturating contract:
NaN becomes zero, finite overflow/infinities clamp to the destination range,
and in-range values truncate toward zero. Unsigned 64-bit conversion splits at
2^63 before using QBE's signed conversion. Boundary tests use exact IEEE bits
to isolate conversion from the SDK decimal parser's known rounding limitation.
The analyzer accepts at most 1,024 input
blocks per function, and retains the runtime's 512 live slots/64 arguments.
These are milestone limits, not intended language restrictions.

A source SDK lives under `native-sdk/`. Prepare/build it with host reference
tools, then rebuild its listed raw sources inside the test guest:

```sh
python3 rcc-portable/native-sdk/build.py
python3 rcc-portable/native-sdk/assess_tools.py
python3 rcc-portable/native-sdk/assess_tools.py --link
python3 rcc-portable/native-sdk/test_tools.py
python3 rcc-portable/native-sdk/test.py
python3 rcc-portable/native-sdk/source_tools.py
```

The prepared directory contains headers, copied library sources (with original
notices), a manifest and a guest Makefile. In a writable guest copy, `make`
rebuilds `libc.a` and the three `crt0-N.o` startup variants; no Python is needed.
That workflow requires guest Make and its `mkdir`/`rm` utilities; the minimal
private x64 test image drives the same compilation steps from its test harness.
The harness shuts down and checks its private filesystem after each source
file, then saves the object with input and output hashes. Unchanged completed
units are reused after an interrupted run, independently of the worker count.
`RCC_TEST_ACCEL=kvm` explicitly selects Linux hardware acceleration; the default
is `tcg`. To validate on a separate Linux host, `--guest-only` leaves the report
at `guest-passed`; copy back its extracted outputs/report and run
`--verify-host-only` on the reference host to perform exact same-object linker
comparison before the final `passed` result. Neither mode installs system tools.
Use the result with `rcc-native-cc --sdk /path/to/sdk hello.c -o hello`. The
linker selects startup for main's 0, 2 or 3 parameters and rejects programs
already carrying minimal startup. Compile all reusable program objects with the
same SDK selection. No normal user image is installed by these commands.

This bootstrap SDK has 75 libc units and three startup units. It covers existing
stdio/integer formatting, allocation, strings, files, process and networking
wrappers, Unicode and a checked C fallback for bulk cell access. It deliberately
does not claim the math/decimal floating library or floating printf, setjmp/longjmp
and the complete upstream libc. Those need remaining lowering and runtime-service
coverage. Two source-copy corrections make `utime` match its const declaration
and explicitly cast the legacy `strrchr` result; shared SDK sources are unchanged.
The compiler still links its private 64-bit `strtoull` support explicitly.

The raw-source x86 acceptance now passes for all 78 units: guest archive
creation, stdio/process/exit-handler probes, mixed variadics, argc/argv,
bank-crossing cell operations, the installed qsort regression and fresh-boot
execution. Independent host linking of the same extracted guest objects gives
byte-identical programs. Evidence: `build/portable-native-sdk/guest-result.json`.
This closes the listed bootstrap SDK build gate, not complete libc coverage
or compiler self-hosting. The initial compiler tools, kernel and base image
remain host-bootstrapped.

`python3 rcc-portable/native-sdk/source_tools.py` prepares a movable source bundle
for the lowerer, linker, driver and archiver. The current 25 translation units
include numeric conversion support, chunked image storage and a relative parser
declaration shim. The accepted two-generation snapshot below predates those
additional units.
In the guest,
run `make SDK=/path/to/native-sdk TOOLS=/path/to/seed/bin`; outputs go under
`build/bin`. This step needs Make and `mkdir`/`rm`, and still uses seed frontend,
QBE and assembler executables. The earlier 23-unit Makefile passed a host build
through the portable C16 pipeline: all four outputs exactly match the tools
already executed in x64, and a second unchanged Make invocation does no work.
The SDK and tool Makefiles group header dependencies into small stamp targets
so they fit guest Make's 2,048-cell expansion buffer. Host-compiled guest Make
also passes both complete builds, unchanged rebuilds and SDK header invalidation.
The groups share a directory stamp, avoiding a separate directory-creation
process per group while retaining stable incremental timestamps.
The original archiver-only `native-sdk/test_make_guest.py` passes in a real
x86 guest: both rule sets parse, Make rebuilds the archiver from raw C, unchanged
Make does no work, and the rebuilt archiver creates a library and executes again
after a fresh boot. That test still uses bootstrap SDK/frontend/QBE/assembler
seeds. The complete four-tool source build and two-generation comparison now
pass as described below. Clean
before changing flags or selected SDK/tool paths.

The extended harness now selects all four tools by default. Each tool gets a
separate private guest build and a checkpoint after clean shutdown, filesystem
checking and byte-identical host linking of its extracted objects. Input hashes
cover the source bundle, SDK, compiler seeds and kernel/base images; missing or
changed outputs invalidate the checkpoint. A repeatable `--tool NAME` limits a
run to selected tools, and reports identify the remaining bootstrap tools.
The rebuilt set then compiles separate C units, archives, links and executes a
program, with a fresh-boot execution check and failed-output preservation checks.
The first complete four-tool guest source build passes for all 23 translation
units, including same-object host link parity for every executable. The rebuilt
set compiles separate C sources, creates an archive, links and executes a program,
preserves outputs on rejected input, and executes the program after a fresh
boot. Evidence and independent local verification are under
`build/mini-native-full/completed/full-stage1-reuse/`.
The linker/lowerer source builds succeeded before the old duplicate collection
step exhausted their test disks. Their intact outputs were recovered from
separate filesystem copies, checked with unchanged Make and guest execution,
then matched against independent host linking. The report retains the original
failure records, input key, log/disk hashes and filesystem-recovery details.
This used the previously guest-built bootstrap SDK snapshot; it does not claim
a refresh of subsequent shared-libc changes. Frontend, QBE, assembler and kernel
remain bootstrap seeds.

The second generation also passes: those four guest-built tools rebuilt all
23 source units, and every resulting executable is byte-identical to generation
one. Independent local linking of the extracted objects reproduces all four
executables. The rebuilt set passes compile/archive/link/execute, rejected-output
preservation and fresh-boot program execution. Evidence and local verification:
`build/mini-native-stage2/completed/full-stage2/`.
The lowerer initially ran out of test-disk space while writing its thirteenth
object. A separate 128 MiB filesystem copy resumed the last three units and
link; all 685 injected inputs and the twelve completed objects were verified
unchanged. Its checkpoint records the interrupted build, disk expansion and
preserved object hashes. Both generations use the same previously guest-built
SDK snapshot. This establishes stability for these four tools; frontend, QBE
and assembler source self-builds remain separate gates.

`native-sdk/source_seeds.py --tool rcc-c|qbe|rcc-x64-as --sdk SDK --output SOURCE`
prepares the remaining tools: 23 frontend, 28 SysV-only QBE and six assembler
translation units. Each bundle contains a movable Makefile, private formatter
and parsing support where required, and retained upstream license notices.
The frozen SDK is not replaced: decimal `strtod` and floating bit helpers are
ordinary private source units of the two compiler tools. The driver searches
explicit user `-I` directories before its SDK fallback, so these private headers
can supply the intended declarations and buffered stdio wrappers.
All three source bundles compile/link through the host portable C16 pipeline.
Real x86 source-build acceptance uses `native-sdk/test_seeds_guest.py`, with
explicit seed overrides, same-object host link parity, semantic execution and
fresh-boot checks. Semantic fixture copies are frozen at launch. This is a
first source generation for the selected tool, not a stable-generation claim;
the remaining tool seeds and their hashes stay explicit in each report.

The six-unit assembler source build now passes in x86, including same-object
host linking, aggregate/conversion/bitfield/pointer-difference execution and
combined compilation/archive/linking after a fresh boot. Evidence:
`build/mini-native-seeds/completed/overnight-v3/seed-assembler-v3/`.
The frontend stopped after six completed objects at the lowerer's liveness
allocation; QBE compiled all 28 units but exhausted memory in its final linker.
The fixes above address these two separate resource failures. Their original
failed reports and disks remain intact.
Both captured workloads now pass in the bounded x86 guest: the frontend IR
lowers identically to the unmodified host reference, and the 28 actual guest
QBE objects link into a byte-identical executable which runs successfully.
Evidence and independent local comparison are under
`build/mini-native-memory/result.json`. Full frontend/QBE source acceptance now
passes, with six frontend objects and all 28 QBE objects verified against the
original stopped disks and reused. The frontend finished under Ubuntu/KVM;
its identical-input Mini duplicate was stopped after acceptance.

`test_seeds_guest.py --reuse-record RECORD` explicitly resumes verified
completed objects after these output-preserving lowerer/linker fixes. The
record must identify the previous input key and original guest disk, every
installed input hash and every recovered object hash. Independently verify
that record against the stopped disk before using it. Source, SDK, frontend,
QBE, assembler and driver inputs must still match; only the two named seed
tools may change. The resumed Makefile omits rebuild rules only for verified
objects, then checks their hashes again after extraction. Acceptance still
requires the final guest link, unchanged Make, independent same-object host
linking, semantic execution and a fresh boot. Reports identify incremental
reuse and new input keys; this does not relabel a failed run or establish a
stable compiler generation.

`--jobs 2` runs two independent private guests and starts the larger tool
groups first. `RESULTS/run-state.json` records completion/failure per tool;
the guest logs also show each compiler stage. An output-directory lock prevents
two launchers from overwriting the same active disks. Completed tools retain
their extracted objects, executables and input/output hashes. Successful guest
root images and firmware-variable copies are deleted by default; failed disks
remain for diagnosis. Use `--keep-disks` to retain successful disks explicitly.
Private guest roots now have at least 128 MiB of physical space, allowing for
larger compiler seeds and live IR/assembly alongside accumulated object files.
This changes test storage only; process memory limits and executable ABI stay
the same.
Source-build outputs are extracted directly from their original paths after
shutdown, without duplicating executables/objects inside the guest filesystem.
This avoids exhausting a small test disk only while collecting a successful
build. Extraction rejects unclean filesystems, nonregular/non-octet files and
invalid paths; its real-filesystem regressions run with
`python3 rcc-portable/native-sdk/test_extract_guest.py`.
`--qbe PATH` selects a separately built QBE seed for both source rebuilding
and final execution checks. Its contents participate in checkpoint input hashes
and the accepted report records its path/hash; changing it requires new build
evidence rather than relabeling old checkpoints.

```sh
# Reuse the proven guest-built libc/startup with the prepared SDK headers.
python3 rcc-portable/native-sdk/test_make_guest.py \
  --sdk build/portable-native-sdk-make-check \
  --sdk-objects build/portable-native-sdk/guest \
  --source build/portable-native-tool-source-full \
  --results build/portable-native-tool-stage1
# Once stage 1 passes, rebuild using those four tools and compare outputs.
python3 rcc-portable/native-sdk/test_make_guest.py \
  --sdk build/portable-native-sdk-make-check \
  --sdk-objects build/portable-native-sdk/guest \
  --source build/portable-native-tool-source-full \
  --seed-tools build/portable-native-tool-stage1 \
  --compare-tools build/portable-native-tool-stage1 \
  --results build/portable-native-tool-stage2
```

Prepare that source directory first with `source_tools.py --sdk SDK --output
SOURCE`, selecting the same SDK. The test uses an existing private x64 base
image under `--output` (default `build/portable-native-x64`). `--sdk-objects`
changes only the linked libc/startup; it does not refresh the header/source
snapshot or assert that its objects reflect later shared-libc changes. Preserve
the accepted SDK snapshot/provenance. The frontend, QBE and assembler remain
bootstrap seeds even when these four tools match across generations. The
checkpoint regressions run with `python3 rcc-portable/native-sdk/test_checkpoints.py`.

The shared libc `qsort` uses `size_t` indices and constant auxiliary storage;
its regression covers bank-crossing arrays, more than 32,767 elements, records
over 128 cells and recursive comparator use. This replaces the old C16-overflowing
implementation for every source-library consumer, rather than only this SDK.

The assembler supports QBE's scalar/SSE2 AT&T forms, REX/SIB/RIP-relative
addressing, symbols, ELF64 sections and relocations. PC-relative addends account
for trailing immediates. Unsupported forms, TLS/GOT, section attributes and
narrow-value overflow produce errors. Branches currently use rel32 without
relaxation. Input/output and ELF quantities count octets despite C16 chars.
The new tools preserve existing outputs when generation fails.
The expression parser accepts unary sign chains in QBE displacements such as
`4+-15312(%rsp)`. Encoding dispatch is divided into instruction families; on
the measured portable assembler source this lowered host QBE's allocation
peak from 33,250,660 to 9,022,310 bytes. This measures host layout, not C16 heap
use or a guest execution-time improvement.

QBE's adapter widens compiler indices and integer constants, adapts varargs,
keeps target byte operations eight-bit and supplies a 32-bit-comparator heapsort.
Named regular-file input and named output now start with 65,536 scratch
instructions (524,288 C16 cells, about 1 MiB physical). If parsing or a lowering
pass exhausts scratch, a clean child process retries at twice the capacity,
up to QBE's original 1,048,576-instruction maximum. Each attempt keeps a stable
scratch address because optimization passes retain interior pointers; incomplete
assembly never replaces the output. Streams and debug output retain the original
maximum allocation because they cannot safely be replayed. The lexer also uses
the compiler-local buffered input path while preserving SDK refill/pushback.
Small upstream probes now request about 542,000..623,000 live C16 cells in total.
Larger x64 heaps are still desirable for full source builds. Metrics
exclude SDK allocations, metadata, globals, code and stack. Decimal float
parsing retains the existing SDK rounding limitation; generated floating IL
uses exact IEEE integer bits. Numeric parsing conformance remains a later gate.
Function-lifetime objects use zeroed slabs with stable addresses, released at
the original `freeall` boundaries. Large allocations have separate slabs.
The resolver/regexp probes reduce allocation requests from 130,238/92,609 to
2,260/2,211. Matched static builds show little wall-time improvement; these
allocation counts do not establish a substantial compilation-speed gain.

The spill pass reuses two separate function-lifetime bitsets for `limit2` and
`dopm`, which calls `limit2` while retaining its own set. Previously these
allocated one full temporary bitset per instruction and retained all copies
until `freeall`, exhausting the bounded guest heap on the native lowerer and
linker sources. Each workspace is overwritten before use and reinitialized for
every function; both are released by `freeall`. Auxiliary scratch storage now
scales with the temporary count rather than instructions times temporaries.
The focused call-chain regression first failed its 12 MiB host allocation
allowance; the extended test now peaks at 4,326,356 host bytes and produces identical
upstream assembly for integer/FP calls and changing function sizes. Host layout
differs from C16; the captured x86 source-build inputs are checked separately.
The exact previously failing `native/analyze.c` and `x64-link/archive.c` inputs
now compile and assemble in the same bounded x86 guest, with tracked allocation
peaks of 5,542,371 and 5,110,745 C16 cells respectively. Independent host
assembly of their actual guest output produces byte-identical ELF objects.
Evidence: `build/mini-native-full/diagnosis/guest-fixed/result.json`.

The first x64 run exposed a 121 KiB bootstrap-generated frame in QBE's spill
pass, exceeding the guarded 64 KiB kernel stack. The adapter separates block
and instruction processing into smaller activations without changing the
algorithm or kernel stack. The shared IR reader also hashes sparse SSA IDs
instead of allocating an array up to the largest ID, benefiting Ripple too.

The portable-built selector also exceeded that stack (138,024 bytes), as did
slot coalescing (68,424 bytes). The adapter now splits selection into opcode
handlers and coalescing, SysV calls and register allocation into their existing
phases. The largest selector-unit frame is now 14,888 bytes. SysV-only guest
QBE omits the unreachable Windows ABI; the unmodified host reference retains
its original targets. All 59 upstream fixtures still produce byte-identical
assembly in the independent host-width adapted build. The C16 guest retains
its documented equal-cost allocation differences.

QBE's fixed 64-entry parallel-move list also failed on the compiler sources.
Its function-arena allocation now scales with the temporary count and retains
the same move ordering. `qbe/test_parallel_moves.py` reproduces the original
failure, executes large/small/large phi joins, and checks unchanged small-case
assembly. `qbe/build.py --host --wide-moves` builds a separate reference with
only this capacity correction; the default host oracle remains unmodified.

Five bootstrap defects have correct-behavior RED rct tests: nonvoid exit
terminators, nested struct-tag completion, function-parameter declarator decay,
static compound-literal pointer initializers and nested array designated
initializers. Adaptations reference those tests; the compiler bugs remain to
be fixed rather than treated as accepted language restrictions.
The module reference also exposed the bootstrap lexer's rejection of string
escapes above 255 despite C16's 16-bit char. Its expected-correct RED test is
`test_c16_full_width_string_escape`; the portable frontend handles that literal.
The host rct reference uses equivalent numeric cells with a documented local
macro, while the actual separately compiled fixture keeps the wide string.

Additional verification:

```sh
python3 rcc-portable/qbe/test_adapt.py
python3 rcc-portable/qbe/test_arena.py
python3 rcc-portable/qbe/test_scratch.py
python3 rcc-portable/qbe/test_spill_memory.py
python3 rcc-portable/qbe/test_parallel_moves.py
python3 rcc-portable/x64/test.py
python3 rcc-portable/x64-link/test.py
python3 rcc-portable/native-driver/test.py
python3 rcc-portable/native/test.py
python3 rcc-portable/native/test_modules.py
python3 rcc-portable/native/test_archives.py
python3 rcc-portable/native/test_variadic.py
python3 rcc-portable/native-sdk/test_memory.py
python3 rcc-portable/native-sdk/test_reuse_objects.py
# Compile/link/rebuild/reload the separate modules inside a private x86 guest.
python3 rcc-portable/native/test_modules.py --guest --skip-build
python3 rcc-portable/native/test_archives.py --guest
# Supplementary C16-hosted reference comparison, in a disposable guest.
python3 rcc-portable/qbe/build.py --metrics
python3 rcc-portable/qbe/test_guest.py --all
python3 rcc-portable/test_ripple.py --guests none
```

Assembler checks cover 181 LLVM encoding comparisons, ELF relocations, 33
rejection/atomicity cases and all 58 non-TLS QBE fixtures. C16 QBE compiles 59
upstream fixtures; 58 match the unmodified host output exactly. `spill1` uses a
different equal-cost allocation ordering with heapsort and is checked by actual
x64 execution. The upstream FP-conversion driver supplies a further independent
execution oracle. Separate continuation tests force suspension rather than
relying solely on timer timing. QEMU reports are saved under
`build/portable-native-x64/compiler-test-result.json`.

Linker tests independently decode RXE1, inspect PC-relative addends and helper
trampolines, check BSS/page alignment, native-pointer fixups, absolute-width
boundaries and reordered section tables, and reject malformed input while
preserving outputs. A deterministic mutation smoke test complements these
specific cases. QEMU additionally executes a multi-object native-pointer/BSS/
page-alignment probe twice, and links the upstream spill/FP-conversion oracles
from separate objects inside the guest. Host/guest linkers produce identical
executables from the same objects; equal-cost QBE allocations can differ
between host libc sorting and guest heapsort. The driver compiles both raw C
probes and a macro/header program returning 37.

`native/test_modules.py` checks three-file recursion/callbacks and floating
calls, duplicate private names, initialized public/private globals, incomplete
external arrays, cyclic/null pointers, wide strings, template reset and
bank-crossing two-cell initializers. It relinks reordered objects, mixes host
and guest objects, replaces only one object, and checks exact host/guest
objects and executables. Malformed module records and deterministic metadata
mutations supplement link-time undefined/duplicate/ABI rejection tests.
Its report is `build/portable-native-modules/result.json`.

`native/test_archives.py` covers guest archive creation, recursive extraction,
unused conflicting/unresolved members, initializer-only imports, BSD/GNU long
names, mixed numeric/pointer runs, wide cells and zero tails. It compares guest
archives/executables with host outputs and reexecutes after a fresh boot.
Startup checks exercise argc, quoted argv, argv termination and envp. Host
checks include startup/reserved-name rejection and atomicity cases plus 96 deterministic archive
mutations, also run under UBSan. The completed extended guest report is
`build/portable-native-archive-final/result.json`; the default command writes
`build/portable-native-archives/result.json`.

Next native acceptance gates, in order:

1. Finish broader libc/runtime coverage: math/decimal formatting, nonlocal jumps
   and general aggregate SSA. Keep process, signal and
   blocking-I/O checks alongside language tests.
2. Extend generation comparison to the frontend, QBE and assembler together.
   Their individual source-build and execution gates pass. The native lowerer,
   linker, driver and archiver now pass two complete current 25-unit source
   generations with byte-identical executables, independent same-object host
   linking and combined execution after a fresh boot. Normal installation is
   separate from these private acceptance images.
3. Replace the host kernel backend/linker in the guest-assisted OS workflow.
   All 71 C16 kernel/embedded-program units compile in Riplix, and their IR boots
   through the host backend with recorded native platform objects. Per-unit
   guest QBE lowering also matches the host, but kernel global layout, entry,
   runtime bindings and final ELF construction still need portable support.
4. Improve scalability: function/section reachability, streaming ELF parsing and relocation records,
   removing duplicated initializer payloads and preserving richer source types
   for stronger aggregate declaration checks.

Performance is still a separate gate. All guest calls are resumable in this
prototype, and portable interprocedural optimization/logical-alloca promotion
are absent. QBE cannot remove arbitrary checked memory-helper calls by itself.
The host backend already has RCC IR optimization and direct leaf calls that
this path still needs. Functional acceptance is not speed parity with Cranelift.

A small instruction-selection comparison on the same probe-body RCC IR gave
these `.text` sizes in octets:

| Probe | QBE prototype | Existing Cranelift path |
| --- | ---: | ---: |
| Scalar/recursive calls | 3,986 | 4,000 |
| Floating arithmetic | 2,363 | 3,720 |
| Phi/call continuations | 1,846 | 1,032 |

QBE includes its additional kernel-exit startup; the Cranelift comparison uses
an arity-one entry adapter. Reachability/inlining and instruction selection also
differ. This is a limited code-size check, not an execution-speed result. It
supports continuing the route without claiming universal code-quality gains.
The input/report files are under `build/guest-native-assessment/`.

## Guest-assisted x64 OS build

The Ubuntu/KVM milestone now passes: a guest-built frontend compiles all 71
kernel/embedded-program C16 source units, with byte-identical host-reference IR.
The resulting kernel, emitted by the existing host backend and linked with
recorded native platform objects, passes two boot sessions including processes,
pipes, scripts, timers and filesystem persistence. This is a guest frontend OS
build; native code generation, platform objects, independent root applications
and EFI/image construction remain explicit host inputs.

See [the bootstrap boundary, commands and remaining gates](native-sdk/OS_BOOTSTRAP.md).
`source_os.py` freezes the source/flag inventory and records private strict-C
source adapters; `test_os_guest.py` verifies guest output and boots the hybrid
image. `test_os_checkpoints.py` checks cache/input integrity. Existing shared
kernel/hardware sources and normal user images are not modified by this workflow.

The remaining frontend source acceptance also passes on Ubuntu, using the same
fingerprinted inputs and six recovered objects as the stopped Mini run. QBE's
28-unit source build and the assembler's six-unit source build pass separately.
The current 25-unit lowerer/linker/driver/archiver snapshot now passes two complete
source generations, same-object host linking, byte-identical executable comparison
and combined fresh-boot execution with guest-built frontend/QBE/assembler inputs.
This evidence is separate from the earlier 23-unit snapshot.
