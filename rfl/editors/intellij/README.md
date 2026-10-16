# RFL for IntelliJ IDEA

Language support for `.rfl` files, using IntelliJ IDEA's built-in LSP client and
the existing `rfl lsp` server. Targets **IDEA 2026.1.5** (build 261.27258.48) and
later 2026.1 maintenance releases, using the LSP API introduced in 2026.1.4.
No separate LSP client plugin is required.

## Build and install

From the repository root, build the language server (or use an existing binary):

```sh
cargo build -p rfl
```

Build the plugin against your installed IDEA. Python 3 and the JDK bundled with
IDEA are sufficient; this route downloads no dependencies:

```sh
python3 rfl/editors/intellij/build.py --test --server target/debug/rfl
```

The default installation is `/Applications/IntelliJ IDEA.app`. Elsewhere, pass
`--idea-home /path/to/idea` or set `IDEA_HOME`; Windows installations are also
accepted. If the IDE has no compiler, set `JAVA_HOME` to a JDK 21 or later.

In IDEA, open **Settings → Plugins → gear menu → Install Plugin from Disk** and
select `rfl/editors/intellij/build/distributions/rfl-intellij-0.2.4.zip`. Restart
if IDEA requests it, then open an `.rfl` file.

To update an installed version, use the same **Install Plugin from Disk** action
with the new ZIP. IDEA keeps the executable setting.

## Compile and run

Click the green run icon beside a top-level `fn main`, or right-click a runnable
`.rfl` file in the editor/project view and select **Run**. IDEA creates an RFL
run configuration. **Run** compiles, links and launches the current version of
the program using `rfl run`; output and the program's exit code appear in the
Run window. Its Stop button terminates the compiler and its child processes.

For compilation without execution, select **Build → Compile RFL File**, or use
the same action in the editor/project context menu. **Check RFL File** runs the
compiler's checks without generating an artifact. These actions also work on
library files without `main`. Compiler file/line/column diagnostics are clickable
in the Run window.

Open **Run → Edit Configurations → + → RFL** to create a configuration manually,
or edit an existing one:

- **Source file:** the `.rfl` file to run, compile or check.
- **Action:** Run, Compile or Check. Run already performs compilation, so no
  separate build step is needed before it.
- **Target:** `ripple` (default) or `native`.
- **Working directory:** defaults to the IDEA project directory. Relative source
  and working-directory paths start at the project directory; `$PROJECT_DIR$`
  macros and `~/` paths are supported.
- **Output file:** optional for Compile. Relative paths start at the working
  directory. An empty value uses the CLI default beside the source: `.asm` for
  Ripple, `.native` for native. The parent output directory must already exist.
- **Use Ripple JIT:** available for Run with the Ripple target.
- **Print IR in the Run window:** available for Compile, instead of writing a
  file. Works with both targets.
- **Emulate terminal for program input:** enabled for Run by default. Provides a
  pseudo terminal for keyboard-reading programs; press Enter in the Run console
  to send typed input. The Run console is not a full-screen terminal emulator.

Run configurations use the executable configured under **Languages & Frameworks
→ RFL**, including automatic discovery. Saved configurations retain their
settings across IDEA restarts; subsequent context-menu actions reuse the matching
configuration. IDEA's normal Run/Rerun controls work with them. Debugging is not
implemented.

The plugin invokes the existing compiler and does not rebuild the Rust toolchain.
Ripple runs require the same assembler, linker, VM and runtime artifacts as
`rfl run` from a terminal (`rasm`, `rlink`, `rvm`, `runtime/crt0.pobj`, and runtime
libraries when used). Native compilation requires the host toolchain used by
`rfl compile --target native`.

## Language server

Use **Refactor → Rename** (usually **Shift+F6**) on an RFL declaration or use.
IDEA uses the server's symbol ranges and workspace edits, with its normal rename
dialog, preview and undo. Rename covers local variables, parameters, functions,
constants, types, constructors, fields, methods, traits, associated types and import aliases. A trait
method rename updates its contract, implementations and resolved calls together;
other traits and inherent methods with the same spelling keep their identities.
Project exports update imports and uses in other `.rfl` files, including unopened files;
open files use their unsaved contents and document versions. Explicit import
aliases keep their local spelling when the exported declaration is renamed.

Renaming an unaliased stdlib or external import creates a local alias, for
example `sin` becomes `sin as sine`, and updates calls in that file. The stdlib,
compiler builtins, `main`, and C linkage declarations cannot be renamed.
Comments, strings and import paths are left alone. Struct shorthand expands
when needed to preserve the field or local binding. Invalid names, name capture,
syntax errors affecting the refactor and unresolved member receivers produce
an error instead of partial edits. Workspace searches skip hidden directories,
`target`, `build`, `node_modules` and `vendor`; files outside the workspace are
treated as library imports unless open in the editor. File/module moves are not
part of symbol rename. The rename index deliberately refuses trait member edits
when erased generic arguments or conditional impls prevent safe resolution;
completion and navigation use the full compiler checker. Rebuild the server
when updating this plugin.

An empty **Settings → Languages & Frameworks → RFL → RFL executable** setting
first tries `rfl` on IDEA's PATH (`rfl.exe` on Windows), then searches the project
directory and each parent for `target/release/rfl`, and finally `target/debug/rfl`.
Release builds in any ancestor take priority over debug builds in nearer directories.
This also works when opening `rfl/` alone within the Cargo workspace.

