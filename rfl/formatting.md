# RFL formatting

RFL has one opinionated formatter shared by the compiler CLI, language server
and IntelliJ IDEA plugin. It uses four-space indentation and has **no maximum
line width**. Long signatures, calls and expressions stay on one line unless
their syntax or comments require a line break.

## Style and safety

- Opening block braces stay on the header line; nonempty blocks have indented
  bodies and a closing brace on its own line. `} else {` stays together.
- Struct declarations and match arms use multiline layout. Struct literals,
  destructuring patterns and selective imports use compact spaced braces.
- Operators have consistent spacing; unary operations, references, generic
  arguments, paths, calls, indexing and ranges use syntax-aware tight spacing.
- Lists remain compact without a width limit. Comments inside lists force
  multiline layout so a line comment cannot hide the next element.
- Blank lines are limited to one between tokens; an existing separating blank
  line is retained. Top-level block declarations are separated by a blank line.
- Nonempty output ends with one newline. Existing CRLF line endings are retained;
  otherwise generated line endings are LF. Whitespace-only input becomes empty.
- Token spellings are preserved, including numeric bases, escapes, strings,
  comments, parentheses, commas and declaration/import ordering. Comment text
  is not reflowed, including indentation inside multiline comments and literals.

For example:

```rfl
fn answer(x:i16)->i16{let y=x+1;if(y>0){y}else{0}}
```

becomes:

```rfl
fn answer(x: i16) -> i16 {
    let y = x + 1;
    if (y > 0) {
        y
    } else {
        0
    }
}
```

Formatting only needs valid syntax, not successful name resolution, imported
files or type checking. It parses Ripple, native and Wasm target configurations so conditional
declarations receive the same style. The formatter verifies that
ordinary token kinds and spellings survive unchanged and reparses the result before
returning it. Invalid or incomplete syntax is left unchanged by the editors;
the CLI reports an error. Reformatting an already formatted file is a no-op.

This is a whitespace formatter rather than a Rust source rewrite: it does not
insert trailing commas, remove parentheses, reorganize imports or normalize
literal spellings. Editor indentation preferences do not override its fixed
style, and no rustfmt installation is needed.

## Standard HTML macros

An explicit macro import from `std/html_macro` enables HTML layout, including
`import macro {html as view} ...` aliases. Other macro packages keep their exact
bodies, even if a macro is named `html`. An alias selecting different DSLs under
different target cfgs stays opaque. Formatting does not load imports or run
macro implementations. Reexports through other modules remain opaque.

```rfl
html!{
    <section>
        <input
            class="edit"
            value={title}
            on_input={fn(value: String) => EditTitle(value)}
        />
        <p>Hello, {name}!</p>
    </section>
}
```

Trees are indented; tags with three or more properties, multiline expressions
or line comments put properties on separate lines. Embedded RFL uses the same
formatter, including nested HTML. Mixed text stays inline and its spelling is
never reflowed. Comments and literal contents are preserved; expressions with
multiline comments/literals retain their original interior.

The standard macro ignores **whitespace-only child runs containing a newline**
outside `pre` and `textarea`. These are layout, not DOM text nodes. Inline spaces
remain significant: `<strong>A</strong> <em>B</em>` retains its separating space. Raw text
containing non-whitespace, explicit entities and `{expressions}` retain their
contents. `pre`/`textarea` descendants preserve all raw whitespace. Use `{" "}`
or `{"\n"}` for an explicit whitespace text child in an otherwise indented tree.
This rule also applies to manually indented standard HTML macros; previous
newline-only child nodes are no longer generated.

The formatter compares parsed HTML trees and embedded RFL tokens before returning
edits, preserving text, property order and expression evaluation order. Malformed
or unsupported DSL forms remain opaque. It never repairs HTML or rewrites a macro
into ordinary source. Editor changes remain whitespace-only, small and selection
bounded inside macros as well as outside them.

## Command line

```sh
# Format one or more files in place.
rfl fmt program.rfl library.rfl

# Exit 1 if any file needs formatting; do not write files.
rfl fmt --check program.rfl library.rfl

# With no file arguments, read stdin and write formatted source to stdout.
rfl fmt < program.rfl
rfl fmt --check < program.rfl
```

All named files are read, parsed and formatted before any is overwritten, so
a syntax error in a later input does not partially format earlier inputs.
Files are explicit arguments; the command does not recursively traverse a
directory. Diagnostics and check-mode filenames go to stderr.

## Language server and IDEA

`rfl lsp` advertises `textDocument/formatting` and
`textDocument/rangeFormatting`. It formats the current open document, including
unsaved changes, and returns small whitespace edits with UTF-16 positions.
Formatting requests run on the source/transport thread, independently of the
serialized semantic worker. They do not wait for dependency fingerprinting,
macro execution or type checking, including after unsaved document changes.
Diagnostics retain their document version so a client can discard old results.
The client owns applying them; formatting never writes an editor's file behind
its back. An unchanged, unknown or syntactically invalid buffer returns no edits.

Selection formatting computes layout using the whole document for context but
only returns changes wholly inside the requested range. A zero-length selection
does nothing. Whitespace spanning a selection boundary is left alone rather
than changing adjacent code. A whole-file pass supplies the complete canonical
layout.

In IDEA, use **Code → Reformat Code**, optionally with a selection. Formatting
also participates in the IDE's Actions on Save when enabled there; the plugin
does not force formatting on every save or keystroke. Install the updated
plugin and rebuild/restart the selected language-server executable. See
[IDEA setup](editors/intellij/README.md#format-code).

Other LSP clients can use the same requests and their normal format-on-save
settings. The bundled minimal VS Code extension requires its own formatting
provider registration before exposing these requests through VS Code commands.

## Validation

```sh
cargo test --jobs 2 -p rfl --test format --test html_format
cargo test --jobs 2 -p rfl --lib formatting
python3 rfl/editors/intellij/build.py --test --server target/debug/rfl
```

The corpus check formats every `.rfl` file under `std`, `examples` and
`benchmarks` in memory and checks idempotence and token preservation, without
rewriting those files. Protocol checks cover unsaved buffers, UTF-16 positions,
CRLF, selection boundaries, syntax-error refusal, CLI check mode and file writes.

The HTML regressions compare compiled Native and RVM output before and after
formatting, including inline spaces, Unicode and preserved elements. The stdio
`lsp_formatting_latency.py` test requires formatting to answer while a real
runaway macro blocks the semantic worker; its diagnostics must still arrive.
IDEA tests cover markup/RFL token colours, incomplete input and every incremental
highlighting restart boundary. Executable discovery is unchanged.

### Timing record (2026-10-18)

For TodoMVC's original unformatted `view.rfl`, the previous release server took
about 9 seconds for open-and-format. The new release server returned all 220
whitespace edits in 2–10 ms across local runs while semantic analysis ran independently. Already
formatted buffers took 3–8 ms. These are local observations, not portable timing
guarantees. A separate runaway-macro regression returned formatting in 2.2 ms
and subsequently delivered the versioned macro diagnostic; its two-second guard
checks starvation rather than a microbenchmark threshold.

Final checks passed: 11 HTML formatter tests, 16 existing formatter/corpus tests,
24 compiled-Wasm browser tests, 27 Wasm backend tests, 30 macro acceptance tests,
15 real-stdio LSP suites, plugin lexer/restart/run-configuration checks, and all
five TodoMVC integration scenarios against the rebuilt, formatted application.
The plugin artifact is `rfl-intellij-0.2.7.zip`.
