# Objects and algebra worksheet

The worksheet is a full-screen curses application with numbered code and text cells.
It runs the C16 symbolic engine in process. OOP and mathematics share the same
Universe; neither library is hard-coded into the evaluator.

## Try it in Fuse-X

```sh
/mnt/host/syma.fxe worksheet /mnt/host/syma/examples/worksheets/objects-and-math.symws
```

The same command opens a cell list and a reading pane with wrapped source and results.
Select a cell with the arrows. F5 runs the document; Enter opens its cell editor.
The example combines inheritance, method override, an is-a test, differentiation,
substitution, expansion and named results. The polynomial `x^3 + 2*x` evaluates
to 33 at x=3; its derivative evaluates to 29.

The screen uses the project's shared curses and Unicode/text libraries, including
Rift's gap-buffer editor, highlighting, grapheme navigation, soft wrapping and
undo. It works at 80×30, adapts to resizing down to 60×14, and honors NO_COLOR.
Source wrapping is visual: it never inserts line endings into saved code.

## Worksheet controls

| Key | Action |
| --- | --- |
| Up / Down | Select a cell |
| Tab, Left / Right | Switch focus between the cell list and reading pane |
| PgUp / PgDn, mouse wheel | Scroll complete source and results |
| Enter / E | Edit the selected cell |
| A / T | Insert a code cell / note after the selection |
| D / Delete | Remove a cell, with confirmation |
| `[` / `]` | Move the selected cell up / down |
| F5 | Evaluate the entire worksheet |
| F6 | Evaluate only the selected cell in the live session |
| F7 | Toggle readable math / exact raw normal form |
| F2 / Ctrl-S | Save the worksheet |
| F3 / F4 | Open / Save as |
| F8 | Set the saved evaluation limit |
| F1 | Help |
| Q / Ctrl-Q | Close; save, discard or cancel when there are changes |

The list shows explicit `...` previews. Selecting a cell displays its complete,
wrapped contents in the reading pane. Readable results show expressions such as
`3 * x^2 + 2`; F7 exposes the exact namespaced term. This presentation never
changes cached terms, evaluation or file contents.

Cell labels have these meanings:

- **Not run:** no result has been computed.
- **Ready:** evaluated during this session.
- **Needs update:** a result is cached from a saved document or an earlier edit.
- **Error:** evaluation failed; the source remains available to edit.

F6 evaluates only the selected code cell, using the session's existing imports,
rules and named values. It never runs earlier cells for you. Run the imports and
definitions you need first, or use F5 to evaluate the entire document. A successful
cell run marks later cached results as needing an update; it does not run them.

F5 starts with a fresh context and proceeds in document order, stopping at the
first error. Editing, moving or deleting a cell invalidates affected results but
keeps the live session. Use F5 to remove old definitions from the session or rebuild
after structural changes. Esc stops a full run between cells; evaluation inside an
individual cell is synchronous. Runs keep your selection and reading position.
Opening a saved worksheet starts a new session: cached output never silently
becomes executable state. F8 changes the rewrite limit without losing definitions.

## Editing a cell

The editor uses arrow keys, Home/End and PgUp/PgDn. Ctrl-A selects the whole cell;
Ctrl-Z/Ctrl-Y undo and redo. Enter inserts a line ending and Tab inserts four spaces.
Unicode cursor movement and deletion operate on whole graphemes. Bracketed paste
inserts bounded batches in one undo group, preserving embedded line endings.

F2 applies edits and returns to the worksheet. F5 applies and evaluates only
that cell in the live session. Ctrl-S applies and saves the worksheet. Esc cancels edits and asks
before discarding changed text. The editor and reading buffers have Rift's
512 KiB text limit. Larger existing cells remain intact; the screen shows an
explicit limit notice instead of a truncated or editable substitute. Use the
line interface or an external editor for those cells.

Code cells contain **one complete Syma term**, which can span multiple lines.
Text cells hold notes. Worksheet cells do not contain REPL colon commands.

## Saving and script mode

Documents store source, cached output and the evaluation limit. Saves use a private
temporary file and rename it over the destination after a successful write.
There are limits of 1 MiB per code cell, 4 MiB per document and 512 cells.
Import paths are relative to the process's working directory.

`syma worksheet FILE --run` evaluates a document and exits on completion or error.
It never opens curses; this remains Rift's F5 command for `.symws` files.
Redirected input also keeps the line interface. To request it explicitly:

```sh
syma worksheet FILE --line
```

Its commands are `:list`, `:show [N]`, `:add code|text`, `:edit N`, `:remove N`,
`:move N M`, `:run [N|all]`, `:limit N`, `:save [FILE]`, `:open FILE` and `:quit`.
A lone `.` ends the line-based cell editor; `..` inserts a literal line containing
`.`. The host and Riplix executables support all three modes.

## Definitions

- `{Import MODULE [as ALIAS] [from "FILE"] [open]}` loads a library.
- `{Module ...}` merges a source module into the current context.
- `{R NAME PATTERN REPLACEMENT ...}` adds a rewrite rule.
- `{Define $name EXPR}` evaluates and stores a named value.
- `$name` references that value, and `$it` references the last expression result.

