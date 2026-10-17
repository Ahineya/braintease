# Rift

Rift is a programmer's terminal editor for FUZIX/Ripple and Fuse-X. Its RFL
interface and command layer sit over a portable C text engine. The shell has
Turbo-style menus, tabs and a muted blue palette; the editing surface uses vi
commands. The command is `rift`; `vi` continues to launch Levee.

```text
rift /path/main.rfl /path/other.txt
rift /usr/share/rift/hello.bf
```

Both normal builds include the executable and example configuration/themes.
Rebuild/install through the normal OS launcher to update an existing disk.
Do not install into a disk while its guest is running. To build only the app:

```sh
target/release/rfl build rfl/apps/rift/main.rfl --os fuzix -o build/fuzix/rift.rxe
target/release/rfl build rfl/apps/rift/main.rfl --os fuse-x -o fuse-x/build/apps/rift.fxe
```

A standalone build produces the executable; it does not update a guest disk.

## Rendering and performance

The virtual screen is retained between keystrokes. Menus, tab labels, footer,
messages and cursor shape update only when needed. Cursor motion updates the
line-number emphasis and position without lexing the file again. A text edit
repaints visible file rows conservatively, because multiline lexical state may
affect subsequent lines; blank rows are retained. Resize, dialogs, theme/settings
changes, tab operations and returning from a program invalidate the cached view.

`lib/text_curses` paints directly from the C engine into curses with a bounded
stack cell buffer and the existing theme palette, avoiding a temporary RFL
vector per line. `Buffer.generation()` changes for every mutation, undo, redo
and language change; the undo revision alone cannot detect successive characters
within one insertion group. Vi state is transferred during editing to avoid
copy-on-write cloning of the growing dot-repeat recording on each key.

Configuration reads feed octets to `lib/ini.parse_bytes`, avoiding repeated
whole-string concatenation. The compiler places longer string literals in
deduplicated data and uses a shared typed copy routine instead of emitting one
store sequence per byte. Curses compares composed rows in batches, diffs changed
rows only, and sends no redundant cursor movement for unchanged input refreshes.
Complete-layer comparison preserves overlapping and aliased window behavior.
Identifier highlighting reads a bounded token once and binary-searches sorted
keyword tables, avoiding a full keyword-list scan for every identifier. The
engine checks preserve case-sensitive language keywords and Pascal case folding.

Measure startup and the first twelve typed frames at 80×24 and 120×40:

```sh
python3 rfl/tools/bench_rift.py --os both
python3 rfl/tools/bench_rift.py --os fuzix --document /tmp/main.rfl
```

This uses private disposable images and reports JSON. Startup is measured from
the shell command to the first complete frame; key latency includes guest input,
editing, drawing, JIT warmup and terminal output. Run without concurrent builds
or tests. On macOS the report also includes child-process CPU time, converted
from Mach ticks using the host timebase, to distinguish work from scheduling
delays. RVM uses the normal JIT-enabled configuration; Fuse-X runs native code,
so it still has a substantial advantage over the emulated instruction stream.

The final 2026-10-17 comparison used the same highlighted `/tmp/main.rfl`,
saved original executables, unchanged RVM and the first twelve inserted frames:

| System / terminal | Startup before → after | Median key before → after | Median key CPU before → after |
| --- | --- | --- | --- |
| RVM, 80×24 | 2.574 → 2.352 s | 77.72 → 45.73 ms | 73.10 → 44.20 ms |
| RVM, 120×40 | 5.319 → 2.621 s | 87.09 → 67.96 ms | 78.85 → 65.88 ms |
| Fuse-X, 80×24 | 1.528 → 1.124 s | 25.93 → 10.35 ms | 25.08 → 9.16 ms |
| Fuse-X, 120×40 | 1.495 → 1.091 s | 36.91 → 11.62 ms | 35.99 → 10.42 ms |

