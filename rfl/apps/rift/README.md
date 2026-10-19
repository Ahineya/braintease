# Rift

Rift is a programmer's terminal editor for Riplix/Ripple and Fuse-X. Its RFL
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

## Standalone macOS

Build Rift as a normal terminal application, without a kernel or disk image:

```sh
python3 rfl/apps/rift/build-macos.py
build/rift-macos/bin/rift /path/to/a/file.txt
```

`build/rift-macos/Rift.command` also opens the editor in Terminal. Keep `bin/`
and `share/` together when moving the application; no checkout or compiler is
needed to run the resulting executable. The build runs on macOS and targets the
build machine's architecture. `--prefix PATH` selects another output directory.

The editor uses real host files, arguments, environment and terminal modes.
Settings remain in `~/.riftrc` and `~/.config/rift/`. F5 runs installed host tools;
the terminal is suspended for the child and restored when returning to Rift.
`RIFT_DATA_DIR` overrides the bundled `share/rift` directory, and `RFL_DATA_DIR`
can override its parent. Theme lookup otherwise follows the executable's location.
The current editor limits (eight tabs and 512 KiB per file) still apply. The
filesystem API rejects oversized metadata rather than silently truncating it;
native directory entries retain full filenames up to 255 octets.

The reusable build interface is:

```sh
target/release/rfl build rfl/apps/rift/main.rfl --os macos \
  --build-dir build/rift-macos/sdk -o build/rift-macos/bin/rift
```

The convenience builder also stages themes/examples. The native backend keeps
its compatibility memory layout; explicit copy adapters cross into macOS APIs.
The RFL editor, portable text/INI libraries, and vi commands use the same sources
as the guests. The shared curses and theme implementations compile with the host
C compiler in the runtime's opt-in `host-tui` feature. A small cell adapter maps
interned grapheme IDs between the two Unicode instances. No interpreter, hosted
kernel, mounted disk, or system-wide installation is involved.

Verify against an isolated HOME and disposable host files:

```sh
python3 rfl/tools/test_rift_macos.py
cargo test --release -p rcc-native-runtime --features host-tui --lib native_os::tests
```

The terminal test covers UTF-8 paste, atomic saves, one-group undo/redo, resizing,
long filenames, named themes, configuration reload, relocation, host commands,
and terminal restoration on both normal exit and SIGTERM.

## Rendering and performance

The virtual screen is retained between keystrokes. Menus, tab labels, footer,
messages and cursor shape update only when needed. Cursor motion updates the
line-number emphasis and position without lexing the file again. The engine
accumulates all edits since the previous frame, then propagates multiline syntax
state until it converges with the cached suffix. Ordinary edits repaint only the
affected rows; newline edits conservatively repaint the suffix. Selection changes
repaint their old/new rows, and blank rows are retained. Resize, dialogs, theme/settings
changes, tab operations and returning from a program invalidate the cached view.

`lib/text_curses` paints directly from the C engine into curses with a bounded
stack cell buffer and the existing theme palette, avoiding a temporary RFL
vector per line. `Buffer.generation()` changes for every mutation, undo, redo
and language change; the undo revision alone cannot detect successive characters
within one insertion group. Vi state is transferred during editing to avoid
copy-on-write cloning of the growing dot-repeat recording on each key.
`Buffer.damage()` consumes accumulated row damage before painting (one renderer
per buffer). Painted rows cache their outgoing lexical state, and `paint_line`
reports right overflow without a separate full-line visual-width scan. Loading,
conflict hashing and saving reuse initialized 4096-octet buffers and valid prefix
lengths; EOF and short reads never serialize the unused tail.

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
python3 rfl/tools/bench_tui.py --os both
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

The 2026-10-18 paste checks use disposable images and the real terminal path:

```sh
python3 rfl/tools/test_rift_paste.py --os both
# Optionally compare saved previous executables:
python3 rfl/tools/test_rift_paste.py --os both --baseline /path/to/previous
```

Observed 512-character unbracketed input bursts fell from 10.03 to 0.09 seconds
on Fuse-X and from 21.68 to 0.52 seconds on RVM. An 8,022-octet bracketed Unicode
paste took 2.18 and 2.13 seconds respectively at 60×30. These include input,
editing and final rendering; timing varies with host load and JIT warmup.
The checks verify saved octets, literal controls, a fragmented paste terminator,
whole-paste undo/redo, short-burst dot-repeat, visual wrapping/resizing and saved
typing-format settings after restart. RVM forwards the host terminal library's
paste events with their boundaries; shared curses batches reads, and Rift edits
in bounded chunks with a final repaint. This uses the existing console path,
without a new DMA protocol.

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

