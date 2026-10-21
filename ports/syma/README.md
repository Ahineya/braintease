# Syma for Fuse-X and Riplix

A shared C16 implementation of Syma's term-rewriting runtime, source loader, terminal REPL and symbolic worksheet.
It runs as a guest application on both execution targets. JavaScript is used only to generate
reference tests; no JavaScript interpreter is involved in running Syma.

## Build and run

From the repository root:

```sh
python3 ports/syma/build.py host
ports/syma/build/host/syma repl
ports/syma/build/host/syma run ports/syma/examples/print-demo.syma

python3 ports/syma/build.py fuse-x --stage fuse-x-shared
python3 ports/syma/build.py fuzix
```

The guest outputs are `build/fuse-x/syma.fxe` and `build/fuzix/syma.rxe` inside this port.
The staging command copies only `syma.fxe` and its `syma/` assets to the shared folder;
it never opens a normal disk image or adds Syma to the default application rebuilds.

In Fuse-X:

```sh
/mnt/host/syma.fxe repl
/mnt/host/syma.fxe run /mnt/host/syma/examples/print-demo.syma
/mnt/host/syma.fxe run /mnt/host/syma/examples/experiments/bf.syma
```

The staged executable finds the adjacent `syma/library` directory. An installed guest can
instead place the library under `/usr/share/syma/library`. `SYMA_PATH` overrides this with
colon-separated search directories. Imports also support `from "./relative-file.syma"`.

Commands:

```text
syma run FILE [--maxsteps N] [--trace] [--arg KEY=VALUE ...]
syma eval 'EXPRESSION' [--maxsteps N] [--trace]
syma repl
syma worksheet [FILE] [--run | --line]
```

`--arg` values are strings in the program's `Args` section. For example, the original
file/JSON demo takes `--arg input=/path/to/file.json`.
`syma normal FILE` prints a JSON AST of the initial normal form without executing effects;
`syma compile FILE` prints the linked Universe before RuleRules expansion. These are useful
for comparing implementations.

## Symbolic worksheet and embedding

The [worksheet guide](WORKSHEET.md) includes a ready-to-run document with the author's
OOP library and a small symbolic mathematics library in one context:

```sh
/mnt/host/syma.fxe worksheet /mnt/host/syma/examples/worksheets/objects-and-math.symws
```

Use F5 to evaluate the document, F6 to run just the selected cell in the live
session, arrows to select, Enter to edit, and F2/F4 to save.
The full-screen interface wraps source and readable results; F7 shows exact raw terms.
Use `--line` for colon commands or `--run` for batch evaluation.
Numbered cells support notes, definitions, named results, editing, reordering and replay.
The public `session.h` interface and the host `libsyma.a` provide in-process evaluation
with independent contexts and rollback on cell errors. See the guide for API constraints
and the mathematics/OOP libraries' supported domains.

## REPL

```text
:rule double Double(x_) -> Mul(x_ 2)
Double(21)

:rule positive Positive(x_) -> x_ :guard Gt(x_ 0)
:rule multiline
unwrap {Box value_}
  -> value_
:end

:module multiline
{Module Demo
  {Rules {R inc {Inc x_} {Add x_ 1}}}}
:end
Demo/Inc(41)

:load /path/to/library.syma
:import Core/Plumb macro
:rules
:trace Double(21)
:set maxsteps 100000
:clear
:quit
```