Bindings are immutable snapshots of evaluated terms, not automatically reactive
formulas. Rerunning the worksheet recomputes them. An error rolls back the entire
cell, including its rule and binding changes. Cells normalize terms; they do not
dispatch the REPL's console/file/timer effect loop. A `{Pending ...}` term remains
data. Library imports can read files, and primitives such as Debug can write to
stderr; this interface is not a security sandbox.

## The OOP library

`{Import OOP/Objects as OOP}` exposes:

```syma
{OOP/Run {Program
  :class Animal { walks() => true }
  :class Dog :extends Animal { bark() => "woof" }
  :class FlyingDog :extends Dog { flies() => true walks() => false }
  :var pet := :new FlyingDog()
  :print(pet.bark())
  :print(pet.flies())
  :print(pet.walks())
  :print(pet is Animal)
}}
; {Output "woof" true false true}
```

This is adapted from the author's OOP example in `Syma-Overview.symnb`, cells
67–68. The original source cells are preserved in `examples/oop/`. The reusable
version has explicit module exports, a public Program entry point, delayed string
conversion during desugaring, and grammar-token comparisons across module
namespaces. Its desugar → class analysis → execution stages remain symbolic.

It is the notebook's small demonstration language: parents precede children,
methods take no arguments and return a term, and classes have single inheritance.
It has no mutable fields, constructors with arguments, access control, or general
method bodies. As in the original notebook, unsupported commands are skipped;
it is not a validating general-purpose OOP language. Missing methods produce no
output. `:print` collects values into an Output term rather than printing to the
host console.

## The small mathematics library

`{Import Math/Small open}` exposes binary `Sum`, `Product`, `Power`, symbolic
`Sin` and `Cos`, and these operations:

```syma
{Simplify {Sum {Product 1 x} 0}}             ; x
{Differentiate {Sum {Power x 3} x} x}        ; 3*x^2 + 1 (as a term)
{Substitute {Power x 3} x 2}                 ; 8
{Expand {Product a {Sum b c}}}               ; a*b + a*c
{Differentiate {Sin {Power x 2}} x}          ; cos(x^2) * 2*x
```

Constructors simplify numeric operations, zero/one identities, repeated factors
and a few like-coefficient cases. Expand distributes multiplication over sums
only when explicitly requested. Substitute walks argument trees, including unknown
function calls; it is structural substitution, without binder/capture analysis.
Differentiate supports constants, independent symbols, sums, products, positive
integer powers, sine, cosine, and the chain rule. Unknown functions and unsupported
powers return UnevaluatedDerivative instead of an invented answer.

This is a small algebra over finite real polynomial expressions, not a full CAS.
It does not impose a canonical order or collect every equivalent polynomial.
Powers outside the nonnegative integer domain stay symbolic. Symbolic x^0 uses the
polynomial identity 1; explicit numeric 0^0 stays unevaluated. Numeric constants
use Syma's binary64 arithmetic, so they are approximate and have finite range;
there are no exact rationals, arbitrary-precision coefficients, solvers, integrals,
or assumptions engine yet. Rules do not cancel x/x or assume x is nonzero.

## Embedding foundation

`session.h` is the public C interface. A host build also produces `libsyma.a`.
The same session implementation is compiled and exercised as a C16 application
on Fuse-X and Riplix.

```c
#include "session.h"

syma_runtime_open(argv[0]);
SymaContext *ctx = syma_context_new(100000);
SymaResult result;
if (syma_context_eval(ctx, "{Add 20 22}", &result)) {
    puts(result.text);
} else {
    fprintf(stderr, "%s\n", result.error);
}
syma_result_free(&result);
syma_context_free(ctx);
syma_runtime_close();
```

Initialize the runtime once before creating contexts, and close it after all calls
finish. Contexts have separate rules, bindings, and last results. Their immutable
terms share an interned arena; every live context participates in garbage
collection. Result text is independently owned until freed. No raw term IDs or
collector roots escape the interface. Calls are serialized: the engine's term
store, module loader, matcher pools, and error boundary are not thread-safe.
Do not mix this lifecycle with the CLI REPL in one process.

The rewrite limit detects rewrite loops. It is not a wall-clock deadline or a
memory quota, and does not bound every pattern match or primitive operation.
Cancellable work slices, host effect callbacks, per-context heap isolation, and
lowering DSLs to C16/RFL are future extensions.

## Verification

```sh
python3 ports/syma/tests/worksheet.py host
python3 ports/syma/tests/worksheet.py native
python3 ports/syma/tests/worksheet.py ripple
python3 ports/syma/tests/tui.py host
python3 ports/syma/tests/tui.py native
python3 ports/syma/tests/tui.py ripple
```

These exercise 398 embedding checks, the original OOP program and math results
against eight original-JavaScript-engine expectations, document editing,
reordering, stale results, saving/reopening, invalid documents and error recovery.
Host checks also compare polynomial derivatives at 18 numeric points. Guest
images are newly constructed from an explicit file list, without opening normal
disk images. `tests/worksheet-oracle.mjs` regenerates the eight reference results
using the existing isolated JavaScript reference snapshot.

The TUI tests drive real terminals: editable cells, page-sized Unicode paste,
whole-grapheme deletion, undo/redo, error correction, movement/deletion, cached
results, single-cell session state, incremental screen updates, file errors, saved limits, resize anchoring, mouse selection and terminal
restoration. Their snapshots come from the actual ANSI output. The host build
uses the same curses implementation with its POSIX input adapter.