Set an absolute path, a path relative to the project directory, or a command
on PATH to override discovery. Paths containing spaces need no quoting. Supply
only the executable: the plugin adds `lsp` and uses stdio, with the project
directory as the working directory. Applying a changed setting restarts the
server. The editor's language-service widget also provides status, restart and
a shortcut to settings.

To restart manually, use **Tools → Restart RFL Language Server**, or search for
**Restart RFL Language Server** in **Find Action**. This restarts RFL in the
current project and picks up the configured executable or discovery order again.
Open an `.rfl` file if none is open. The language-service widget's restart button
is also available while editing an RFL file. Applying a changed executable setting
restarts the server automatically.

## Features

- Compiler diagnostics as you type, completion and hover documentation.
- Prelude function completion (`print`, `println`, `putc`, `from_char`,
  `from_byte`) with signatures, available without imports.
- Trait and bound highlighting, trait symbols, applicable trait method completion
  after `.`, and qualified completion after `Trait::`, including imported aliases.
  Hover, definition and parameter information work in generic bodies too.
- Associated type symbols, projections, bounds, hover and navigation. Completion
  covers `Self::`, bounded `T::`, concrete names and `<T as Trait>::`; projected
  method results show their normalized types. Rename joins contract, definitions
  and resolved projections. Nested projection completion is not yet provided;
  rename refuses cases whose projected receiver cannot be safely resolved.
- Go to definition, find usages and rename through the server.
- Cmd-click an imported name or alias to open its declaration, or its quoted
  path to open the module file. Supports stdlib and relative imports. Builtin
  exports without a source declaration open their stdlib module.
- Document symbols, parameter information, inlay hints and semantic highlighting.
- Local syntax highlighting, comment/uncomment actions and paired brackets,
  including when the server is unavailable.
- Run gutter and reusable configurations for running, compilation and checks,
  with Ripple/native targets, JIT, terminal input and compiler diagnostic links.
- Alt+Enter import quick fixes for unresolved standard-library functions,
  constants, types and constructors, supplied by the RFL language server.
- Dot completion for stdlib extension functions, with automatic imports when
  selecting an extension that is not yet in scope.

## Import quick fixes

Put the caret on an unresolved standard-library name (for example `sin`, `PI`,
or `List`), press **Alt+Enter**, and choose **Import `sin` from `std/math`**.
The fix inserts a selective import or adds the symbol to an existing import from
the same module. Existing aliases, comments and line endings are preserved.
If several modules export the name, IDEA offers a separate choice for each.
Only exported standard-library names are suggested; project-local import discovery
is not included.

After a receiver such as `b.`, completion includes built-in/inherent methods,
visible applicable trait methods and compatible exported stdlib extension functions.
Trait candidates use the compiler's bound and impl checks, so `T: Read` offers
`Read` methods while an unconstrained `T` does not. Ambiguous trait methods are
omitted; inherent methods retain priority. `Printable.print` and the prelude
`println` extension need no auto-import. Trait method calls navigate to the
contract; implementation declarations also navigate to their contract signature.
Qualified calls such as `Read::read(&value)` have completion and signature help.

A string offers `replace`, `trim`,
`contains`, `split`, and other matching functions from `std/string` even before
they are imported. Selecting one adds its import or merges it into an existing
import. Existing aliases are suggested under the alias with no duplicate import.
Extension discovery is limited to stdlib functions. Private helpers and arbitrary
project free functions are not suggested. This uses the existing extension-call
semantics and does not require traits or changes to the stdlib's declarations.

These actions require the updated **RFL executable**. The existing plugin already
displays LSP quick fixes. Install plugin **0.2.4** for local `trait` keyword
highlighting and correct rename token ranges; restart the language server after
updating its executable. If you installed an older `rfl` on PATH, rebuild it with `cargo build --release -p rfl`, then update
that PATH installation or point **Languages & Frameworks → RFL → RFL executable**
at the new `target/release/rfl`. Restart the language server through its editor
widget or **Tools → Restart RFL Language Server** after replacing the executable.
Leaving the setting empty still gives a PATH installation priority over local builds.

Semantic features have the same scope and limitations as the existing RFL
server. Compilation and execution use the existing CLI. Formatting and debugging
are not provided. The local parser
creates a token-based PSI file for IntelliJ; syntax and type checking remain
in the language server.

## Development

The optional Gradle project uses the IntelliJ Platform Gradle plugin and
downloads IDEA 2026.1.5 by default. With Gradle installed and a JDK 21+ selected:

```sh
cd rfl/editors/intellij
gradle check buildPlugin
gradle runIde
```

Set `IDEA_HOME` to build or run against a local installation instead. The
dependency-free `build.py --test` and Gradle `checkEditor` run the same lexer
and executable-discovery checks. The run-configuration checks cover main detection,
argument construction, validation, persisted settings and independent copies;
Gradle runs these through `checkRunConfigurations`. With `--test --server`, the
build also exercises real checks, assembly/IR/native compilation, Ripple/native
execution, JIT and compiler failure reporting, using a temporary source with
spaces in its path. `--server` checks the actual RFL stdio server, including
full-document edit synchronization and diagnostics recovery. Its separate trait
protocol checks cover imported aliases, bounds, qualified calls, completion,
contract navigation, parameter help, semantic tokens, versioned workspace rename
and malformed impl diagnostics. Associated type protocol checks cover imported
projections, partial-path completion, concrete signatures and workspace rename.
All plugin sources,
checks and plugin build output live in this directory. The existing compiler's
run commands use and clean up their usual workspace `target/` scratch artifacts.
