# QuickJS C16 porting experiment

Status: **the experimental `js` terminal host, full contexts and focused
standard-library tests run on both guest targets; a supported embedding SDK
is still being ported.** The adapted utility code,
binary64 number conversion, Unicode/regexp libraries, inline value representation
and embedding/platform hooks run as real C16 applications in both
RVM/Riplix and native Fuse-X. Neither `qjs` nor a replacement NetSurf JavaScript
adapter is installed in the user's images.
Extracted core string, serialization and bytecode-optimizer routines also pass
guest tests; these use test allocation/context plumbing, not a complete runtime.
The complete adapted engine now links and creates/destroys real guest runtimes
on both targets. The experimental integration gate also checks contexts,
objects, atoms, C callbacks and cycle collection. A separate evaluation gate
executes real JavaScript with basic intrinsics. The full-context gate additionally
initializes every standard intrinsic and exercises shared host language vectors,
logical-octet typed arrays and Promise jobs. The separate terminal host adds script execution, a line REPL, module loading and an asynchronous system host.
[System-host API, architecture and utility](system-host.md) describe filesystem,
environment/process APIs, timers and the `select()` loop.
Broader conformance, reusable embedding/RFL wrappers and browser bindings remain
future gates.

The C16 call-frame adapter reuses one released small frame per size class in
each runtime, with at most 992 allocation units of cached block capacity plus
allocator overhead. Active frames still count against the stack budget;
lowered memory limits, exception cleanup, generator ownership and runtime
teardown remain checked. Larger frames use the regular allocator. The
host reference/failure gate is `python3 ports/quickjs/tests/test_core_frames.py`.
The [YouTube extraction regressions](../ytmusic/extraction/README.md) exercise
the complete guest engine with realistic large scripts and retained timings.

The [jpipe tools](../jpipe/README.md) reuse this engine for streaming JSON Lines,
text adapters, map/filter/reduce, stable sorting and fused single-process pipelines.
They are a separate frontend; piped input to `js` continues to mean source code.

## Reproduce

The pristine MIT-licensed upstream release is pinned by archive SHA-256 in
`UPSTREAM.json` and by individual file hashes in `upstream-manifest.json`.
`vendor.py` verifies sources and refuses to overwrite a modified upstream tree.
All adaptations are generated under the build directory.

```sh
python3 ports/quickjs/vendor.py
python3 ports/quickjs/explore.py --target host
python3 ports/quickjs/explore.py --target both
```

The last command requires the existing Riplix and Fuse-X kernels, shells, RVM,
assembler/linker, and `target/release/rcc`. It builds SDKs and test applications,
then boots disposable disk images. It never modifies `fuzix.img` or
`ROOTFS.IMG`. On macOS, set `SDKROOT` if the default SDK cannot link host C code.
The experiment snapshots rcc, records its checksum, and writes build logs,
guest transcripts and `report.json` under `build/quickjs-explore`.
During support-library development, `--only support --target both` rebuilds
only the Unicode/regexp fixtures and writes `support-report.json`, preserving
the earlier full-run report.
`--only platform --target both` similarly runs the C16 embedding/platform
fixture and writes `platform-report.json`. Source adaptation has separate
host tests: `python3 ports/quickjs/tests/test_adapt.py`.
`--only storage --target both` tests the extracted core storage/operand routines
against pristine host output and writes `storage-report.json`.
`--only heap --target both` exercises the production pool allocator and writes
`heap-report.json`. `--only runtime --target both` links the complete engine
and tests runtime/context integration, writing `runtime-report.json`; this heavier gate
is explicit rather than part of the default utility matrix. RVM code generation
can take several minutes. Identical IR/compiler combinations reuse generated
assembly, with both checksums recorded in the cache key.
`--only eval --target both` runs the same engine with JavaScript evaluation
enabled and writes `eval-report.json`. It requires a complete evaluation marker,
correct results and successful cleanup. Its negative control deliberately
expects the wrong JavaScript result, and must fail the process. This gate uses
`JS_NewContextRaw` plus `JS_AddIntrinsicEval`, not a claimed complete context.
`--only full --target both` uses `JS_NewContext`, runs the shared language and
typed-array scripts, drains Promise jobs and writes `full-report.json`. The
whole-engine fixtures use fresh 65,536-block roots (128 MiB physical) to fit
the larger native executable and its one-cell-per-file-octet storage.
Add `--trace` when invoking the fixture directly to print individual JS checks.
`python3 ports/quickjs/tests/test_core_storage.py` separately checks the portable
string/operand/typed-array changes in complete host engines, including compiled-bytecode
save/reload. It does not run a guest engine.

### Build and run the terminal host

```sh
python3 ports/quickjs/build.py --target both
python3 ports/quickjs/tests/test_cli.py --target native --binary build/quickjs-cli/native/js.fxe
python3 ports/quickjs/tests/test_cli.py --target ripple --binary build/quickjs-cli/ripple/js.rxe
```