Expressions continue across lines while delimiters or strings are open. `EXPR \` starts
an explicit multiline expression ending with `.`. `:rule` alone accepts a full `{R ...}`
term ending with `.`; `:rule multiline` and `:module multiline` end with `:end`.
`:match` alone enters a multiline block with `:rewrite`, `:target`, `:norm` or `:universe`
clauses and `:end`. Long pasted lines are assembled before processing.

| Commands | Purpose |
| --- | --- |
| `:universe`, `:u` | Pretty-print the current Universe |
| `:program`, `:p` | Show Program state |
| `:rules-section`, `:rs`; `:rulerules`, `:rr`; `:macro-scopes` | Inspect rule sections and module scopes |
| `:rules`; `:rule NAME` | List rules, or inspect a named rule |
| `:rule NAME PATTERN -> REPLACEMENT`; `:edit NAME ...`; `:drop NAME` | Add, replace or remove rules |
| `:match`, `:m` | Show bindings; optionally substitute or rewrite Universe state |
| `:apply ACTION` | Dispatch an action against the current Program and process effects |
| `:apply RULE EXPR`; `:exec RULE EXPR` | Apply a named rule once, or wrap and normalize an expression |
| `:norm [show]` | Normalize Program and process its effects |
| `:trace [EXPR]`; `:trace verbose`, `diff`, `stats` | Toggle tracing, trace an expression or show statistics |
| `:why EXPR` | Explain root pattern, guard or context failures |
| `:save FILE`; `:load FILE`; `:bundle FILE`; `:reload` | Save, load, compile a module or repeat the last file operation |
| `:import MODULE_OR_FILE [as ALIAS] [from "FILE"] [open] [macro]` | Import a library and optional open names/RuleRules |
| `:export MODULE FILE` | Export a compiled library snapshot, including currently loaded dependencies |
| `:undo`; `:history [N]`; `:clear` | Undo a Universe change, inspect history or reset state |
| `:set trace on/off`; `:set maxsteps N` | Configure evaluation |
| `:help`, `:h`; `:quit`, `:q`, `:exit` | Help and exit |

`$it` refers to the last evaluated result. `:match` defaults to matching sections inside
Universe; `:target EXPR` matches an explicit term and `:norm EXPR` normalizes it first.
Adding `:universe` allows the replacement to update the matching Universe section.
Named `:apply` performs one guarded rewrite without normalizing its result; `:exec`
uses the normal evaluator. `:why` diagnoses the expression root, rather than every subterm.

`:bundle FILE [:args --KEY VALUE --FLAG --KEY=VALUE -k VALUE POSITIONAL]` loads a module without starting
its program. `:norm` starts it. `:load` replaces the current Universe, accepts `.syma` or
JSON AST snapshots, and normalizes/runs their initial Program. Use `:import` to merge
libraries; plain, open, aliased, file and RuleRules imports are supported. `:reload` retains
bundle arguments. Argument values are strings, valueless flags contain `Empty`, and positional
arguments have numeric keys. Existing module-qualified Args sections keep their names.
Export writes the current compiled library snapshot, including dependencies and other loaded
rules, with an empty Program; it does
not reconstruct an original Module source file.

Universe changes are transactional on command failure, and the last 64 changes can be
undone. Undo restores terms/rules/imports; it cannot reverse file writes or other effects
already performed. The last 200 history entries are bounded to 64 KiB. Interactive history
persists privately in `$HOME/.syma/history.syma` with mode 0600. Batch input does not write
history. Automatic tracing, trace style and normalization limits are session settings.

A bad input reports an error and leaves the interactive REPL running. Redirected scripts
return nonzero if any command failed. The original `.symnb` examples can have their code
cells fed into this REPL without a notebook frontend.

## Rift

`.syma` files have dedicated highlighting for structural keywords, options, variables,
rest patterns, signed/exponent numbers, strings/raw strings and comments. Multiline lexical
state and vi bracket navigation follow Syma's comment/string conventions. `.symnb` files
use JSON highlighting and have no direct notebook runner.

F5 saves and runs `syma run FILE`; F8 reports the executable. Rift uses `SYMA` when set,
otherwise `syma` from PATH, falling back to `/mnt/host/syma.fxe` on Fuse-X. `[run] syma`
and `RIFT_RUN_syma` can override the runner template. For the shared-only installation:

```sh
/mnt/host/rift.fxe /mnt/host/syma/examples/print-demo.syma
```

## Implemented runtime

- Symbols, binary64 numbers, UTF-16 strings, compounds, expression heads and empty compounds.
- Brace/function syntax, variables and rest variables, greedy anchors, raw strings and comments.
- Structural matching, repeated bindings, rest splitting, alternation/capture, negation,
  repetition, regular-expression captures and meta-matching.
- Stable priorities, guards, innermost rules, `:scope`, `:with`, `:let`, quoted patterns,
  `Inert`, `MetaSafe`, substitution and splicing.
- Arithmetic, string and sequence operations, structural/normalized comparisons, booleans,
  type checks, JS-compatible 32-bit operations, serialization, IDs and random values.
- Modules, relative/standard imports, aliases/open exports, qualification, definitions and
  module-scoped RuleRules. The original standard library is shipped unchanged.
- Print, ReadLine, GetChar, file/Syma-file read/write, random, timers, animation-frame timing,
  storage and Exit effects. Input and timers share a blocking `select` loop. Partial input
  survives other effects and normalization. GetChar accepts a key without Enter on a terminal.
- Storage uses `$HOME/.syma/storage.syma`; it falls back to the current directory when HOME
  is absent. The runtime does not put its state in `/` explicitly.

The browser frontend, DOM renderer, HTML projection, networking/WebSocket services, external
process execution and package manager are outside this terminal bring-up. Symbolic UI and
HTML-projection terms remain data. Unavailable effects produce error replies.

## Representation and limits

Terms are immutable, hash-consed nodes addressed by 32-bit IDs in a paged arena. A balanced
persistent sequence with eight-element leaves shares unchanged compound children and rest
spans. Cached structural hashes compose independently of rope shape. Structural equality
normally becomes an ID comparison. An explicit mark/sweep collector reclaims terms and
sequence nodes between normalization steps and REPL commands; RuleRules compilation also
collects at allocation checkpoints.

The matcher uses an explicit task/choice stack, a binding trail and cached binding slots.
Rest alternatives are generated lazily, with shortest-first/greedy order matching the reference.
Primitive names and pattern operators receive integer dispatch tags at symbol creation.
Closed rest spans retain their existing sequence, and argument lists that cannot match any
atom rule are skipped without scanning every cell. Small matcher, traversal and sequence
allocations are pooled to avoid guest allocator overhead. Hash-table epochs avoid clearing
the entire weak intern table at each collection.

Rule lookup merges a head/atom bucket with universal rules in stable priority order. Ordinary
traversal follows the reference's actual FIFO order; innermost traversal is child-first.
Guards exclude all rules already being evaluated by enclosing guards. Context-dependent
results and impure primitives are not memoized.

For scale, a measured host build on the development Mac completes a full launch,
parse/index of 5,000 unrelated rules and 129 rewrites in about 48 ms. Building a
3,000-item flat sequence through rewrites takes about 14 ms. These are host timings,
not guest guarantees: C16 execution, especially under RVM, adds substantial overhead.
`tests/benchmark.py` records reproducible whole-process measurements in `build/`.

Lengths and indices use explicit 32-bit types despite C16's 16-bit `int`. Strings retain UTF-16
code units internally, including embedded NULs and surrogate pairs; files/terminals use UTF-8.
The existing adapted QuickJS regexp, Unicode and decimal-conversion C libraries are linked
independently, without the QuickJS language runtime.

The default normalization limit is 10,000 steps and is configurable. Parsing/substitution
and primitive folding limit nesting to 256; nested guard normalization, regexp work and pattern
search also have resource limits. Riplix retains its bounded guest heap, so it cannot accommodate
the same maximum Universe size as hosted Fuse-X. These limits report errors rather than
pretending that an unfinished rewrite reached normal form.

Two deliberate improvements over the audited CLI are handling `//` and `/* ... */` comments
and starting effect processing after normalization. The old CLI could miss newly generated
effects, and its module loader skipped the register module in the VM demo because of comments.
The old `test.syma` demo has inconsistent anonymous rest counts; this port reports that error.
Some saved notebook displays predate current reference semantics; tests compare the current
engine, rather than treating those historical displays as authoritative.

