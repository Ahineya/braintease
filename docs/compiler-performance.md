# RCC performance investigation

Investigation date: 2026-10-17. The initial measurements and prototype are
retained below. The serial optimisations are now implemented; see
[Implementation follow-up](#implementation-follow-up) for new measurements,
validation and the repeatable benchmark runner.

The largest measured opportunity is avoiding unnecessary macro-expansion work.
Parallel compilation is also feasible, but removing this work should come first.
Neither approach requires changing the C ABI or adding language extensions.

## Where compilation time goes

Three Riplix translation units were compiled using a copy of the release RCC
binary and their Riplix include paths and defines. Driver progress messages were
timestamped from process launch:

| Source | Whole compilation | Time through preprocessing | Share |
| --- | ---: | ---: | ---: |
| `ports/fuzix/ripple/hello.c` | 0.4391 s | 0.4346 s | 99.0% |
| `ports/fuzix/ripple/gterm.c` | 7.8770 s | 4.7079 s | 59.8% |
| `ports/fuzix/upstream/Kernel/tty.c` | 15.2410 s | 12.4459 s | 81.7% |

These are approximate wall-clock boundaries, not a CPU profile. “Through
preprocessing” includes process startup and reading the source; this matters
especially for the tiny `hello.c` sample. The remaining time includes parsing,
semantic analysis, IR generation, optimisation, lowering and output. Each row
is one sample, and other builds were active on the host.

## The preprocessor hotspot and measured prototype

In [`expand_macros_impl`](../rcc-preprocessor/src/directives/mod.rs), each
expansion iteration visits every defined macro. Object-like macros construct a
new regular expression, prepare their replacement body, mask literals and scan
the current text. Function-like macros also construct a regular expression and
mask literals. This happens even when the macro name does not occur anywhere
in the text. Expansion operates on text chunks separated by directives and may
repeat several times as macros introduce further macros.

A temporary copy of the preprocessor added only this check at the start of the
existing macro loop:

```rust
if !result.contains(name.as_str()) {
    continue;
}
```

This is a necessary-condition filter. If the name occurs inside a larger
identifier or a quoted literal, processing still follows the original matching
and literal-handling code. It does not replace those checks or change expansion
order. It also checks the current `result`, so names introduced by earlier
expansions remain visible to subsequent checks.

A release harness ran the original and candidate preprocessors on the same
inputs, flags and include paths, then compared their complete output strings:

| Source | Original preprocessing | Candidate preprocessing | Ratio |
| --- | ---: | ---: | ---: |
| `ports/fuzix/ripple/hello.c` | 0.082436 s | 0.005688 s | 14.49× |
| `ports/fuzix/ripple/gterm.c` | 6.854171 s | 0.055598 s | 123.28× |
| `ports/fuzix/upstream/Kernel/tty.c` | 13.402621 s | 0.110319 s | 121.49× |

All three outputs were byte-for-byte identical. All **55 existing preprocessor
library tests passed** against the candidate.

The harness used workspace-locked dependencies, including `regex` 1.11.1, with
`opt-level=3`, thin LTO and one codegen unit. Both implementations ran in the
same executable with separate preprocessor instances. The original ran first,
so filesystem cache warming may favour the candidate. These are single samples
on a host reporting 10 logical CPUs; exact hardware and peak memory were not
recorded.

These timings are **preprocessing only**, from a separate experiment from the
whole-compiler table. They do not establish a 120× compiler or Riplix build
speedup. No complete compiler incorporating the candidate was benchmarked.

Further preprocessor opportunities identified in the original investigation
(see the follow-up for implemented changes):

- Reuse compiled macro-name patterns rather than rebuilding them for every
  chunk and expansion iteration. Preserve correct behaviour across macro
  redefinition and `#undef`, especially if caching replacement data as well.
- Compile fixed patterns for `__FILE__`, `__LINE__` and `defined` once.
- Avoid repeated literal masking and output allocation when there is no match.
- Consider token-based expansion later if profiling still justifies a larger
  change. The small filter is independently useful.

Header caching needs more care: expanded output depends on the active macro
environment, include order and predefined file/line values. Caching an expanded
header solely by filename would be incorrect.

## Other opportunities found in source review

These findings describe the pre-change compiler. They were candidates for
measurement, rather than demonstrated speedups; the follow-up records the
implemented subset and combined results:

| Location | Finding | Possible change |
| --- | --- | --- |
| [`finish_module`](../rcc-driver/src/main.rs) | Textual IR is generated even when no IR output option is requested. | Generate it only for emit, print or save requests. |
| [`lower_module_with_stack_mode`](../rcc-backend/src/lower/module.rs) | Internal-symbol renaming scans the internal-name list for each relevant assembly instruction and repeatedly formats names. | Precompute replacements and efficient lookups, preserving offset/bank expressions and block-label prefixes. |
| Driver and backend module handoff | Modules are cloned at multiple boundaries. | Profile allocation cost and investigate ownership changes while preserving isolation between transformations. |
| [`ports/fuzix/Makefile`](../ports/fuzix/Makefile) | Many rules depend on broad header lists, the compiler binary and the Makefile. | Track actual included headers and command/flag changes to reduce unnecessary incremental rebuilds. |

Do not simply remove build prerequisites: compiler, target, ABI, flags and header
changes must still invalidate affected outputs.

## Where Rayon fits

[`run-riplix.sh`](../scripts/run-riplix.sh) already uses `make -j4`: separate source
files compile concurrently in separate RCC processes. Rayon would add
parallelism **inside one compilation**.

The Ripple backend has useful potential boundaries:

1. In [`optimize_module`](../rcc-backend/src/optimize/mod.rs), optimise independent
   functions in parallel, complete module-wide inlining, then parallelise the
   independent per-function passes that follow it.
2. In module lowering, run pointer-bank preservation, loop rotation and phi
   destruction independently for each function, preserving their order within
   each function.
3. Allocate globals first, then lower functions using separate register and
   naming contexts with read-only global information. Collect results in source
   order and merge assembly and linker metadata serially.

These boundaries still need a Rust `Send`/`Sync` and shared-state audit. Preserve
symbol uniqueness, stable function ordering and predictable error reporting.
The existing naming machinery uses generated namespaces, so raw assembly byte
equality is not by itself a suitable correctness test across separate runs.

The frontend uses `Rc<RefCell<...>>` for shared semantic/type environments, so
adding `par_iter_mut` around frontend traversal is not sufficient. Independent
translation units can still use isolated frontend state in separate workers;
the current Make build already provides that isolation through processes.
Preprocessing within a translation unit has ordered, mutable macro state and
should remain sequential initially. Native code generation needs a separate
audit; the shared preprocessor improvement would benefit both targets.

Rayon supports [parallel mutable iteration](https://docs.rs/rayon/latest/rayon/iter/trait.IntoParallelRefMutIterator.html)
and [bounded thread pools](https://docs.rs/rayon/latest/rayon/struct.ThreadPoolBuilder.html#method.num_threads).
Use a configurable budget shared with build-level concurrency, with small
modules staying serial. Four compiler processes each using every CPU could
increase contention and memory use. No Rayon prototype was measured here.

## Recommended implementation order

1. Land the absent-name filter with focused regression tests, then measure
   pattern caching and fixed-pattern reuse.
2. Add opt-in stage timings and a repeatable benchmark corpus before making
   broader changes.
3. Re-profile and address remaining serial work: unnecessary IR emission,
   repeated symbol lookup/formatting and measured allocation costs.
4. Add bounded per-function backend parallelism around explicit module-wide
   barriers. Measure against the serial backend with the same build job budget.
5. Make build jobs configurable and improve dependency tracking for incremental
   builds, preserving all required invalidation.

## Implementation follow-up

Implemented on 2026-10-17:

- Skip a macro before pattern preparation or literal masking when its name is
  absent from the current expansion result. Expansion order is unchanged.
- Lazily compile each macro's invocation pattern once per definition. All
  definition paths reset the cache, including object/function redefinition;
  both undefine paths remove it. Replacement bodies remain live definition data.
- Reuse fixed builtin/`defined` patterns, skip absent builtins, and borrow text
  when literal masking or a replacement has nothing to change.
- Generate textual IR only for `--emit-ir`, `--print-ir` or `--save-ir`. Borrow
  the module when it already has the requested target instead of cloning it.
- Precompute internal-symbol replacements. Assembly references and linker
  metadata use hash lookups, and block labels check underscore boundaries.
  Ambiguous prefixes retain the original ordered first-match behaviour.
- Add `rcc compile --timings`: stage wall times go to stderr as
  `rcc-timing STAGE SECONDS`, including on early errors. Normal stdout and
  `--emit-ir` output retain their existing contracts. The total includes
  diagnostic/trace work; stage measurements are not a CPU profile.

Follow-up measurements used three measured runs after one warm-up, alternating
baseline/candidate order. These are separate samples from the investigation
above. All preprocessing outputs were compared byte-for-byte on every run.

| Source | Original preprocessing | Updated preprocessing | Speedup | Original whole compile | Updated whole compile | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| hello.c | 0.076444 s | 0.007404 s | 10.32× | 0.050048 s | 0.008038 s | 6.23× |
| gterm.c | 4.813452 s | 0.031438 s | 153.11× | 8.924725 s | 3.437640 s | 2.60× |
| tty.c | 6.811251 s | 0.018615 s | 365.91× | 10.568846 s | 2.547936 s | 4.15× |

Environment: macOS 27.0, ARM64, 10 reported logical CPUs, Rust 1.97.1;
workspace release settings are opt-level 3, thin LTO and one codegen unit.
The original `rcpp` was built from the pre-change sources with the current
locked dependencies. The whole-compiler baseline was the saved pre-change
release binary, rather than a fresh baseline rebuild. Exact CPU model/RAM
were unavailable to the measurement process. Other compilation/test jobs were
active on the shared host; the benchmark itself ran one child at a time.
Treat these ratios as illustrative, especially the tiny hello sample.
Preprocessing and whole compilation are independent measurements and must
not be subtracted from each other. There is no clean-build speedup claim.

The candidate stage timings now put almost all of gterm/tty compilation time
in Ripple lowering/emission, rather than preprocessing. Backend parallelism,
further ownership changes and actual-header dependency tracking remain future
measurement work; no Rayon threads or weakened build prerequisites were added.

The [benchmark runner](../scripts/benchmark-rcc.py) records individual
wall/CPU times and peak RSS, stage timings, medians, binary hashes, source
hashes, Cargo.lock hash, tool/platform information, revision and worktree status.
Its samples are warm-cache measurements; it does not evict filesystem caches.
`--mode preprocess` takes `rcpp` binaries and checks complete output equality;
`--mode ir` takes `rcc` binaries and also checks output equality. Compile mode
does not compare raw assembly because generated symbol namespaces can differ.

Save baseline binaries before rebuilding, then run:

```sh
cargo build --release --locked -p rcc-driver -p rcc-preprocessor --bin rcc --bin rcpp
python3 scripts/benchmark-rcc.py --mode preprocess \
  --compiler target/release/rcpp --baseline /path/to/original/rcpp \
  --output /tmp/rcc-preprocess.json
python3 scripts/benchmark-rcc.py --mode compile \
  --compiler target/release/rcc --baseline /path/to/original/rcc \
  --output /tmp/rcc-compile.json
```

Validation includes 61 preprocessor library tests and the line-splicing
integration test; 347 backend tests; 28 driver integration checks covering
output modes, native programs, continuations and segmented-stack execution;
and 11 C integration cases covering preprocessing, internal linkage, global
initializers and banked pointers. A further 12 real translation units from
the kernel, shell, libc, utilities, AWK and networking matched original
preprocessor output, in addition to the three benchmark sources.
Fresh Riplix builds in separate temporary directories passed the JIT integration
suite in both then-supported MMU=1 and MMU=0 profiles, covering fork/exec, preemption,
indirect file I/O, reboot persistence, cleanup and offline dirty-disk recovery.
The MMU profile also passed its heap/COW and privilege-protection checks.
The MMU=0 build has since been retired; these are historical validation results.

## Reproducing and validating follow-up work

Run from the repository root. A representative terminal compilation is:

```sh
target/release/rcc compile ports/fuzix/ripple/gterm.c -o /tmp/gterm.asm \
  --bank-size 64000 -DRIPPLE_MMU=1 -DNO_64BIT \
  -Iports/fuzix/ripple/include -Iports/fuzix/upstream/Library/include \
  -Iruntime/include -Iports/fuzix/ripple -Iports/graphics/include
```

The `hello.c` sample uses the same flags. For kernel `tty.c`, use:

```sh
target/release/rcc compile ports/fuzix/upstream/Kernel/tty.c -o /tmp/tty.asm \
  --bank-size 64000 -DRIPPLE_MMU=1 -DRIPPLE_BULK_IO=1 \
  -DRIPPLE_CODE_BANKS=1024 -DBANK_SIZE=64000 \
  -Iports/fuzix/ripple -Iports/fuzix/upstream/Kernel/include \
  -Iports/fuzix/upstream/Kernel/dev -Iruntime/include \
  -Iports/graphics/include -Ic-test/examples/rOS
```

For preprocessing-only comparisons, construct independent original/candidate
preprocessors with identical defines and include paths, time their processing
calls, and assert complete output equality. The investigation harness was
temporary; this document preserves its method and results, not a committed
benchmark runner.

Before landing compiler changes, extend coverage for names occurring only in
literals or larger identifiers, recursive and nested expansion, argument
rescanning, stringification, token pasting, redefinition, `#undef`, conditional
includes, file/line builtins and malformed-input diagnostics. Compare outputs
over a broader corpus, then run the relevant compiler and native tests and a
Riplix MMU build/boot check using JIT. Non-MMU Riplix is retired and is no longer
a build or validation target.

For trustworthy performance comparisons, pin dependencies and release settings,
record revision plus local changes, compiler/tool versions and host hardware,
repeat runs with alternating original/candidate order, and separate warm/cold
cache results. Record wall time, CPU time and peak memory. Report single-file,
clean-build and incremental-build results separately, including process/thread
counts. The measurements above are a reason to prioritise the work, not a
replacement for that validation.

## Runtime foundations audit, 2026-10-18

The standalone allocator in `runtime/malloc.c` already uses segregated free
lists and physical neighbor links. Riplix and Fuse-X applications link a different
allocator, `ports/fuzix/ripple/libc/heap.c`. That implementation still searched
every live allocation on malloc/free and traversed the entire heap to coalesce
after each free. It also called both sbrk and brk for each new allocation.
These costs can dominate applications independently of their source language.

The guest allocator now uses the same basic free-list strategy as the standalone
one, with direct header recovery, neighbor coalescing, splitting on shrink and
in-place growth into a free successor. Only free blocks are searched; allocation
can still scan unsuitable blocks within a size class, so this is not a
worst-case constant-time allocator. Heap growth is amortized in 4096-cell chunks,
with an exact-size retry near the heap limit. Direct sbrk reservations remain
separate, and process-local state retains fork's copy-on-write semantics.

The guest API keeps its existing zero-size behavior and 16-bank per-request
bound, including the header.
Headers grow from 5 to 12 C16 cells; each growth can reserve up to 4095 extra
cells for later allocations. Free memory stays available for reuse rather than
shrinking the break. These are deliberate space/time tradeoffs. Programs
must be relinked to use the new allocator. The standalone allocator and the
RVM/RBF instruction sets are unchanged by this follow-up.

The checked workload allocates varied 16–64-cell objects, verifies their full
16-bit contents, frees alternate objects, fills the holes, and frees in reverse
order, for two rounds. Three runs per configuration gave these medians:

| Live objects | Fuse-X before | Fuse-X after | Ratio | RVM interpreter before | RVM interpreter after | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 128 | 26 ms | 8 ms | 3.25× | 1236 ms | 264 ms | 4.68× |
| 512 | 180 ms | 30 ms | 6.00× | 13225 ms | 1112 ms | 11.89× |
| 1024 | 539 ms | 60 ms | 8.98× | 61004 ms | 2443 ms | 24.97× |

These are allocator workload times, not whole-application speedups. Both versions
use the same compiler, kernel and timed source. Baseline runs precede candidate
runs rather than alternating. Measurements use the guest monotonic clock, with
millisecond resolution on this configuration. RVM JIT samples vary substantially
as different batches cross the hot-code threshold; its first batch must not be
treated as a warm allocation measurement.

For completeness, the three-run JIT medians were 656 → 802 ms at 128 objects,
2010 → 65 ms at 512, and 4986 → 76 ms at 1024. The first small batch regressed
in this sample; the candidate allocator has more code to interpret/compile.
Candidate 128-object times ranged from 151 to 842 ms. These observations are
why the interpreter comparison is reported separately, and why a faster warm
allocator does not by itself establish faster cold app startup.

The RVM interpreter report for the complete three-run session, including boot,
shell execution and untimed correctness checks, falls from **4,020,334,877 to
520,241,652 guest instructions** (87.1% fewer). Interactive input/idle scheduling
can affect complete-session counts, so these are not exact per-allocation
instruction counts. This is stronger evidence of reduced guest work than the
timing ratios alone. Timing samples varied even without JIT: the candidate's
1024-object samples ranged from 1156 to 2514 ms on the shared host. The quoted
tables are observations on this machine, not isolated universal speedups.

`heapcheck.c` also checks calloc zeroing/overflow, cross-bank data preservation,
failed realloc preservation, heap exhaustion and coalescing. `heapedgecheck.c`
adds a header crossing a bank boundary, direct sbrk gaps, in-place grow/shrink,
an exact final allocation when chunk growth fails, and child/parent heap
isolation. The runner boots disposable disks and checks filesystem integrity
after shutdown. Raw initial measurements and probe hashes are in ignored
`build/foundation-performance/{before,after,edges}/`.
Interpreter comparisons and complete RVM reports are in
`build/foundation-performance/{before,after}-interpreter/`.

Additional acceptance checks passed libc allocation/I/O/signal tests, a checked
400-line sort, AWK arrays and Tcl evaluation on both systems, plus RVM's large
heap/COW, child-process and kernel/MMIO protection checks. The full RFL OS and
interactive Commander suites passed on both systems, including dialogs, editor
handoff, file actions, program launch and terminal restoration. Its old hard-link
identity assertion for `mc` was updated to assert the current symlink contract.
These checks establish compatibility, not whole-app performance ratios.

Build the normal kernel, shell, echo and user SDK first, then run from the root:

```sh
python3 ports/fuzix/tools/bench_foundations.py \
  --output build/foundation-performance/current
python3 ports/fuzix/tools/bench_foundations.py \
  --source ports/fuzix/tools/heapedgecheck.c --repetitions 1 \
  --output build/foundation-performance/edges-current
```

Use `--os fuzix` or `--os fuse-x` to select a system, `--build-only` to freeze
probes before measuring, and `--measure-only` to reuse the probes in the output
directory. `--engine interpreter` selects RVM without JIT; JIT is the default.
Preserve baseline probes before changing the SDK. Do not compare a newly linked
baseline against an older allocator source: the linked code is what runs.
The `test-heap` Make target in either OS directory builds its prerequisites and
runs the additional edge checks once. Normal filesystem images are untouched.

### Next targets, in priority order

The following are source-audit candidates, not measured speedup claims:

1. **Memory and string primitives, plus pointer induction.**
   `runtime/string.c` and guest `ripple/userlib.c` step banked pointers one cell
   at a time. Ripple lowering normalizes those pointers, and size_t loop counts
   are wide. Introduce bank-bounded spans with narrow inner counters, then
   consider proven loop transformations in the backend. This is the strongest
   shared candidate for RVM and RBF, and would also benefit guest-native code.
   RFL's `lower/string_runtime.rs` emits its own copy loops, so changing libc
   alone would miss string concatenation, slicing and literal construction.
   Test unequal source/destination bank boundaries, overlap direction, zero
   counts and high-bit cells. Preserve MMIO and memory-fault semantics; simply
   replacing every copy with host memcpy would bypass those contracts.
2. **Repeated RFL string construction.** `std/posix.rfl::text_from_c` appends
   one character at a time, while `std/string.rfl::repeat` and `replace` repeatedly
   concatenate strings. The generated concat runtime allocates a new result and
   copies both inputs each time: these loops can do quadratic copying even with
   a fast allocator. A builder with reserved capacity, or a length/copy pass,
   is an additional candidate across all RFL backends. Measure separately from
   C library improvements and preserve string ownership/aliasing semantics.
3. **Fuse-X's generated-code/runtime boundary.** Every scalar load/store and
   GEP currently calls a runtime helper. Wide accesses process individual cells;
   shared kernel cells acquire the arena lock per cell. Measure specialized
   scalar/span access and checked inline pointer arithmetic. The standalone
   native export layer already has bulk memory functions; guest libc definitions
   take their own path, so optimizing only those exports misses applications.
4. **Native leaf calls and continuations.** Resumable code suspends for internal
   calls, including small allocator helpers. A conservative call-graph analysis
   could give proven nonsuspending leaves direct native calls. Preserve indirect
   calls, fork, blocking services, nonlocal jumps and scheduling checkpoints;
   removing continuation handling indiscriminately would break process behavior.
5. **RVM cold JIT cost.** Record compilation time, compiled regions, interpreted
   and native instruction counts alongside app startup and typing latency.
   Existing RAM/TLB fast paths already avoid many host helpers, and data-only
   translation flushes already avoid code-map revalidation. Prioritize measured
   remaining compilation/entry costs rather than reimplementing those paths.
6. **RBF navigation and cold loading.** Re-profile the current CPU export before
   extending summaries: the arithmetic and moving-state improvements in
   [the BF CPU notes](arch/bf-cpu-performance.md) already changed the dominant
   costs. The retained post-change sort profile spends much more instrumented
   time in walk summaries than routed loops. Explore generic navigation/JIT
   integration and parsing costs with the bounded existing benchmark suite.
   Keep BF execution portable and compare exact state, instruction counts and
   BF dispatch counts; historical profiles are not fresh timing evidence.

For each follow-up, freeze matching sources and tools, separate cold startup
from warmed loops, and measure instruction/dispatch reductions as well as wall
time. Real application latency should then be checked in Rift, Commander and
Lynx; a microbenchmark ratio should never be presented as their speedup.

### Bank-aware memory loops and pointer steps, 2026-10-18

Implemented the first memory/pointer follow-up:

- `memcpy`, `memset`, `strlen` and `strcmp` share internal bank-span routines in
  `runtime/include/__banked_memory.h`, used by both standalone `runtime/string.c`
  and the Riplix/Fuse-X guest `ripple/userlib.c`. Source and destination can reach
  bank boundaries at different times. Inner counters and offsets are 16-bit;
  banks and 32-bit remaining lengths change once per span. Standalone `memmove`
  also uses reverse spans for overlapping moves. Other string routines retain
  ordinary pointer stepping; strcpy and comparisons cache loaded characters.
- Constant GEP steps compare the original offset with the crossing threshold.
  One comparison covers both a bank crossing and overflow past 65535. Negative
  steps use a borrow test without decrementing/restoring the bank on every cell.
  Zero-extended word GEP uses the same threshold technique after normalizing its
  inputs, with one fewer scratch register. Live input pointers remain intact.
- Native GEP now honors the shared optimizer's zero-extended narrow-index
  contract. Previously, a removed `ZExt` was replaced by native sign extension,
  turning unsigned indices 32768–65535 negative. Signed C indices keep their
  explicit `SExt` to 32 bits. A focused C regression was confirmed failing on
  native before the fix. This also matters for bank alignment and heap safety,
  independently of the new library loops.

The memory routines use ordinary C16 loads/stores, so each backend retains its
memory checks, MMIO handling and copy-on-write behavior. No host-memory shortcut,
DMA dependency, new ISA instruction or eight-bit byte assumption is introduced.
Sizes still count full 16-bit cells, and the public function signatures stay the
same. Applications must be rebuilt/relinked to use these changes. RFL-generated
pointer steps benefit from backend lowering; its separately generated string
copy/concat loops have not been replaced by these C routines.

A checked workload performs 32 operations on 1200-cell buffers, including an
unequal bank crossing with 2048-cell banks. Complete-program counts, including
buffer initialization, were:

| Operation | Before instructions | After instructions | Reduction |
| --- | ---: | ---: | ---: |
| memcpy | 4,585,015 | 2,337,031 | 49.0% |
| memset | 2,933,279 | 906,097 | 69.1% |
| strlen | 2,316,963 | 857,399 | 63.0% |
| strcmp | 3,585,145 | 2,671,531 | 25.5% |
| overlapping memmove | 5,132,791 | 2,506,375 | 51.2% |

Three interpreter runs per case had median whole-process wall times of
98→57, 69→26, 56→26, 79→60 and 107→67 ms respectively. These short timings include
host startup and run sequentially, baseline first, on a shared macOS ARM64 host;
instruction counts provide the more stable comparison. These are primitive
workload results, not claimed Rift/Lynx/Commander speedups. A separate eight-cell
probe did not regress complete-program instruction counts, but startup/setup
costs and early-mismatch strings should be profiled in real workloads before
claiming universal gains.

The same five operations also ran through actual BF exports with 512-cell banks,
a 520-cell length and one repetition. RVM and RBF instruction counts matched for
each corresponding binary/export, and both old and new versions produced the
checked result. This establishes cross-backend execution, rather than inferring
BF behavior solely from RVM. These shorter BF runs include substantial buffer
initialization, so their ratios are not directly comparable to the table.

Reproduce the comparison after saving a baseline compiler and string source:

```sh
python3 scripts/benchmark-memory.py \
  --baseline-compiler /path/to/original/rcc \
  --baseline-runtime /path/to/original/string.c \
  --output build/banked-memory/current
python3 scripts/benchmark-memory.py --bank-size 512 --length 520 --rounds 1 \
  --repetitions 1 --bf --output build/banked-memory/current-bf
python3 ports/fuzix/tools/test_memory.py
```

The benchmark preserves input sources, hashes and individual execution reports.
It creates an empty private disk file, and the guest acceptance runner creates
and checks disposable root filesystems. Normal root images are untouched.

Validation passed all 350 Ripple backend unit tests, four native backend unit
checks, 25 native compiler/process integration tests, all 105 C memory-category
cases on RVM, and the focused pointer/string
regressions on RVM and native. The complete standalone string implementation was
also compiled into a native test executable (so native service bindings did not
bypass the implementation). Real Riplix and Fuse-X guests passed the 65,550-cell
span/overlap checks, allocation reuse and fork COW isolation, followed by clean
filesystem checks. The bank-alignment portion of this guest test also caught the
unsigned-index bug as heap corruption before the native fix.

The normal 64,000-cell configuration was measured separately with the same
1200-cell, 32-operation workload: memcpy 4,584,113→2,406,471 instructions,
memset 2,932,377→901,969, strlen 2,316,125→819,223, strcmp
3,584,243→2,704,843, and overlapping memmove 4,957,969→2,490,599.
Those buffers stay within their banks; the 2048-cell comparison above exercises
crossings. The eight-cell workload's instruction reductions ranged from 3.1%
(strcmp) to 31.9% (strlen).

In the small BF comparison, dispatches decreased from 41,355,036→30,459,802
(memcpy), 32,265,792→20,778,771 (memset), 27,761,706→20,467,163 (strlen),
35,361,628→33,190,067 (strcmp), and 49,282,124→34,813,824 (memmove).
No BF instruction-recognition shortcut was added; these are reductions while
executing the existing portable CPU export.

## Fuse-X Lynx build pause and native linking (2026-10-18)

An apparently stalled Fuse-X build was doing expensive whole-program native
optimization. Two macOS sampling profiles caught the shared IR optimizer
repeatedly computing dominance facts with cloned tree sets. Loop invariant
hoisting also constructed a validated CFG, discarded its dominance results,
and solved the same problem again with hash sets.

The shared CFG solver now intersects packed 64-bit words during its fixed point,
then constructs the same ordered dominance sets for existing consumers. Loop
invariant hoisting reuses that CFG. Optimization policies, instruction selection,
and the C ABI are unchanged. A comparison against the previous set solver covers
cycles, unreachable blocks, duplicate branch targets, noncontiguous labels, and
sizes around the 64-bit word boundary.

The comparison used exactly the same 241 native IR inputs from the interrupted
Lynx build, including its guest TLS library and libc. Both links produced 2,597
functions, 314,042 global cells, and a 41,455,875-octet package; their serialized
application manifests were identical.

| Measurement | Original compiler | Updated compiler |
| --- | ---: | ---: |
| Native link, including IR loading and native code generation | approximately 346.76 s | 66.76 s |
| Updated compiler: IR preparation and optimization | — | 28.33 s |
| Updated compiler: through native function generation | — | 60.73 s |

This is approximately a 5.2× improvement for this link, not for every build or
runtime workload. The baseline duration comes from the startup IR write just
before launching the linker and the completed package timestamp; the updated
run measures subprocess wall time. Other development processes were running,
so these are local observations rather than controlled machine-wide benchmarks.
Profiles and timing records are retained in `build/native-link-profile/`.

The build machinery had separate problems:

- The default Make graph requested Lynx directly and then requested it again in
  the full application inventory. A changed fingerprint between those passes
  caused a second full build. The inventory now owns the single Lynx build;
  explicit `apps/lynx.fxe` targets remain available. Lynx is required for a normal
  build, so a failed browser build cannot silently count as success.
- Failed application records were cached as reusable results. They now retry;
  successful unchanged applications still reuse their artifacts, and missing
  outputs rebuild.
- Heartbeats repeated the last completed program, such as `yes: built`.
  Application builds now publish start, phase, and finish events. Heartbeats list
  pending applications with elapsed time and their latest phase, including IR
  preparation, optimization, and native function generation. With no tracked
  task they explicitly label the message as the last update.

`RCC_NATIVE_PROGRESS=1` enables the native compiler phase messages. The Fuse-X C
build wrapper enables them automatically and preserves full application logs.
The application and progress regression tests use tiny fake compilers and do not
open the user's guest disk:

```sh
python3 fuse-x/test_build_apps.py
python3 scripts/test_build_progress.py
python3 scripts/test_build_os.py
python3 scripts/test_build_cache.py
cargo test -p rcc-backend --lib
cargo test -p rcc-native
cargo test -p rcc-driver --test native --test native_process
```

Validation passed all 351 backend tests, six native backend checks, 25 native
compiler/process integration tests, and the build/cache/progress regression
checks. The updated Lynx also passed its version check and two local HTML dumps
inside a disposable Fuse-X guest.

The full interactive native browser suite also passed: HTTP redirects and chunked
responses, forms, long requests, cancellation, resize, transfer limits and error
recovery, local navigation, job-control suspend/resume, and browsing over Telnet
while a second shell stayed usable. The normal `fuse-x/build/apps/lynx.fxe` was
rebuilt successfully in approximately 93 seconds (frontend work included), and
an immediate second request reused its successful application cache.