`build.py` produces `js.fxe` and `js.rxe`, snapshots the compiler and writes
`cli-build.json` with executable checksums. `--compiler` selects an explicit rcc;
`--build` selects an output directory (one subdirectory per target for `both`).
It builds artifacts only and does not install them into existing disk images.
The C16 executables remain large (roughly 23 MiB RXE / 36 MiB FXE in the current
fixture builds); RVM loader/startup cost and generated C frame sizes remain
performance targets. These are executable sizes, not runtime heap requirements.
The CLI tests install the binary as `/bin/js` in fresh disposable images.

Inside a guest with that binary installed:

```sh
js -p '70000 + 42'
js -e 'console.log("Hello, Ж 😀")'
js hello.js Ripple
js demo.mjs
js
```

[examples/hello.js](examples/hello.js) demonstrates arguments, Unicode, Date and
Promise output. [examples/demo.mjs](examples/demo.mjs) imports its sibling
[numbers.mjs](examples/numbers.mjs), uses DataView/BigInt and awaits a Promise.
Keep those two module files together.

`js` supports `-e` execution, `-p` result printing, script files, `-`/piped stdin,
`-m` module mode, `.mjs`/source module autodetection and `-i` for the line REPL.
On a terminal, no arguments also starts the REPL; `.exit` or EOF exits. Each
line is evaluated in the same context. The initial REPL accepts one expression
or statement per line, up to 4,095 input cells, without history or multiline
editing. An earlier error remains reflected in its exit status after recovery.
Options precede the file or `-e`/`-p` source. Remaining arguments become
`scriptArgs[1..]`; `scriptArgs[0]` is the filename or `<eval>`/`<stdin>`.
`print`, `console.log` and `console.error` stringify arguments and preserve
embedded NULs using length-aware UTF-8 output.

The file loader resolves ordinary relative ES module imports through QuickJS's
normalizer. Dynamic import and settled top-level await use the same bounded
pending-job loop. Missing modules, rejected/unsettled top-level await and
unhandled Promise rejections fail the command. Rejections handled by later jobs
in that evaluation do not fail it. This loader reads source files; it does not
supply package resolution, arbitrary native modules, `import.meta.url` or Node
built-ins. The built-in `fuse:system` module supplies the optional OS capabilities
and the CLI services timers, files and child pipes through a bounded event loop.

Exit statuses are 0 for success, 1 for JS/IO/resource failures, and 2 for invalid
command options. Defaults are 1,048,576 engine allocation cells (2 MiB physical
on C16), 10,000 interrupt polls per evaluation and 1,024 pending jobs. Adjust with
`--memory`, `--max-polls` and `--max-jobs`. `--max-time-ms` additionally bounds
each evaluation/event loop to 60,000 ms by default. Poll counts are engine callbacks,
not milliseconds or a precise instruction count. The terminal profile explicitly budgets 48,000 stack cells on C16, leaving
16,000 cells of RVM/Riplix's 64,000-cell stack bank for unchecked C callees,
exception reporting and signals. The general embedding header retains its
conservative 16,384-cell default. RVM's larger C spill frames made that default
reject ordinary dynamic Function compilation; the terminal policy keeps checks
enabled and is tested for actual recursive exhaustion. Source files are separately
bounded to 1,048,576 logical octets; source buffers and OS allocator bookkeeping
are additional to the engine limit. Stack checking remains enabled. The host
reference's allocation unit is an ordinary eight-bit C byte.

The CLI suite exercises scripts/arguments/stdin, banked
source, Unicode/NUL output, Date/Math/diagnostics, static/dynamic imports, await,
Promise failures/late handling, actual error line numbers, source errors, memory
limits during setup and execution, recursion/interrupt/job limits, invalid
options, console conversion failures and REPL state/error recovery. Reports and
guest transcripts are under `build/quickjs-cli-tests`; disposable roots must
pass shutdown filesystem checks. Repeat `--case "exact case name"` to rerun focused regressions; those use
separate selected-case reports and logs. The same CLI is also tested with the pristine
host engine, which adds a case forcing a buffered stdout flush failure
without filling a disk. This is a first terminal platform, not Node compatibility.

The next core compilation gate can be reproduced independently:

```sh
python3 ports/quickjs/core_probe.py --target both
```

This writes `core-report.json` and per-target diagnostics. It returns a failing
status for a blocked or timed-out compilation and removes incomplete IR.
This command only emits IR for the experimental adapted source; it never links
an application. The separate runtime fixture links that source explicitly.
Reaching IR does not establish correct JavaScript execution.

To reimport the same release after obtaining its official archive:

```sh
python3 ports/quickjs/vendor.py /path/to/quickjs-2026-06-04.tar.xz
```

## What was established