## Validation

```sh
python3 ports/syma/tests/test.py host
python3 ports/syma/tests/test.py native
python3 ports/syma/tests/test.py ripple
python3 ports/syma/tests/repl.py host
python3 ports/syma/tests/repl.py native
python3 ports/syma/tests/repl.py ripple
python3 ports/syma/build.py host --sanitize
UBSAN_OPTIONS=halt_on_error=1 python3 ports/syma/tests/test.py host \
  --binary ports/syma/build/host/syma-sanitize
python3 ports/syma/tests/benchmark.py
```

Guest tests construct new disks from explicit manifests and shut them down afterwards. They
never clone or boot an installed personal image. Logs and build outputs are under `build/`
inside this port, which is ignored by Git.

The portable corpus currently contains 318 evaluations: 267 calls captured while running the
187 original core tests, 37 additional edge cases and 14 evaluations extracted from all seven
notebooks. The original standard library and demo programs additionally exercise the native
parser/module loader, console input, NUL/Unicode file round trips, persistent storage and timers
while input is pending. A deliberately wrong expected result verifies the test runner fails.

The initial full host, Fuse-X and RVM runs all passed, including clean guest shutdown/filesystem
checks; the host suite also passed with undefined-behavior instrumentation. On the development
Mac, the complete integration runs took about 2 s hosted, 189 s on Fuse-X and 887 s on RVM.
These totals include the symbolic notebook workloads and program loading, not just startup.


The expanded REPL has separate command-workflow tests on host and both guests, including
state inspection, rule edits/undo, match/rewrite, actions, imports/aliases, bundle arguments,
save/load/reload, raw/multiline strings, input effects and persistent interactive history.
`rfl/tools/test_rift_syma.py` drives highlighting, F8 tool detection, F5 execution/error return
and clean shutdown on disposable images; Fuse-X tests the shared-folder fallback directly.
The portable text-engine tests cover Syma lexical convergence and bracket navigation.

To regenerate reference expectations, provide an isolated copy of the Syma repository with
its dependencies available:

```sh
python3 ports/syma/tests/record.py --reference /path/to/isolated/syma \
  --node-modules /path/to/syma/node_modules
node ports/syma/tests/oracle.mjs /path/to/isolated/syma
node ports/syma/tests/notebooks.mjs /path/to/isolated/syma
```

`record.py` temporarily instruments that copy and restores it in `finally`; do not point it at
an actively used source checkout. Checked-in fixtures run without Node or the side repository.
See `PROVENANCE.txt` and `tests/source-manifest.json` for the original snapshot and licenses.