Ctrl-O/F3 and File → Open use a browsable floating file dialog, initially in the
active document's directory (or the startup directory for an unnamed buffer).
Type a path directly, or Tab to its directory panel. Enter enters a directory
or opens a regular file; arrows, Home/End and Page Up/Down move selection, and
typing in the list finds a name prefix. Ctrl-B goes up, Ctrl-T toggles hidden
files and Ctrl-R refreshes. Tab returns to filename editing, with arrows,
Home/End, Delete/Backspace and Ctrl-U. Esc cancels without changing tabs or
buffers. The picker preserves its state through resize, including a temporarily
small terminal. Selecting an already-open file switches to its tab, even when
all eight slots are occupied. New named buffers remain available through
`:e PATH`; Save As keeps its editable path prompt.

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
:set wrap|nowrap  :set textwidth=0..4096
:set formatoptions+=t  :set formatoptions-=t
:run  :tools  :theme  :help
```

`:e` opens another tab rather than discarding the active buffer. `:q` closes
the current tab and exits when it is the last; `:qa` exits all tabs. Forced
variants deliberately discard unsaved changes. Save As is also in File.

Long lines wrap visually at word boundaries by default, with a `>` in the
continuation gutter. This preserves the file's line breaks. Arrow keys and
Page Up/Down follow displayed rows; vi `j/k` still follow file lines. `:set nowrap`
restores horizontal scrolling, and `:set wrap` enables visual wrapping again.

To insert actual line breaks while typing prose, use `:set textwidth=80` and
`:set formatoptions+=t`. Formatting breaks at spaces/tabs, using terminal column
widths and the file's newline convention; long words remain intact. This applies
to newly typed text, without reformatting the existing document. `textwidth=0`
(the default) or `formatoptions-=t` disables it. Bracketed clipboard pastes keep
their original bytes and line breaks, including when typing formatting is on.

Accepted `:set` changes automatically save editor options to `$HOME/.riftrc`
and load on the next launch. Write errors appear in the status line. The dotfile
uses an `[editor]` INI section; keep runner templates in `config.ini`.

Host clipboard input is collected in bounded chunks. Terminals supporting
bracketed paste send its contents literally, with one undo group and a final
repaint. Control bytes in that paste do not execute editor commands. Ordinary
Insert-mode input bursts are also batched; short bursts retain dot-repeat.
Bracketed pastes clear dot-repeat rather than recording thousands of keys.

## Files, limits and highlighting

Up to eight buffers are open at once, each bounded to 512 KiB and 65,536 lines.
Gap capacities share a 2 MiB budget; line indexes and history require additional
heap memory. Each buffer retains up to 64 undo groups and 1 MiB of edit payload;
older groups are pruned. Very long insertions can split at the payload bound.
Dot replay holds up to 4096 keystrokes; exceeding that clears repeat and reports
it. Allocation/limit failures are visible and leave the failing edit unapplied.
There is no swapfile or crash recovery yet.

Files are read/written in chunks. LF, CRLF, missing final newlines and original
octets are preserved; new line breaks follow the first encountered newline
convention. Input and display support UTF-8, including Cyrillic, CJK, combining
marks and emoji sequences. Cursor motions, character counts, deletion, visual
selection and registers operate on extended grapheme clusters. Vertical motion,
scrolling, tabs and the position indicator use terminal columns; clipping never
draws half a wide character. Dot replay retains complete Unicode scalars, and
`r`/`f`/`F`/`t`/`T` accept Unicode scalar targets without function-key collisions.

Storage offsets remain octets. Malformed UTF-8 displays replacement characters
and stays byte-for-byte intact unless edited; control bytes display safely and
NUL-containing files are rejected. Rift does not normalize text, implement bidi
layout or provide script shaping. Motions follow logical file order. Display
uses the shared Unicode engine and the terminal's available glyphs; its bounded
grapheme pool can show replacement glyphs for overlong/too many distinct complex
clusters without changing the file. Files beyond the size limit are rejected,
never silently truncated.

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

BASIC `.bas` files use case-insensitive keyword highlighting, apostrophe/`REM`
comments, and BASIC double-quoted strings. F5 invokes the guest `basic` interpreter;
`BASIC` overrides its executable and `[run] basic` overrides its runner template.
Try `rift /usr/share/basic/hello.bas`, or copy the Granny Bingo example to a
writable working directory before editing/saving. See the
[BASIC language and game guide](../../../ports/basic/README.md).

JavaScript `.js`, `.mjs` and `.cjs` files use a dedicated lexer for JavaScript
keywords, `$` identifiers, comments, quoted strings, multiline template strings,
regular expression literals and numeric literals. Slash disambiguation is
heuristic, and template interpolation is colored as part of the string. Vi
structural commands skip JavaScript comments and quoted/template strings; they
do not parse regexp bodies or nested template interpolation.

F5 saves the active file and runs the guest `js` executable from PATH. Your
`/bin/js` installation works directly; F8 shows whether it is available. Set
`JS=/mnt/host/js` to try a shared build, or `JS=/path/to/interpreter` to select
another runtime. `[run] javascript` and `RIFT_RUN_javascript` override the runner
command. Files ending `.server.mjs` or `.server.js` add `--server`, enabling
long-running server mode while retaining per-turn CPU and memory guards. F5
then Ctrl-C/Enter runs and stops a server without leaving the editor. Try
`/mnt/host/js-system/hello.server.mjs`. Custom runner templates must include
`--server` themselves when desired. `.mjs` enables ES modules in our QuickJS host; `.cjs` gets JavaScript
highlighting and a runner but does not add Node's CommonJS APIs.

Try `rift /usr/share/rift/hello.mjs`, or open the shared
`/mnt/host/js-system/fetch-json.mjs` and press F5 to run the HTTPS JSON/cache demo.
The latter needs the QuickJS host with `fuse:http`, `/bin/htget` and the installed
CA bundle. Syntax errors and runtime stacks appear in the suspended terminal;
Enter returns to the same editor tabs. Inline diagnostics, completions and LSP
remain future work; the lexer and runner use the existing separate services.

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
| `$HOME/.riftrc` | Saved editor options, layered last; updated by `:set` |
| `/etc/rift/theme.ini` | System theme selection/overrides; preserved on upgrade |
| `$HOME/.config/rift/theme.ini` | User theme selection/overrides |
| `/usr/share/rift/themes/*.ini` | Installed `turbo`, `graphite`, `mono` and custom named themes |
| `/usr/share/rift/{config,theme}.example.ini` | Configuration examples |
| `/usr/share/rift/hello.bf` | Runnable Brainfuck example; prints `A` |
| `/usr/share/rift/hello.mjs` | Runnable QuickJS ES module; lists the current directory |

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
wrap = true
textwidth = 0
formatoptions = t

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
`MAKE`, `M4`, `DC`, `CC`, `CXX`, `RFL`, `RUSTC`, `FPC`, `PYTHON`, `JS`, `LUA`.
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
| `lib/tui_file_dialog`, `lib/tui_files` | Shared Open dialog and Commander directory panel |
| `lib/tui`, `lib/tui_list`, `lib/tui_input` | Drawing contract, selection/scrolling and editable fields |
| `lib/tui_dialog`, `lib/path` | Reusable floating dialogs and lexical guest paths |

The [TUI companion libraries](../../lib/README.md) separate state, drawing and
application input policy; Commander and Rift use the same file-panel renderer.
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
cargo test --release --jobs 2 -p rfl --test rift --test string_slices --test string_literals --test companion_libraries --test tui_widgets
python3 rfl/tools/test_rift.py --os both
python3 rfl/tools/test_rift_unicode.py --os both
python3 ports/fuzix/tools/test_curses.py --native
python3 ports/fuzix/tools/test_curses.py --build build/fuzix
```

The host engine test uses randomized edits checked against a separate reference,
line-index invariants, grouped undo/redo, CRLF, nested objects/comments,
multiline-color invalidation and a 70 KiB boundary edit. The RFL test drives the
actual vi controller/engine. Guest tests exercise the real terminal, filesystem,
programs, tab ownership and restoration on both kernels using disposable disks.
They independently decode saved disk contents to verify CRLF and 70 KiB edits,
and cover file-picker navigation/scrolling/type-ahead, hidden files, failed
paths, cancellation/resize, existing tabs, failed saves, external-change
confirmation, per-user settings and a custom runner with an unavailable default
tool.
Logs are `/tmp/rift-fuse-x.log` and `/tmp/rift-fuzix.log`.

The Unicode checks drive both kernels through a real PTY, split input encodings
across reads, distinguish U+0102 from Down, check display-column cursor positions,
and verify saved UTF-8 directly from the stopped disk image. They cover combining
marks, CJK, emoji ZWJ sequences, undo/dot replay and Unicode Open/Save As paths.
The engine checks also move the physical gap through every encoding boundary,
exercise malformed octets, and test long Unicode words and counted searches.
Unicode logs are `/tmp/rift-unicode-{fuse-x,fuzix}.log`.