The host harness embeds the five engine source files directly, without
`quickjs-libc.c` or the upstream command-line application. Its 29 checks exercise
32-bit and binary64 arithmetic, negative zero, NaN, closures, classes, optional
chaining, BigInt, JSON, Map, Unicode regular expressions and normalization,
supplementary characters, embedded NUL, byte-oriented typed arrays, C callbacks,
cycles/GC, Promise jobs, a self-contained ES module, allocation failure and
interruption/recovery. It does not establish filesystem module loading, OS
bindings, browser compatibility or full ECMAScript conformance.
Three negative controls verify that assertion failures, rejected Promises and
invalid native callback arguments produce a failing process status.

The separate guest fixture runs 223 checks against the adapted upstream
`cutils.c`, `cutils.h` and the value-related parts of `quickjs.h`:

- 32-bit signed values and reference counts, double/NaN boxing, special tags,
  and banked pointer round trips.
- Floating-point tags whose low 16 bits alias reference tags. A negative-control
  run reproduced this misclassification before widening the tag operations.
- Explicit octet truncation/sign extension and unaligned 16/32/64-bit operands,
  including an operand crossing a real 64,000-cell bank boundary.
- All leading/trailing bit-scan positions, mixed lower bits and terminating
  zero scans. The portable helpers use bounded binary decisions rather than
  bit-at-a-time loops, keeping native direct-call eligibility.
- Supplementary Unicode conversion, surrogate pairs, malformed UTF-8 rejection,
  bit scans, dynamic buffer growth beyond one bank, allocation failure and sort.

The generated header is an internal experiment, **not a supported embedding
SDK**. Counts, flags, callback results and callback output pointers now use
32-bit types consistently. The core still needs to implement those declarations
with audited representations. This foundation fixture does not parse, compile
or execute JavaScript; the separate evaluation gate does.

The numeric fixture runs another **379 checks on each guest target** against
the adapted upstream `dtoa.c`. Its 134 output records match the unmodified host
implementation exactly. Coverage includes shortest decimal round trips,
random finite bit patterns, bases 2–36, subnormals, negative zero, maximum finite
values, Infinity/NaN, fixed/precision formatting through 101 digits, numeric
separators, binary/hex prefixes, parse endpoints and signed/unsigned 64-bit
integer conversion. Canaries check the conversion workspaces and output buffer.

The numeric adapter fixes a hard-coded eight-byte workspace stride: C16
allocation sizes must be divided by `sizeof(uint64_t)`, not eight. Invalid
number input uses explicit IEEE quiet-NaN bits for stable reference vectors.
The shared SDK's historical finite `NAN` macro has also been repaired.

The compiler now supports C99 hexadecimal floating literals directly, including
suffixes, subnormals, ties-to-even rounding and long significands. QuickJS's
`0x1p1008` constant is retained without a decimal rewrite. Runtime regressions
`test_hex_float_exponent` and `test_hex_float_limits` pass on RVM and native;
lexer tests cover malformed input, precision boundaries and direct f32 rounding
without an intermediate f64 rounding.

### Unicode and regexp milestone

The adapted upstream `libunicode.c` and `libregexp.c` now run on both kernels.
The support fixture passes **179 checks per guest**, and its five semantic
fingerprint records match the pristine host implementation. The host passes
157 checks; 22 extra guest checks inject allocation failures into locally
repaired upstream cleanup paths. A negative control on the host and both
guests verifies that a failed assertion produces a failing process status.
All disposable roots pass their post-shutdown filesystem checks.

Coverage includes case conversion and identifier classification sampled across
every Unicode plane; 56 normalization vectors in NFC/NFD/NFKC/NFKD; a
36,000-codepoint normalization result crossing a memory bank; Unicode category,
script-extension and property sets; and all 3,958 RGI emoji sequences emitted
by this pinned release. These use QuickJS's own Unicode 17 tables, independently
of the operating system's terminal Unicode tables.

Regexp checks cover ordinary/named captures and backreferences, lookahead and
lookbehind, scoped modifiers, multiline/dotall modes, Latin1 and UTF-16 input,
supplementary characters, Unicode property escapes, Unicode-set intersections
and string disjunctions. A long pattern produces bytecode spanning banks;
another executes a 35,000-character repetition. Execution allocation failure,
backtracking interruption and recovery, invalid-pattern diagnostics, recursive
stack exhaustion and repeated creation/destruction are checked explicitly.

The port distinguishes serialized four-octet bytecode operands from native
`uint32_t` normalization/string-set workspaces. UTF-16 subject offsets use
`sizeof(uint16_t)` rather than a hard-coded two-cell stride. Backtracking frames
use explicit fields instead of packing an index/type into pointer-sized
bitfields. Octet promotions, mask shifts, property-program varargs and signed
indexes use the appropriate 32-bit types without changing SDK declarations.