The earlier empty-buffer comparison reduced median per-key CPU cost by roughly
59–65% on RVM. Highlighting still costs work, and cold compilation still causes
occasional pauses (the final large-terminal sample peaked at 320 ms). Wall times
also vary with host scheduling; these are observations, not a guaranteed latency
on another machine. Executable loading, cold interpretation/JIT compilation and
the compiler's frame/managed-value overhead remain startup and first-key costs.
Earlier JIT compilation was tested and rejected because it increased pauses;
the VM policy is unchanged. Further compiler work should target small synchronous
wrapper inlining and reducing frame/managed-temporary code volume.

## Editor shell

| Key | Action |
| --- | --- |
| F10 | Open menus: File, Edit, Search, Run, Tabs, Help |
| Ctrl-N | New buffer |
| Ctrl-O / F3 | Open another file in a new tab |
| Ctrl-S / F2 | Save; unnamed buffers prompt for a path |
| Ctrl-W in Normal mode | Close current tab |
| Ctrl-Q | Quit all tabs, asking about unsaved buffers |
| F4 | Choose tab |
| F6 / F9 | Next / previous tab |
| F5 | Save if needed and run using the detected/configured guest tool |
| F7 | Literal search |
| F8 | Show detected development tools |
| F1 | About and command reference |

Menus and prompts use independent floating curses windows. Dialogs support
arrows, Home/End, Delete/Backspace, Ctrl-U, Enter and Esc, with scrollable lists
and resize recovery. The editor retains each tab's cursor and scrolling.
Selection uses a semantic theme role and remains visible in monochrome mode.
A block cursor indicates Normal/Visual mode; a bar indicates Insert mode on
terminals supporting DECSCUSR. The previous screen and default cursor shape
are restored on exit and while a program runs.

## Vi commands

Supported motions: `h j k l`, arrows, `w b e W B E`, `0 ^ $`, `gg G`, `%`,
`f F t T`, `; ,`, Page Up/Down and Ctrl-B/F/U/D. Numeric counts are bounded to
999, including forms such as `3w`, `2dd` and `d2w`. Vertical motions retain
the desired visual column across short lines and expanded tabs.

Editing: `i a I A o O`, `x X s r D C S`, `d/c/y` plus a motion, `dd cc yy`,
`p P`, `>> <<`, `J`, `u`, Ctrl-R, and `.` to repeat a completed change.
An insertion session or a change followed by insertion is one undo group.
In Insert mode, Ctrl-W deletes the previous word. Esc/Ctrl-C returns to Normal.

Text objects: `iw aw iW aW`, inner/around parentheses, square/curly/angle
brackets, and same-line single/double/backtick quotes. Opening and closing
bracket aliases both work, including `ci)`. Nested bracket objects skip quoted
strings and the comments recognized for the selected language. Structural
matching is lexical, not a full parser; `<`/`>` may be comparison operators.
`v` selects characters, `V` selects lines; motions extend the selection, then
`d/c/y/>/<` apply an operation. Registers are shared between tabs.

`/` and `?` search literal text, with `n/N` to repeat/reverse. Search wraps.
This milestone does not implement regular expressions, named registers,
macros, every Vim motion/operator, or a full Vim compatibility layer.

The `:` prompt accepts:

```text
:w [PATH]  :q  :q!  :wq  :x  :qa  :qa!
:e PATH  :tabnew [PATH]  :bn  :bp  :bd  :bd!  :NUMBER
:set number|nonumber  :set autoindent|noautoindent
:set expandtab|noexpandtab  :set tabstop=1..16
:run  :tools  :theme  :help
```

`:e` opens another tab rather than discarding the active buffer. `:q` closes
the current tab and exits when it is the last; `:qa` exits all tabs. Forced
variants deliberately discard unsaved changes. Save As is also in File.

## Files, limits and highlighting

Up to eight buffers are open at once, each bounded to 512 KiB and 65,536 lines.
Gap capacities share a 2 MiB budget; line indexes and history require additional
heap memory. Each buffer retains up to 64 undo groups and 1 MiB of edit payload;
older groups are pruned. Very long insertions can split at the payload bound.
Dot replay holds up to 4096 keystrokes; exceeding that clears repeat and reports
it. Allocation/limit failures are visible and leave the failing edit unapplied.
There is no swapfile or crash recovery yet.

