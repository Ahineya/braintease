# BASIC for Riplix and Fuse-X

`basic` is a terminal-first interpreter with QBasic-style syntax. Its portable C
engine builds through RCC for Ripple/Riplix and native Fuse-X. Granny Bingo is
its first application port. This is a growing system BASIC dialect, not a claim
of complete QBasic compatibility.

Normal OS builds install `/bin/basic`, `/usr/share/basic/hello.bas`, and
`/usr/share/basic/granny.bas`. Rebuild/install with the usual OS launcher. Save
games are created in the working directory; copy the game to a writable working
directory before editing it:

```sh
cp /usr/share/basic/granny.bas /tmp/granny.bas
rift /tmp/granny.bas
# F5 saves and runs; Enter returns to Rift after the program exits.
# Or run it directly:
cd /tmp
basic granny.bas
```

Choose a new game, buy supplies, visit the bingo hall, or save using menu option
8. During bingo, `Q` returns home. Enter a save name without the `.sav` suffix;
load with the same name. Files have a `GRANNY1` header and readable CSV records.
Use a persistent working directory for saves you want to retain. On Fuse-X,
`/mnt/host` is another convenient location. The port preserves the original
game's adult/crude humor.

Rift detects `.bas` case-insensitively, highlights BASIC keywords, strings and
comments, and invokes `basic` through the guest PATH. Set `BASIC` to another
interpreter executable or configure `[run] basic = {tool} {file}` in Rift's INI.
Errors use `file:line:column: message`. Rift currently displays the output and
returns to the editor; automatic error navigation remains future work.

## Language and execution

The loader tokenizes source and binds scoped labels and structured block
boundaries to instruction indices. Expressions execute from tokens, without
re-lexing source. A validation pass checks every statement before program I/O,
including code in branches that are not taken. `basic --check FILE` performs
this pass without running file operations, reading input, sleeping or printing
program output. This is syntax/type-shape checking, not complete static analysis.

- Names and keywords ignore ASCII case. `$` names hold strings; other names
  hold binary64 numbers. Implicit variables start at zero or the empty string.
- Strings use double quotes, doubled quotes for a literal quote, and literal
  backslashes. Source and the initial application port use ASCII. No DOS code
  page or UTF-8 conversion is performed.
- Operators: `+ - * / ^`, integer `\` and `MOD`, comparisons, bitwise `NOT AND
  OR XOR` on signed 32-bit operands. True is `-1`. `/` divides real numbers;
  `INT` floors and `FIX` truncates toward zero. Integer operators truncate
  their inputs. Boolean operators evaluate both operands.
- Assignment/`LET`, labels/`GOTO`, block `IF/ELSEIF/ELSE/END IF`, inline
  `IF ... THEN statement[:statement...]`, `SELECT CASE/CASE ELSE/END SELECT`,
  `FOR/TO/STEP/NEXT`, `WHILE/WEND`, `END` and `STOP`.
- `CASE` supports individual values, comma-separated choices and `low TO high`
  ranges. Inline `ELSE`, numeric line labels, arrays, user-defined functions,
  `GOSUB`, shared/static variables and typed declarations are not implemented.
- `SUB ... END SUB` and `CALL`: arguments that are bare variables are passed
  by reference. Expressions (including parenthesized variables) are copied.
  Locals are fresh on each call; `GOTO` cannot cross a subroutine boundary.
- Built-ins: `INT`, `FIX`, `ABS`, `SGN`, `LEN`, `LEFT$`, `RIGHT$`, `MID$`,
  `UCASE$`, `LCASE$`, `STR$`, `RND`, `TIMER`, `GRAPHICSAVAILABLE`.
  The PRNG is a local deterministic 32-bit generator, not DOS QBasic's sequence.
  `RANDOMIZE expression` sets its seed; `TIMER` is guest UTC seconds since midnight.
- `PRINT` with semicolons, comma zones and `TAB(n)` on stdout; `INPUT` supports
  prompts and CSV fields. Invalid numeric console input prompts for a retry.
  `CLS`, `COLOR foreground[, background]` and `LOCATE row[, column]` use ANSI
  sequences on a terminal. Redirected output omits display control sequences.
- `SLEEP n` waits whole seconds, up to one day. `SLEEP`/`SLEEP 0` waits for an
  Enter-terminated line. CR, LF and CRLF input are accepted, including a freshly
  booted console with CR line endings. Input EOF and I/O errors are reported; Ctrl-C uses the
  guest's normal foreground-process interrupt behavior.
- `OPEN path FOR INPUT|OUTPUT|APPEND AS #n`, `CLOSE [#n,...]`, `PRINT #n`,
  `WRITE #n`, and `INPUT #n`. `WRITE` quotes strings/doubles embedded quotes and
  writes binary64 numbers with 17 significant digits for text round trips.
  `INPUT #` requires the expected field count on one line. This first version
  has no DOS `RANDOM`/binary record I/O or BASIC error handlers.

Bounds: 512 KiB source, 50,000 tokens, 16,384 statements, 1,024 tokens per
statement, 63-character names, 4,096-character strings/input lines, 64 nested
blocks/expressions, 63 subroutines plus main, 32 parameters/input fields, 32
nested calls, 256 variables per scope, and 16 BASIC file numbers (subject to
libc and OS descriptor limits). Actual guest heap
capacity may impose a smaller bound. Generated `WRITE` records must fit the
input-line bound to be read back with `INPUT #`.

## Optional graphics

`--graphics=auto` (default) and `--graphics=off` run terminal programs.
`GRAPHICSAVAILABLE()` currently returns zero on both systems: a Nano-X backend
has not been implemented. `--graphics=on` reports that limitation and exits 2.
Unsupported drawing statements are explicit errors. The terminal port removes
the graphical credits and DOS screen changes; it never starts Nano-X.

## Source layout and validation

`main.c` owns startup/diagnostics, `parse.c` loads and binds program structure,
`value.c` implements values/expressions, and `run.c` executes instructions and
provides terminal/file operations. The runtime is process-local and runs one
program per invocation. The new interpreter code is MIT licensed.

```sh
python3 ports/basic/test.py --system host
python3 ports/basic/test.py                 # both real guests, disposable disks
python3 ports/basic/test.py --system fuzix  # only RVM guest integration
```

The integration checks run the same language probe on all backends, edit/run a
`.BAS` file through Rift F5, play a bingo turn, buy two supplies, save, shut down,
reboot, load and verify the state. An independent filesystem reader compares
the two saves and checks filesystem consistency. Normal guest disks are never
opened by this suite. Logs are saved in `build/basic/`.

`examples/granny.bas` is adapted from the supplied `GRNYBING.BAS`, credited to
Phillip Gervais. The original source and `QBASIC.EXE` in the shared folder are
untouched and are not build dependencies. The game retains its original
authorship; the interpreter's MIT license does not relicense that game.
The port replaces DOS art with ASCII, adopts text saves including player name
and fatigue state, fixes save-and-exit/case checks and multi-item shop charging,
validates purchase quantities, repairs the exhausted-player sleep path, and
adds voluntary departure from a bingo turn. Further changes to the original
game's rules are separate from interpreter compatibility.