Two local upstream cleanup repairs are included: normalization frees its
buffer on allocation failure, and script construction returns an error instead
of jumping back onto its own failure label. The pristine host reference skips
failure injection for those two defective paths; guest-only checks verify the
repairs with guarded allocations and live-block accounting.

The fixture supplies a real stack-distance guard, linearizing banked addresses
on C16. Deeply nested non-capturing groups must trigger it; the separate
forced-rejection test also checks the callback contract. This is the support
library's embedding callback, not a completed QuickJS core platform layer.
These results do not establish full Unicode/regexp conformance or execute JS.

### Embedding and platform milestone

The separate platform fixture passes **68 checks per guest**. It verifies
32-bit callback argument counts, iterator output pointers and magic arguments,
interrupt results, property flags through bit 16 and reference headers. The
header's default stack budget is now 16,384 cells rather than an overflowing
16-bit constant expression or a budget exceeding the guest stack region.

`config/qjs-platform.[ch]` supplies real `JSMallocFunctions` callbacks. Limits
and accounting include requested cells, the bridge's allocation header and
alignment slack;
usable-size reporting excludes that header. System allocator bookkeeping is
additional. Physical-size reporting uses 64-bit arithmetic at two octets per
cell. Exact-limit and cumulative-limit checks, header-addition overflow, forced
allocation/reallocation failure, preservation of the old block after failure,
growth across a bank, shrinking after lowering a limit, moved allocator state
and repeated destruction are tested. Instrumentation is optional.
Payloads retain four-cell alignment across initial allocation, growth and
shrinkage. This preserves the low pointer bits used by atom free entries and
lazy-property realm tags. Reallocation moves the payload before writing a
relocated header, preserving data even when the raw allocation's alignment
changes.

Both guests grow their stacks upward. The guard linearizes `(bank, offset)`
addresses using 64,000 cells per bank, measures distance from the embedding
frame, and checks remaining capacity before adding a requested reservation.
Tests cover bank boundaries, both growth directions, overflow, actual recursive
guest frames, unwinding and recovery. A zero limit explicitly disables checking,
as in QuickJS. Embedders must budget headroom for native frames and exception
handling; this helper does not enlarge the operating system's stack region.

These hooks are connected to the core compilation draft, with switch dispatch,
Atomics disabled and stack checking retained. The platform fixture itself does
not instantiate a `JSRuntime`. Negative controls fail on both targets, and
disposable roots pass post-shutdown filesystem checks.

Source type rewriting now operates on C tokens, preserving comments, string
and character literals, escaped quotes and include paths. It also recognizes
compound types separated by comments. It never redefines types across SDK
headers or alters embedded JavaScript text as a side effect of widening C types.

The core probe exposed a C99 parser defect in Date's `double fields[static 9]`
parameter. `test_static_array_parameter` was registered and confirmed failing
before the compiler fix. Constant minimum bounds and bracket qualifiers now
adjust to pointer parameters; bracket `volatile` qualifies the pointer rather
than its elements. Invalid placement and syntax are rejected. Both this test
and the related `test_restrict_array_parameter` pass on RVM and native, alongside
four focused frontend tests and 28 existing parser tests. Dynamic parameter
bounds remain explicitly unsupported; their entry-time side effects are not
silently discarded.

The next attempt hit a five-minute compilation timeout. Cast and `sizeof`
backtracking copied every remaining token; the parser now shares immutable
token storage with independent cursors. A focused cast/sizeof stress regression
was confirmed failing before the change and now passes on both backends. The
126 frontend unit tests and four array-parameter tests also pass. In the final
release-compiler probe, core parsing took 0.28/0.32 seconds and the entire
compile attempt took 0.93/1.00 seconds for native/RVM respectively. These are
individual compile-gate observations, not JavaScript execution benchmarks.

Both adapted core probes now compile the full engine translation unit to IR on
both targets. The core-probe command remains a compilation gate; the explicit
runtime fixture additionally verifies lifecycle, not a usable interpreter.
The shared math SDK now declares and implements the missing double
functions through host acceleration, including acosh/asinh/atanh, and its NAN
macro is a real quiet-NaN constant, including in static float/double initializers.
The historical finite constant was reproduced by a failing rct regression first.

`core_frames.py` replaces the four GNU alloca sites with checked heap frames.
They use the runtime allocator and count active frame cells against the stack
budget, together with native stack distance. Normal/exception returns free the
owned frame after closing local references. Suspended generator/async frames
remain owned by their existing state; resuming them does not allocate or free a
second local frame. Native callback padding allocates before installing its
stack-frame record. Allocation overflow and failure propagate JS exceptions.