Files are read/written in chunks. LF, CRLF, missing final newlines and original
non-ASCII bytes are preserved; new line breaks follow the first encountered
newline convention. Input/display currently target ASCII: non-ASCII/control
bytes display as `?`, and NUL-containing files are rejected. This is not yet a
Unicode editor. Files beyond the limit are rejected, never silently truncated.

Saving creates an exclusive sibling temporary, checks writes and close, preserves
ordinary file permissions and renames only after completion. The saved revision
tracks undo/redo accurately. Before overwriting, Rift checks the current content
fingerprint and asks if the file changed, disappeared or a Save As target exists.
Another tab cannot save over a file already open in Rift. This is a conflict
check, not filesystem locking or crash-durable fsync. Symlink files are explicitly
rejected: open their target directly. Ownership, hard-link identity, special
mode bits and timestamps are not preserved by replacement saves.

Extension detection selects lightweight lexical highlighting for RFL/C/C++/Rust/
JavaScript, shell/Tcl/awk, Python, Pascal, JSON, INI/TOML/config, Markdown,
Brainfuck, Makefiles, HTML/CSS, Lisp-like text and assembly. The C engine caches
multiline lexical state per line and invalidates it from each edit. Highlighting
is heuristic; it supplies no parser, semantic diagnostics or language server.

## Tools and configuration

The current guest images provide `bf`, `tclsh`/`picol`, `awk`, `sh`, `make`, `m4`
and `dc`. F8 checks executability through the **guest** PATH. C, RFL, Rust and
Pascal compilers are not supplied inside the guest by this feature; their host
compilers cannot be invoked as guest tools. Optional runners become usable when
a compatible compiler/interpreter is installed or a command is configured.

| Path | Purpose |
| --- | --- |
| `/etc/rift.ini` | System editor settings and runner templates; preserved on upgrade |
| `$HOME/.config/rift/config.ini` | User settings/runners layered over system settings |
| `/etc/rift/theme.ini` | System theme selection/overrides; preserved on upgrade |
| `$HOME/.config/rift/theme.ini` | User theme selection/overrides |
| `/usr/share/rift/themes/*.ini` | Installed `turbo`, `graphite`, `mono` and custom named themes |
| `/usr/share/rift/{config,theme}.example.ini` | Configuration examples |
| `/usr/share/rift/hello.bf` | Runnable Brainfuck example; prints `A` |

Create the user directory with `mkdir -p "$HOME/.config/rift"`, then copy/edit
the examples. The generic INI and theme libraries are shared with Commander.
A theme can extend a named base or provide all roles directly. User overrides
without `base` retain the system theme; with `base` they select a fresh base.
`:theme` changes/reloads the session theme without writing configuration.

```ini
[editor]
tabstop = 4
expandtab = true
autoindent = true
numbers = true

[run]
brainfuck = {tool} {file}
c = {tool} -Wall {file} -o {output} && {output}
```

```ini
[theme]
base = turbo
name = My Turbo
[colors]
comment = white, blue
selection = black, white
```

Theme roles are `text`, `comment`, `keyword`, `string`, `number`, `operator`,
`type`, `selection`, `menu`, `tab`, `gutter`, `dialog`, `accent`, `input`, `error`,
`shadow`. Colors and inheritance use the [shared format](../../../libraries/README.md).

Runner tool paths may be overridden through `BF`, `TCLSH`, `AWK`, `SHELL`,
`MAKE`, `M4`, `DC`, `CC`, `CXX`, `RFL`, `RUSTC`, `FPC`, `PYTHON`, `NODE`, `LUA`.
These values name one executable; put extra flags in a template. A
`RIFT_RUN_<language>` environment variable overrides the INI template, for
example `RIFT_RUN_brainfuck`. Language IDs are visible in the status bar/F8.
Templates are trusted guest shell commands. `{file}`, `{dir}`, `{tool}` and
`{output}` are expanded once as individually quoted arguments, preserving spaces,
apostrophes, dollar signs and placeholder-looking bytes in a filename.

