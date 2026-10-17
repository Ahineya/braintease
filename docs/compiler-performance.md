# RCC performance investigation

Investigation date: 2026-10-17. The initial measurements and prototype are
retained below. The serial optimisations are now implemented; see
[Implementation follow-up](#implementation-follow-up) for new measurements,
validation and the repeatable benchmark runner.

The largest measured opportunity is avoiding unnecessary macro-expansion work.
Parallel compilation is also feasible, but removing this work should come first.
Neither approach requires changing the C ABI or adding language extensions.

## Where compilation time goes

Three FUZIX translation units were compiled using a copy of the release RCC
binary and their FUZIX include paths and defines. Driver progress messages were
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
whole-compiler table. They do not establish a 120× compiler or FUZIX build
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

[`run-fuzix.sh`](../scripts/run-fuzix.sh) already uses `make -j4`: separate source
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
Fresh FUZIX builds in separate temporary directories passed the JIT integration
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
FUZIX MMU build/boot check using JIT. Non-MMU FUZIX is retired and is no longer
a build or validation target.

For trustworthy performance comparisons, pin dependencies and release settings,
record revision plus local changes, compiler/tool versions and host hardware,
repeat runs with alternating original/candidate order, and separate warm/cold
cache results. Record wall time, CPU time and peak memory. Report single-file,
clean-build and incremental-build results separately, including process/thread
counts. The measurements above are a reason to prioritise the work, not a
replacement for that validation.