`python3 ports/quickjs/tests/test_core_frames.py` tests that transformation on an
otherwise pristine full host engine before C16 core execution is available.
Both the reference and adapted engines pass 38 semantic checks, including bound
calls/constructors, callback padding, closure lifetime, suspended generators,
stack exhaustion and recovery, with the same final live allocation count. The
only retained-size difference is one size_t active-frame counter in JSRuntime.
A separate 16-check fixture disables arena reuse to force allocation failure on
bytecode, ordinary C,
C-data and bound call paths, checks that callbacks are not entered on failure,
and verifies recovery. Both engines also reject assertion and Promise negative
controls. These are host validation results, not guest JS execution claims.

The guest clock SDK now supplies gettimeofday using its existing realtime clock.
The initial engine profile explicitly uses UTC, matching guest localtime;
Date precision remains that of the guest calendar clock. Math acceleration and
its MMU-aware packet contract are documented in
[the architecture reference](../../docs/arch/mmio.md#floating-point-acceleration).

### Core storage and operand milestone

`core_storage.py` distinguishes native Latin1/UTF-16 string units from serialized
octets. Allocation, copying, widening, shrinking, allocator slack and memory
reporting now use native unit sizes. UTF-16 occupies one C16 cell per code unit;
serialized UTF-16 still occupies two logical octet cells. String comparisons
widen operands before subtraction so unsigned 16-bit promotions cannot reverse
their ordering. Regexp capture offsets divide by the native character size while
retaining the original Latin1/UTF-16 selector passed to the regexp engine.

The serialized reader reconstructs native UTF-16 units explicitly. Numeric
writers emit 2/4/8 logical octets rather than copying native C objects, and the
adapted bytecode reader/writer no longer applies native-endian swaps to the
already little-endian accessors. Oversized UTF-8 input is rejected before reading
it or narrowing its length. A locally reproduced upstream UTF-8-export failure
leaked its temporary string reference; the generated adaptation releases it on
the failure path. Pristine upstream files remain unchanged.

`core_bytecode.py` uses explicit eight-bit sign extension for short backward
branches and range checks when selecting compact immediates. Optimizer opcode
alternatives use 32-bit shifts and explicit octet masks. Its pattern matcher
receives a typed C99 compound-literal array instead of mixed-width C varargs:
otherwise enum/literal arguments occupy one C16 cell while the widened private
matcher retrieves two cells. This change does not alter the public embedding ABI.

The isolated storage fixture passes **93 checks on each guest**. Its 30 string,
serialized-data and compact-immediate records match pristine host routines
exactly. The host reference passes 91 checks; two additional guest checks cover
the new length guard and the repaired reference leak. Coverage includes UTF-8,
CESU-8, Latin1, BMP/supplementary code points, embedded NUL, unpaired surrogates,
malformed UTF-8, substring narrowing, concatenation, 70,000-unit strings spanning
banks, allocator slack, failed growth/widening/shrinking, and recovery. Serialized
string truncation is tested at every cut point. Immediate tests cover signed
32-bit boundaries and the one-, two-, three- and five-octet encodings; optimizer
tests exercise literal/enum arguments, wildcards, wide operands, line numbers,
packed opcode alternatives and truncated operands. Guarded allocations check
overwrites and cleanup. Negative controls fail and disposable filesystems pass
their post-shutdown checks.

`storage_fixture.py` extracts actual verified core definitions, string layouts,
opcode enums and tables. Its replacement context, allocator and exception
plumbing deliberately isolate representation issues; they do not establish
production allocator, atom table, object or GC correctness. The portable string
and operand adaptations originally passed **42 checks in each full host engine**, with
identical retained memory and allocation counts, including regex replacement,
capture indices, ropes, string/object serialization and execution of reloaded
compiled bytecode. Assertion and Promise negative controls fail on both engines.
The isolated storage fixture does not itself verify guest bytecode dispatch;
the separate evaluation gate now exercises it in the production engine.

### Production heap and runtime milestone

`core_heap.py` preserves the production arena allocator rather than replacing
it with test allocation plumbing. Its C16 block header is eight cells, with
padding before the 32-bit reference count so public value duplication still
addresses that count immediately before the payload. Provider alignment, arena
offsets and size-class strides preserve the engine's low pointer tag bits.
Allocation/header rounding rejects overflow before invoking the provider.
Atom flags and private shift constants use explicit 32-bit arithmetic; token
rewriting leaves JavaScript source strings and comments untouched.

The isolated production heap fixture passes **156 checks per guest**: all 31
size classes, reference/GC header identity, zero-size storage, pool/large-block
reallocation, cross-bank data, overflow/failure preservation, thousands of live
blocks, temporary allocation/free churn, pool reuse under provider failure, and
30-bit free-atom indexes. It verifies complete arena/provider cleanup. Negative
controls fail and disposable filesystems pass their shutdown checks.

The separate whole-engine fixture passes **36 checks per guest**. It creates a
real `JSRuntime` and `JS_NewContextRaw` with the production basic intrinsics,
checks initial job/opaque state, inserts and reads Unicode/numeric object
properties, retains 1,200 atoms through table/hash growth, reuses freed slots,
calls C functions with ordinary/padded arguments, and collects a self-referencing
object cycle. It injects failure at every runtime-startup provider allocation
and repeatedly destroys/recreates the runtime without leaked provider blocks.
Context-construction failure injection remains separate future work. Executable
sizes are recorded per target in each gate report; they include the experimental
fixture and are not a heap-size estimate.

Its `qjsruntime --eval` mode is now a separate passing evaluation gate, described
below. The compiler's separate `test_string_hex_octets` regression still fails:
C string hex escapes are reencoded as UTF-8. Evaluation vectors use JavaScript
escapes and literal UTF-8 source to test both paths without that C input
corruption. The raw-context fixtures do not establish full intrinsic initialization;
the separate full-context gate below does. None establishes conformance or is
installed as a supported interpreter.

Runtime integration exposed compiler defects, each registered and reproduced
with a failing C regression before repair:

- Constant union members containing multi-cell integers, pointers and native
  callbacks now lower to cell storage with correct relocation widths, including
  nested structs/unions and zero padding. RVM, native and Wasm global lowering
  consume the same representation.
- Negative enum constants retain their signed integer type. Previously `-1`
  could become an unsigned 64-bit literal and make a valid object tag compare
  unequal to `JS_TAG_OBJECT`. Tests also cover varargs and switch cases.
- Flexible array members occupy zero cells in aggregate IR layouts. Their
  former pointer representation made struct pointer strides disagree with
  `sizeof`, corrupting adjacent QuickJS allocations through shape-property
  addressing.

The port also gives the three-bit closure-kind field an explicit unsigned
32-bit storage unit. Upstream relies on GCC's unsigned representation of a
nonnegative enum; RCC's signed enum field otherwise reads kinds 4–7 as negative
values and aborts when creating global lexical closures.

The portable heap/constant adaptations also pass the full host engine's
42-check string/serialization harness with unchanged live-memory and allocation
counts. C16-specific layout/alignment checks run in real guest processes.

### Guest evaluation milestone

The evaluation fixture uses the production parser, optimizer, bytecode dispatch,
allocator, objects and GC. It passes **69 checks on each guest**, including the
36 runtime/context checks. Its expression vectors cover integer/binary64
arithmetic, global and local lexical loops, closures retaining numbers/strings,
object literals, concatenation, embedded NUL, literal UTF-8 and JavaScript
Unicode escapes, and JavaScript calls to ordinary/padded native callbacks.
Directive tests distinguish an actual `"use strict"` directive from an escaped
lookalike, a parenthesized string expression and an overlong directive candidate.
Malformed source produces error objects, a thrown numeric value is preserved,
an infinite loop calls the embedding interrupt handler, and subsequent
evaluation recovers. The entire context/runtime is then freed with zero live
provider allocations.
One directive candidate contains 70,000 characters, crossing a real memory
bank and exceeding the signed 16-bit precision range. Both guests evaluate
its length correctly. Negative controls reject a deliberately incorrect JS
result; all disposable roots pass their post-shutdown filesystem checks.

The string-literal crash was a port ABI error in directive detection. Private
integer widening changed the precision argument of `snprintf("%.*s", ...)` to
32 bits, while guest libc consumes a 16-bit `int`; it then read the following
string pointer from the wrong varargs position. `core_libc.py` keeps this slot
in the libc integer domain and bounds precision before narrowing. It also fixes
the corresponding JavaScript/JSON unexpected-token diagnostic slots. Ordinary
host default argument promotions still supply the host's correct int type.
The later literal-format adaptation below extends this repair across core
formatting call sites.

The portable parser-format adaptation also passes the complete host engine's
42-check string/serialization suite with identical memory/allocation counters.
That raw-context validation alone did not establish standard globals or built-ins.
The newer full-context and terminal-host gates below cover several of these
paths, including Date formatting, selected Math functions and module execution.
Broader conformance remains separate.

### Full contexts, typed arrays and Promise jobs

`explore.py --only full` passes **145 checks per guest**, including the earlier
69 evaluation/runtime checks. It creates a real `JS_NewContext` with all standard
intrinsics and evaluates the same `semantics.js` used by the host oracle, followed
by `typed-arrays.js` and `formats.js`. Standard-library coverage includes JSON, classes, prototype
lookup, BigInt arithmetic, Map, Unicode regular expressions and normalization.
Two pending Promise jobs run through `JS_ExecutePendingJob`; the fixture verifies
the final result and imposes a job-count budget. Runtime teardown leaves zero
live provider allocations. A deliberately wrong JavaScript result fails the
negative control, and both disposable roots pass their shutdown filesystem checks.

`core_typed_arrays.py` keeps one logical ArrayBuffer octet per addressable cell.
Indexed access and DataView operations explicitly mask/sign-extend octets and
load/store little-endian integers or IEEE floating-point bit patterns. They never
reinterpret ArrayBuffer cells as native scalar arrays. DataView defaults to big
endian and honors explicit little-endian requests, including unaligned accesses.
Native UTF-16 strings and sort-index workspaces keep their own representations.

The adaptation retains upstream conversion, bounds and detachment checks. Fill,
reverse and sorted copyback operate on logical elements; sorting comparators
load those elements explicitly. Search uses the existing strict-equality or
SameValueZero helpers, preserving NaN, signed-zero and BigInt distinctions. That
value-based search favors correctness; optimizing it is separate work.

The 39 additional typed-array checks cover integer views through 64 bits,
Float16/32/64, clamped rounding, cross-view bit identity, signed values, unaligned
DataView/endian operations, overlapping copyWithin, shared subarrays, independent
slices, fill/reverse, default and callback sorting, NaN/signed-zero searching,
resizable-buffer view tracking and transfer/detachment. A guest callback locates
an actual bank edge inside a 70,016-octet buffer so an eight-octet DataView access
straddles that edge; another typed element resides in a later bank.

The for-of vectors exposed a further porting promotion error. In `-3 - pc[0]`,
the C16 unsigned character carrier converted the negative literal to unsigned
16-bit before assignment to a widened variable. The iterator then read stack
slot 65,533 instead of a negative offset. `core_bytecode.py` now widens the
subtraction operands first. This was an adaptation issue, not a compiler defect.

The complete pristine and adapted host engines pass **92 checks each**, expanding
the earlier 42-check storage suite with 39 typed-array and 11 formatting/Math
vectors. Retained
memory, allocation and semantic counters match; assertion and Promise negative
controls still fail correctly. Upstream sources remain pristine. These focused
results do not establish full ECMAScript conformance, all auxiliary byte codecs,
all overlapping cross-type set cases or browser integration.

### Formatting ABI and terminal-host milestone

`core_libc.py` now tokenizes literal format-call contracts across the core.
Private integer output uses explicit `%ld`/`%lu`/`%lx` conversions and matching
casts, including enum/literal arguments that otherwise still occupy one C16
cell. Width/precision and character arguments retain libc's ordinary integer
ABI; pointer, string, binary64 and 64-bit integer arguments receive their exact
promoted types. Adjacent literals and PRI macros are checked together. Atom
error macros and nonliteral `va_list` forwarding retain their own contracts.
Unsupported formats and argument-count mismatches stop source generation.
SDK declarations and JavaScript source embedded in C strings are not widened.

The failing Date ISO regression was reproduced on Fuse-X before this repair.
The 11 new language checks cover positive/negative Date epochs and extended
years, dynamic-width UTC formatting, JSON/RegExp code-unit escapes, token/character
diagnostics, a stack-trace line above 40,000, and accelerated `acosh`/`sin` through
actual JavaScript Math functions. All 145 full-context checks pass on both guests,
including production-provider teardown without live allocations and the failing
negative control. The host's 92 checks retain matching memory/allocation counters.

`cli.c` is a separate portable command host. It initializes full contexts, owns
all values crossing the C boundary, checks helper creation failures, loads
length-bounded source, tracks pending rejection reasons with engine allocations,
reports exceptions and drains jobs under an explicit budget. It uses the C16
provider on guests and the pristine engine's provider in the host reference.
It does not depend on upstream `quickjs-libc.c` or its POSIX assumptions.

## Porting decisions

1. Keep one source engine for both targets. Native Fuse-X applications retain
   the C16 ABI; the ordinary host executable is only a reference oracle.
2. Keep the upstream 32-bit-pointer NaN-boxed value format initially. The guest
   checks demonstrate that 32-bit banked pointers and 64-bit values can round
   trip correctly. Widen value payload/tag operations and reference counts to
   explicit 32-bit types; a 16-bit C `int` is insufficient.
3. Store each logical bytecode octet in one addressable cell. `qjs_octet_t` is
   explicitly a 16-bit carrier. Mask stores and implement logical little-endian
   operand access explicitly. Do not pretend casts or packed C structs provide
   eight-bit storage. This doubles the physical size of octet buffers compared
   with an ordinary eight-bit-byte machine.
4. Preserve JavaScript's UTF-16 string semantics, eight-bit typed-array
   semantics, and binary64 Number semantics. Internal C string storage and
   logical ArrayBuffer octets are distinct representations; the core typed-array
   adapter now replaces their scalar pointer casts with explicit accessors. Avoid a silent JS language subset.
5. Keep the engine independent of POSIX, curses and NetSurf. Start with a
   single-threaded runtime and switch dispatch; defer shared-memory Atomics
   and workers. Preserve a real stack limit when replacing upstream stack
   detection; do not disable it to make the engine appear to work.
6. Pass memory limits through the allocator, with explicit units: C16 `size_t`
   measures cells, while physical storage uses two octets per cell. Browser
   heaps must fit alongside DOM/layout/images in the process data window.

The utility adapter widens its private arithmetic/index `int` declarations to
`long`, uses explicit octet operands in both buffer paths, and uses cell-based
sorting swaps. It does not redefine `int` across SDK headers or claim that
mapping `uint8_t` to a wider character type ports the engine.

## Remaining gates, in order

The reproducible compile matrix first tries all five original engine sources
with the SDK, then with a local integer-format header. Currently the first
unmodified gate is missing `inttypes.h`; the next gates are the upstream GNU
packed-struct syntax in the support libraries and `pthread.h` in the core.
These are first diagnostics, not a complete list of defects. In particular,
removing pthread dependencies alone would not make JavaScript execute.

1. Add an explicit guest platform configuration: no OS threads/Atomics,
   portable switch dispatch, bank-aware stack checks, allocator hooks, time,
   and a defined floating-point rounding policy. The SDK's `fenv.h` currently
   has no implementation. The stack and allocator hooks now pass isolated
   guest checks and the production runtime/context gate. The binary64 Math
   surface and checked call-frame adaptation now pass their
   isolated tests. Audit integer widths, shifts, bitfields, reference headers
   and remaining libc calls throughout the engine.
2. Unicode and regexp support now pass the focused guest reference comparison,
   alongside numeric conversion and utility/value checks. Expand upstream
   conformance and malformed-input coverage as the full engine is integrated.
   The core's own buffers, object layouts, bitfields and signed octet conversions
   still need auditing; passing support libraries does not port them implicitly.
3. Core string storage, serialization primitives and optimizer operand handling
   now pass isolated guest tests. Production allocator/atom/object integration
   additionally passes the runtime/context gate; expand those checks and audit
   pointer tagging, integer constants,
   native layouts and remaining libc/varargs boundaries. Focused bytecode dispatch,
   complete intrinsic initialization, shared host language vectors, typed-array
   accessors and Promise jobs now pass on both guests. Expand selected upstream
   and Test262 tests, especially auxiliary base64/hex codecs, overlapping
   cross-type typed-array set, debug/serialization paths, broader Date/Math behavior
   and module graph edge cases.
4. Verify GC cycles, cross-bank references, resource limits, interruptions,
   stack exhaustion, exception recovery and repeated engine creation/destruction.
   Measure startup, maximum/live heap and representative workloads separately
   on both targets. The small fixture timings are not JS engine benchmarks;
   the host harness's final live memory is not a guest size estimate.
5. Expose a C embedding library and RFL ownership wrapper with explicit value
   lifetimes, length-aware strings, native callbacks, protected evaluation,
   module loading, pending jobs, errors and memory/execution limits.

## Terminal runtime and browser consumers

The first `js` executable now provides a line REPL, script arguments, exit
status, console output and source-module loading. The optional
[system host](system-host.md) now adds filesystem/environment/process APIs,
timers and a readiness loop backed by `select()`, alternating bounded Promise
job batches with OS work. The `fuse:http` source module now adds bounded
GET/JSON/binary responses, redirects, cancellation and whole-request deadlines
through the existing guest `htget --response` TLS worker. The
[HTTPS JSON utility](examples/fetch-json.mjs) keeps progress timers responsive,
caches UTF-8 JSON and launches a viewer. Rift detects `.js`/`.mjs`/`.cjs`, applies
a dedicated JavaScript lexer and runs saved files through `js` from PATH.
Curses and image decoding should likewise reuse existing libraries/services
with coarse operations. The `fuse:net` module now supplies nonblocking IPv4 TCP handles, accept/read/write
Promises, cancellation and bounded writes that honor backpressure. A separate
`fuse:http-server` module adds a small GET/POST router, named parameters, async
middleware, JSON bodies/replies, connection bounds and graceful/forced shutdown.
Run [hello.server.mjs](examples/hello.server.mjs) with `js --server`, or F5 in
Rift (the `.server.mjs` suffix selects server mode). Each middleware stage yields
to a Promise job, preserving RVM's native stack guard. HTTP streaming,
keep-alive, inbound TLS and Node/Express package compatibility remain future work.

NetSurf currently disables Duktape in `ports/netsurf/config/sources.mk`. Its
existing DOM binding generator targets Duktape, so a QuickJS browser adapter
needs separate work for DOM identity, events, GC roots and page lifetimes.
The browser adapter must not inherit the terminal runtime's filesystem or
process modules implicitly.

Node-compatible built-ins, npm compatibility, native Node addons and TypeScript
execution are separate milestones. QuickJS supplies the JavaScript language
and embedding machinery, not those host APIs. No second engine or new language
implementation is proposed by this experiment.

Upstream references: [project](https://bellard.org/quickjs/),
[C API and internals](https://bellard.org/quickjs/quickjs.html).