Programs run in the file's directory. `{output}` reserves a private temporary
directory for compiled output; the program is launched only if the preceding
compiler command succeeds with `&&`. The output/directory are removed on return.
Curses suspends while output and terminal input belong to the program; Enter
returns to Rift, including after errors or Ctrl-C. Exit results currently display
the guest's encoded wait status. The editor saves before running a modified file.

## Structure and future plugins

| Module | Responsibility |
| --- | --- |
| `main.rfl` | Editor shell, tabs, dialogs, save/conflict policy, commands |
| `vi.rfl` | Vi state machine, motions/operators, selections and replay |
| `ui.rfl` | Layout, viewport, syntax-cell drawing, cursor and status |
| `files.rfl` | Bounded loading, fingerprints, temporary/rename saving |
| `languages.rfl` | Filename detection, lexer and default runner definitions |
| `tools.rfl` | Guest PATH detection, template quoting, program lifecycle |
| `config.rfl` / `theme.rfl` | Application settings and semantic roles |
| `lib/text`, `libraries/text/` | Reusable owned binding and C text engine |
| `lib/tui_dialog`, `lib/path` | Reusable dialogs and lexical guest paths |

The engine is independent of curses and filesystem policy. Edits operate on
byte ranges and history groups; views and language services can evolve without
replacing the text model. Levee is a useful control reference, but its fixed
30,000-character core and global editor state are not used as Rift's engine.

The existing Tcl is the small Picol implementation, with procedures, lists,
expressions, file I/O, `source`, `catch` and external command execution. Its C
`picolRegisterCommand` callback plus private data can expose editor commands.
It is a plausible first plugin language for explicit commands and save hooks;
no benchmark establishes that it is too slow. Keep per-keystroke motions,
highlighting and buffer edits native and pass handles/ranges, not whole files.

Embedding Picol safely needs work first:

1. Separate interpreter/library initialization from its command-line main.
2. Replace process-wide `exit` on OOM and the script `exit` command with
   recoverable editor errors; provide complete interpreter/command teardown.
3. Add evaluation work budgets/cancellation. Depth 32, 128 arguments and
   12,000-character strings/scripts bound some resources but do not stop an
   infinite `while` loop or make callbacks safe during an edit.
4. Expose narrow commands such as `editor::buffer`, `editor::selection`,
   `editor::replace`, `editor::message`, `editor::command`, `editor::on_save`.
   Validate handles and ranges; wrap each plugin change in one undo group.
   Queue hooks after edits; prohibit reentrant buffer mutation and unload hooks
   before freeing their interpreter.
5. Benchmark actual plugins on both kernels, then decide whether parsing costs
   justify cached scripts/bytecode. A new Lisp interpreter is not yet justified.

No plugins are loaded in this milestone. An eventual Lisp-like implementation
could reuse that command boundary and C engine if profiling makes it worthwhile.
Completions, build-output diagnostics, error navigation and LSP are subsequent
work; the current highlight and runner code do not claim those features.

## Validation

```sh
python3 libraries/tests/test_text.py
cargo test --release -p rfl --test rift --test string_slices --test string_literals --test companion_libraries
python3 rfl/tools/test_rift.py --os both
python3 ports/fuzix/tools/test_curses.py --native
python3 ports/fuzix/tools/test_curses.py --build build/fuzix
```

The host engine test uses randomized edits checked against a separate reference,
line-index invariants, grouped undo/redo, CRLF, nested objects/comments,
multiline-color invalidation and a 70 KiB boundary edit. The RFL test drives the
actual vi controller/engine. Guest tests exercise the real terminal, filesystem,
programs, tab ownership and restoration on both kernels using disposable disks.
They independently decode saved disk contents to verify CRLF and 70 KiB edits,
and cover failed saves, external-change confirmation, per-user settings and
a custom runner with an unavailable default tool.
Logs are `/tmp/rift-fuse-x.log` and `/tmp/rift-fuzix.log`.
