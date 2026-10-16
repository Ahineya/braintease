# FUZIX on Ripple

This is a working initial port of the FUZIX kernel to RCC and RVM. It boots
from a FAT16 disk, runs separate RCC userspace executables, supports copy-based
`fork`, `exec`, `waitpid`, and timer preemption, and provides a writable root
filesystem that survives VM launches and kernel rebuilds.

Run from the repository root:

```sh
scripts/run-fuzix.sh
```

The launcher builds incrementally and enables the JIT by default. At the prompt:

```text
help
ls
cat README
echo hello > note
echo another session >> note
cat note
mkdir work
cp note work/copy
hello coffee
forkcheck
preemptcheck
shutdown
```

Start it again and `cat note`. Use **shutdown** to save and stop normally.
`sync` commits filesystem changes without stopping. `exit` shuts down PID 1;
inside a child `sh`, it returns to its caller.

Select the Brainfuck CPU with `scripts/run-fuzix.sh --rbf` (or positional `rbf`).
`--rvm` selects the default native VM. Both use the same persistent disk and
kernel/userspace binaries; the RBF build additionally exports `bios.bf` with
LOADC/STORC, IRQs and the instruction timer enabled. `--rbf --run-only` reuses
that export, and `--rbf --build-only` prepares it without starting the host.
The JIT remains enabled by default on either host; use `--no-jit` to disable it.

The BF tape reserves 80 data banks and 68 code banks for dynamically loaded
executables, requiring about **266 MiB for the tape alone**. Brainfuck execution
can be much slower than RVM, including BIOS disk loading. `--telemetry-lite`
shows RBF progress. Interactive launches stream raw terminal bytes to the guest,
including Ctrl-C, Tab and arrow keys; Ctrl-] stops the host and restores the
terminal. `--headless --input TEXT` runs without live terminal input. Use
`shutdown` to save cleanly before switching hosts.

## Unix userspace

The image now includes **90 utility, scripting and game command names**, built from the pinned
upstream sources and adapted where needed, plus the upstream V7 Bourne shell at `/bin/sh`.
They are real executables using FUZIX system calls:

| Purpose | Commands |
| --- | --- |
| Files and directories | `ls`, `cat`, `cp`, `mv`, `ln`, `rm`, `mkdir`, `rmdir`, `pwd`, `touch`, `chmod`, `chown`, `chgrp`, `mkfifo`, `mknod`, `basename`, `dirname`, `find`, `dd`, `tar`, `mktemp` |
| Text | `echo`, `head`, `tail`, `wc`, `grep`, `fgrep`, `sed`, `sort`, `uniq`, `cut`, `tr`, `tee`, `comm`, `rev`, `expand`, `colrm`, `od`, `sum`, `cmp`, `diff`, `diffh`, `join`, `split`, `fmt`, `column`, `pr`, `col`, `tsort`, `cksum` |
| Terminal and paging | `more`, `less`, `clear`, `stty`, `tty` |
| Editing | `ed`, `vi` (Levee), `levee`, `vile` |
| Processes and environment | `ps`, `kill`, `sleep`, `env`, `printenv`, `id`, `whoami`, `uname`, `date`, `which`, `xargs`, `time`, `nice`, `nohup`, `groups`, `logname` |
| Filesystem information | `df`, `du`, `sync` |
| Manuals | `man` (43 installed manual pages) |
| Fun and scripting | `cowsay`, `picol`, `tcl`, `tclsh`, `wump`, `hamurabi` |
| Shell helpers | `test`, `true`, `false`, `seq`, `yes` |

Try:

```text
echo coffee > note
cp note saved
cat saved | wc -w
seq 1 10 | tee numbers | wc -l
sort numbers | uniq -c
grep -n coffee note
sed 's/coffee/tea/' note
mkdir -p work/nested
cp -R work backup
rm -r backup
ps -ef
df /
sh -c 'echo child shell; exit 7'; echo $?
```

`/bin/sh` is the upstream V7 Bourne shell, compiled with RCC using the existing
Ripple ABI. It supports quotes and escapes, variables and positional parameters,
assignments/export, globbing (`*`, `?`, `[a-z]`), backtick command substitution,
`;`, `&&`/`||`, pipelines, redirection, here-documents, subshells, `if`/`elif`,
`for`, `while`, `until`, `case`, `read`, `eval`, `.`, traps and background/wait.
Both `sh -c` and `sh scriptfile` work. Executable text scripts fall back to the
shell after `execve` reports ENOEXEC. For example:

```sh
for file in /bin/s*; do echo "$file"; done
if test -f note; then cat note; else echo missing; fi
echo "Today: `date`"
```

`help`, `sync`, and `shutdown` remain small Ripple additions. PID 1 bootstraps
straight into the shell, so it consumes one process slot. The kernel currently
has eight slots; pipelines, subshells, command substitutions and background tasks
share that limit. Interactive prompts now provide Tab completion for shell builtins, executable
commands in `PATH`, and filenames/directories. Tab extends a common prefix;
press it again to list ambiguous matches. Spaces and shell metacharacters are
escaped, and quoted filenames work. Completion does not evaluate `$` variables,
backticks, or glob expressions. The input limit is 255 characters.

Up/Down (or Ctrl-P/Ctrl-N) recalls session history: up to 32 commands in a
4,096-character buffer. Left/Right, Home/End, Delete, Backspace, Ctrl-A/Ctrl-E,
Ctrl-B/Ctrl-F, Ctrl-K, and Ctrl-W edit the line. Ctrl-U clears it and Ctrl-C
cancels pending input. Enter/Ctrl-J submits a line, and Ctrl-D at
an empty PID 1 prompt shuts down cleanly. Ctrl-C interrupts foreground programs
and pipelines and returns to the prompt. Terminal settings are restored after
commands, and PID 1 reaps adopted children. Ctrl-] is the emergency VM-stop key;
use `shutdown` for a clean stop.

This is a historical Bourne shell, not a modern POSIX shell: shell functions,
`$()`/arithmetic expansion, modern `set` options, and terminal job control are
not provided. Builtin I/O redirection retains upstream restrictions; use a
compound command/subshell when needed. Shebang interpreter selection is not
implemented; executable text uses the Bourne fallback. Utilities retain their
upstream option sets. ASCII text is the supported interchange; file lengths and
`od` addresses count native 16-bit C characters.

The libc layer provides syscall wrappers, buffered file I/O, integer/string
formatting, directories, environment lookup, UTC time conversion, and a reusable
process-local `brk` allocator. Upstream portable string, option, sorting, regular
expression, passwd/group, and mount-table routines are reused. It is not yet the
complete upstream libc; floating formatted I/O,
networking, hardware-specific applications, compilers, and the rest of the
application collection remain later porting work.

## Full-screen editing

`vi filename` now runs **Levee**, developed by David L Parsons. It is also
available as `levee filename`. It supports undo (`u`), yank/put (`yy`, `p`, `P`),
regex searches (`/pattern`, `?pattern`, `n`), substitutions (`:%s/old/new/g`),
marks, repeat (`.`), counts, and the upstream ex command set. Use `i` to insert,
Escape to return to command mode, `h`/`j`/`k`/`l` to move, `:w` to save, `:wq`
to save and exit, and `:q!` to discard changes. Ctrl-C cancels insert/command
input. Starting with `vi` alone works too; `:w filename` names the buffer for
subsequent saves. Levee's buffer holds at most 29,999 native characters; overflowing reads
are marked read-only. Undo and larger yanks spill to temporary disk files.
Writes use a temporary file before replacing the destination and preserve
existing permissions. Replacement gives the saved file a new inode; other hard
links retain the earlier contents. `:set autocopy` enables `filename~` backups.

The simpler **VILE** remains available as `vile filename`. Its buffer holds
30,000 native characters; its supported commands and limits are unchanged.
Both editors use an alternate screen and restore terminal input settings on
exit. Native characters outside ASCII are displayed as `\u1234` escapes.
The console defaults to 80 columns by 24 rows. Guest window dimensions and
explicit environment overrides are supported, for example:

```text
env LINES=30 COLUMNS=100 vi note
```

Host window resizing is not forwarded to the guest. Use the traditional
`h`/`j`/`k`/`l` movement keys in the editors; the shell's cursor-key support does
not add arrow-key bindings to the upstream editors. In `ed`, enter `q` to quit
(confirm with `y` if prompted), or `Q` to discard changes and quit immediately.
Ctrl-C terminates `ed` and returns to the shell.

The launcher now allocates 5,120,000 native memory cells (10,000 KiB of data
memory), up from 4,608,000. This funds eight process slots, each with private
snapshots of its data and stack banks, and a spare executable slot allows `exec`
when all process slots are occupied. The current per-process layout remains
one 64,000-character data/heap bank and one 64,000-character stack bank. Larger
individual heaps need a separate allocator/loader change; raising VM memory
alone does not enlarge them.

## Cows and small Tcl scripts

`cowsay` is the bundled Fedux/FUZIX C version. It accepts a message or standard
input, wraps at 40 columns by default, and supports its eight cow designs:

```sh
cowsay Hello from Ripple
echo 'the cow has entered userspace' | cowsay -d
cowsay -W 20 'a longer message that wraps onto several lines'
```

`-W` accepts widths from 1 to 200. Input is bounded at 4,096 characters; oversized
messages fail with a nonzero status. `--help` lists the designs.

`picol`, `tcl`, and `tclsh` run the same **Picol Tcl subset**, derived from
Salvatore Sanfilippo's implementation bundled with FUZIX. Start `tcl` for an
interactive prompt, run `tcl filename arguments...`, pipe a script into it, or
use `tcl -c 'script'`. `exit` returns to the shell; Ctrl-C interrupts the program.
Interactive input supports multiple lines for braced procedure/loop bodies.

```sh
tcl -c 'puts [expr {2 + 3 * 4}]'
tcl /hello.tcl Ripple
cat /tcl-visits
```

The installed `/hello.tcl` example calls cowsay, calculates squares, and appends
a visit to `/tcl-visits`. Both files survive later image updates; editing the
example does not cause your copy to be overwritten.

Supported commands include `set`, `incr`, `append`, `puts`, `expr`, `if`/`else`,
`while`, `for`, single-variable `foreach`, `break`, `continue`, `proc`, `return`,
`list`, `llength`, `lindex`, `lappend`, `string length/equal/index`, `source`,
`eval`, `catch`, `error`, `exit`, and `exec`. Procedures have local variables,
fixed positional parameters or a final `args` parameter. Script arguments are
available as `argc`, `argv`, and `argv0`. The bundled Picol's prefix arithmetic
commands (`+`, `-`, `*`, `/`, comparisons) remain available, with `%` fixed/added.

Integer expressions support parentheses, variables, arithmetic, comparisons,
boolean operators and short-circuit evaluation. They use checked signed
32-bit integers; division rounds down, and overflow or division by zero raises
an error. Braced expressions do not perform nested command substitutions;
calculate those values first in a variable. `if` supports one body and an
optional `else`; `elseif`, default procedure parameters, arrays, namespaces,
packages, floating point, Tk and the complete Tcl standard library are absent.
This is a small interpreter rather than a full Tcl distribution.

`open`, `close`, `read`, `gets`, `eof` and `flush` support text files. Use
`puts $channel text` to write and modes `r`, `w`, `a` (or their `+` variants).
`exec` runs one program with explicit arguments and captures stdout; it does
not interpret shell metacharacters or pipeline syntax. External nonzero exit
statuses become catchable errors. File writes and subprocesses use ordinary
FUZIX system calls and the persistent filesystem.

Scripts/strings are bounded at 12,000 native characters, nesting at 32 evaluator
levels, lists/commands at 128 elements/arguments, and open file channels at eight.
The current process heap can impose a smaller limit when several large values
are live at once. Text cannot contain NUL characters; ASCII is the supported
interchange. Malformed input, oversized scripts and script errors return nonzero
status without stopping the OS.

## Builds and persistent disks

```sh
scripts/run-fuzix.sh --build-only
scripts/run-fuzix.sh --run-only
scripts/run-fuzix.sh --no-jit
scripts/run-fuzix.sh --headless --input $'cat note\nshutdown\n'
```

Artifacts live in `build/fuzix/` by default. Set `FUZIX_BUILD` to change that.
Building in `ports/fuzix/` directly uses `ports/fuzix/build/` instead:

```sh
make -C ports/fuzix -j4 images
```

The default disk contains `FUZIX.SYS` and a fixed-size `ROOTFS.IMG`. The latter is
an ordinary FAT16 file containing the guest filesystem. The driver caches and
validates its cluster chain and overwrites only its existing payload; guest
file creation/deletion does not require modifying the outer FAT allocation.
The rOS FAT16 reader and storage helpers are shared source dependencies.

A normal launch updates the kernel and managed executables on the default disk.
It preserves user files, inode identities, permissions, and timestamps; `/etc`
configuration is seeded only when missing. Hard links to an older executable
keep their old content when its managed pathname is replaced. Before an update,
the launcher checks the filesystem while holding the disk lock. Recoverable
shutdown/free-list metadata is repaired automatically after saving a backup;
ambiguous structural damage stops the update with a readable error. Executable
updates also save a backup before writing. `/bin/vi` and `/bin/levee`
share an inode to avoid storing the editor twice. The `cp`/`mv`/`ln`
executables and the three Picol names likewise share their respective binaries. No-op updates leave the
image and its modification time untouched. An exclusive companion lock covers
updates, VM execution, and offline checks.

Smaller roots of the current filesystem version are expanded automatically, including their inode tables;
early data blocks are relocated when necessary and the result is checked before
writing. New/default disks are 256 MiB FAT16 images containing a
65,535-block native root (just under 128 MiB) with at least 1,024 inodes. An old
disk with additional outer FAT entries is refused during expansion to avoid
losing them. Inner guest files are preserved within the current filesystem version.

The larger C16 V2 blocks replace the earlier C16 V1 layout. V1 images need
`scripts/run-fuzix.sh --fresh --build-only` to create a new disk; this saves the
previous disk as a numbered backup. Existing V1 disks are rejected rather than
silently converted. Executable RXE1 files and the C ABI are unchanged; no
executable compression or packing is involved.

`--disk IMAGE` boots an existing image without updating it. To install the new
userspace on an explicit disk, with the VM stopped:

```sh
scripts/run-fuzix.sh --disk path/to/image.img --install-apps --build-only
```

`--run-only` skips builds and updates. `--fresh` explicitly creates a fresh
filesystem and backs up the previous disk. `make clean` removes compiled
artifacts while retaining disks and backups.

## Storage durability and recovery

RVM COMMIT and COMMIT_ALL now wait for host `fsync`/`sync_all`; failed barriers
retain dirty data and set STORE_CTL's sticky ERROR bit. The block driver checks
this status. Kernel `sync` flushes both filesystem buffers and the VM cache,
including when all guest metadata buffers have already been written. Shutdown
flushes ordinary changes before writing and committing the clean superblock.
It reports a failed barrier instead of claiming successful shutdown.

This filesystem has no journal. A timeout, Ctrl-], SIGKILL, or host crash can
leave it dirty; unsynced data may be lost. FUZIX mounts dirty roots read-only.
Normal launches recover repairable metadata automatically. `--run-only` skips
that check/update step. With the VM stopped, manual checks are also available:

```sh
python3 ports/fuzix/tools/fsck.py build/fuzix/fuzix.img
python3 ports/fuzix/tools/fsck.py build/fuzix/fuzix.img --repair
```

The checker walks inode block trees and directories, checks ownership, link
counts, reachability, free lists, and the clean flag. Repair backs up the disk,
rebuilds free-space metadata, and marks it clean only after structural checks
pass. It refuses ambiguous damage such as cross-linked blocks or unreachable
inodes; it is not a substitute for backups or a complete salvage utility.
Always use the Python entry point, which takes the same disk lock as launch.

## Ripple ABI and filesystem

The RCC ABI is unchanged: 16-bit C characters and ints, 32-bit longs and banked
pointers, separate instruction memory, and the existing calling convention.
The compiler fixes include standard C `register` parameters, pointer typedef
member access, physical line splicing, and releasing consumed call-argument
registers before allocating return values. The linker now includes the full GP
reservation in executable data sizes. No GNU C extensions were added.

The disk format deliberately uses **1,024 C characters per block**, serialized
as 2,048 physical little-endian octets. Inodes remain 32 cells / 64 octets;
directory entries occupy 32 cells / 64 octets. The superblock uses the native
Ripple structure layout. Files count C characters, and text is stored as 16-bit
characters. RXE files contain one executable-format octet in each file character.
A distinct superblock magic (`0x7253`) and `RIPPLE-FUZIX-C16-V2` signature prevent
confusion with conventional eight-bit FUZIX filesystems. Use the supplied image
builder/checker rather than upstream host mkfs/fsck or ordinary FAT tools on the
inner filesystem. The outer FAT16 filesystem uses its ordinary octet format.

Each process has a 64,000-cell global/data bank and a separate 64,000-cell stack
bank. Fork snapshots both banks into backing RAM; scheduling restores them at
the same execution addresses, preserving pointers. Kernel continuations use a
separate stack bank per process. Code images are reference-counted across fork
and relocated into separate IMEM slots on exec. This currently supports eight
process table entries, nine live code images, and up to four IMEM banks per
userspace executable. Userspace allocation must use FUZIX brk/sbrk within its
data bank; RCC's generic multi-bank runtime malloc is not a process allocator.

The timer uses IRQ ABI v2 and a dedicated interrupt stack. All ordinary registers
and CPU return state are preserved. A timer return completes IRET before entering
the scheduler; a software IRQ supplies the trap frame when restoring a preempted
user continuation. No task sleeps with IRQ_BUSY held. Timer ticks are based on
executed VM instructions (100,000 per tick), not a host wall-clock RTC.

The launcher allocates 4,608,000 DMEM cells, enough for banks 0–71, rather than
using RVM's very large general-purpose default. The port has no MMU or privilege
isolation. Networking, the remaining upstream libc/application collection, swap,
dynamic multi-bank user heaps, and RBF execution remain future work.

Caught signals use validated IMEM code pointers and a user-stack register frame.
The kernel enters a userspace trampoline after a syscall or timer preemption;
return restores the interrupted state, while `longjmp` can abandon the frame.
No suspended kernel call remains beneath a handler. `signal()` has the FUZIX
one-shot semantics: a handler re-registers itself when needed. `setjmp`/`longjmp`
preserve RCC callee state including stack/global banks.

## Validation

With the release compiler, assembler, and VM built:

```sh
make -C ports/fuzix test
# Or for the launcher's output directory:
python3 ports/fuzix/tools/test.py --build build/fuzix
node --test ports/fuzix/tools/disk.test.mjs
python3 ports/fuzix/tools/test_terminal.py --build build/fuzix
python3 ports/fuzix/tools/test_unix.py --build build/fuzix
python3 ports/fuzix/tools/test_editor.py --build build/fuzix
python3 ports/fuzix/tools/test_interactive.py --build build/fuzix
python3 ports/fuzix/tools/test_shell.py --build build/fuzix
python3 ports/fuzix/tools/test_filetools.py --build build/fuzix
python3 ports/fuzix/tools/test_fun.py --build build/fuzix
```

The integration test boots real disks with both JIT and interpreter, verifies
forked mutable data and child exit status, preempts a computing process, checks
arguments to a separate executable, writes and reopens a 77,777-character file
through direct/single/double-indirect blocks, seeks/truncates/deletes it, checks
filesystem persistence and cleanup, repeatedly reuses executable slots, and
exercises offline dirty-filesystem recovery. Host tests cover image structure,
root preservation, backups, no-op updates, and malformed disk mappings. A live
pseudo-terminal test exercises ordered input, control keys, the run-only
launcher, and exclusive disk locking. The Unix suite checks upstream command
output and status, three-stage pipelines, redirection, `sh -c`, `ed`, heap/fork
isolation, sleep, recursive copies, FIFO/device nodes, upgrades, and saved files
after an interpreter reboot. Device-to-pipe inode recycling is checked with both
engines to prevent stale device numbers from becoming file blocks. Linker tests
verify that executable data reservations include the complete GP extent so the
heap cannot overlap globals.

The editor suite drives live terminal input with both engines, creates and
reopens files across a reboot, checks exact saved text, backups and permissions,
discards changes, assigns filenames, restores canonical input, exercises screen
dimensions, and checks the buffer boundary. RVM tests cover Escape/Alt decoding
so a quick Escape followed by `:` reaches the editor intact.
Terminal regressions also interrupt `ed`, a pipeline, and a computation with
Ctrl-C under both engines, then verify that the shell and process slots still
work and the disk shuts down cleanly. Host tests exercise automatic recovery,
backup preservation, unchanged clean images, and refusal of structural damage.

The shell suite checks control flow, positional parameters, eval/source,
globbing, command substitution, here-documents, executable text fallback,
background/wait, traps, syntax recovery and reboot persistence. Libc checks
cover non-local jumps, jumps out of handlers, synchronous signal return and an
alarm delivered to a computation that makes no syscalls.

The file-tools suite checks expressions and direct `find -exec`, deep paths,
`dd` count/seek/large-file behavior, normal diff output against the host tool,
stdin and final-line handling, USTAR exchange with the host, native character
round trips, pipes, blocking, metadata, malformed archives and reboot persistence.

## File search, comparison and archives

`find` evaluates the upstream MWC expression grammar: implicit/explicit `-a`,
`-o`, `!`, parentheses, `-name`, `-type`, `-perm`, `-size`, `-links`, `-inum`,
`-user`, `-group`, date predicates, `-newer`, `-print`, `-exec`, and `-ok`.
Traversal closes directories before descending, avoiding depth-related descriptor
or process exhaustion. `-exec` passes an argument vector directly, preserving
spaces and metacharacters in filenames. The small C99 parser is built from the
upstream C portions and token names by `tools/generate_find.py`; building requires
no yacc/bison installation. Paths are bounded to 599 characters. `-size` counts
512 native-character blocks, or individual characters with the `c` suffix.

`dd` supports `if=`, `of=`, `bs=`, `count=`, `skip=`, and `seek=`, with native
character counts and checked 32-bit offsets. Block sizes are 1..32767 characters.
`count=0` creates an empty output without reading input; `seek=` preserves the
existing prefix and truncates at the final output position. Record statistics go
to stderr so stdout remains usable in pipelines. Numeric suffixes are `k` (1024),
`b` (512), and `w` (one native int). Other dd conversions are not implemented.

`diff` is the V7 line comparison tool: normal change records, `-b`, `-e`, `-f`,
and `-h` are available. It compares stdin through `-`, handles unterminated final
lines, and returns 0 for equal files, 1 for differences, and 2 for errors.
`diffh` supplies the bounded-memory heuristic comparison used by `diff -h`.
Unified/context output and recursive directory comparison are not provided.

`tar` creates, lists and extracts USTAR archives, including recursive directories,
file metadata, hard-link extraction, FIFO/device entries, long USTAR names and
archive streams. It accepts `tar -cf archive path`, `tar cf archive path`, or
`-f -` for stdin/stdout. Blocking factors are 1..32; `-k` keeps existing files.
Extraction checks header checksums and rejects parent traversal/absolute names.
Symbolic links, compressed archives and GNU/PAX extensions are not supported.

Tar records contain **512 native C characters**. ASCII/octet-valued streams have
standard USTAR layout when exported as one octet per cell. Native character
values above 255 survive guest archive round trips; such content requires a
C16-aware transport rather than conversion to ordinary host octets. The ABI and
filesystem representation remain unchanged.

```sh
find / -name '*.c' -type f
find work -type f -exec wc -c {} \;
dd if=note of=prefix bs=1 count=5
diff note saved; echo $?
tar -cf backup.tar work
tar -tf backup.tar
mkdir restored; cd restored
tar -xf /backup.tar
```

## Pagers and everyday text tools

`less filename` runs FUZIX's small pager. Space/`f` advances a page, `b` goes
back, `d`/`u` moves half a page, `j`/Enter and `k` move one line, `g` returns
to the first indexed line, and `q` quits. Ctrl-C exits and restores the terminal.
The pager uses an alternate screen, handles final lines without a newline,
and spools piped input into an unlinked temporary file for backward navigation.
Its line index is bounded by the process heap; under memory pressure it drops
older positions. Lines are read in chunks of at most 511 characters. Search
commands are not implemented by this upstream pager.

`more filename` streams pages; Space advances, Enter displays one more line,
`n` moves to the next file, and `q` quits. Both pagers accept pipes and become
plain copy streams when stdout is redirected. Dimensions come from the guest
window, with valid `LINES`/`COLUMNS` overrides and an 80-column, 24-row fallback.
`/etc/termcap` is seeded with ANSI terminal definitions and preserves user edits.
`clear`, `tty` and `stty` use the guest terminal interfaces. `stty` rejects invalid
options without applying a partially changed configuration; the kernel supports
only the terminal flags implemented by its console driver.

```sh
seq 1 100 | less
more README
column -t -s: /etc/passwd
fmt -w 60 README
join names scores
split -l 20 README part
pr -t README
stty -a
time sh -c 'seq 1 100 | wc -l'
nice -n 5 echo hello
```

`join` expects sorted inputs. It supports duplicate keys, unmatched records
(`-a1`, `-a2`, `-a`), custom separators (`-t:`), field selection (`-1 N`, `-2 N`,
`-j N`, `-j1 N`, `-j2 N`) and output fields (`-o 1.2 2.2`). Records are limited
to 1,022 characters and 19 fields. Its second input must be seekable.
`split` accepts `-l N` and historical `-N`, preserves unterminated final lines,
checks output errors and prevents overwriting its own input. Suffixes run from
`aa` through `zz` (676 files).

`column` supports table mode (`-t`), separators (`-s`), row ordering (`-x`),
widths up to 1,000 (`-c`), and table column limits (`-l`). Input records are at
most 255 characters. `fmt` retains the upstream paragraph, centering and mail
options, with numeric formatting limits of 4,096. `pr` retains historical
pagination/column options; `-t` suppresses headers and final page padding.
`col -b` removes overstrikes and `-x` expands tabs; its bounded line storage
supports at most 780 columns/overstrike characters. `tsort` accepts tokens up
to 50 characters, reports cycles and incomplete pairs, and returns a failure
status for them.

`cksum` computes the standard CRC for octet-valued files, matching host `cksum`
on ASCII input. It rejects native characters above 255 rather than discarding
their high bits. `mktemp` creates files atomically with mode 0600, or directories
with mode 0700 using `-d`. It also supports `-p`, `-t`, `-q` and `-u`. Generated
names use the whole trailing run of at least six `X` characters.

`time`, `nice`, and `nohup` run separate programs and preserve useful exit
statuses. `nohup` ignores SIGHUP and appends terminal output to `nohup.out`.
`groups` and `logname` use the current FUZIX identity; the default session is
root, and the Level 1 kernel supplies its primary group. The clock used by
`time` follows the guest's instruction-driven timer.

## Manuals and classic games

`man intro` provides a command overview. The image installs 43 plain manual
pages in `/usr/man`, including the Bourne shell, editors, Tcl, common text tools
and the games. These combine retained upstream manuals with port-specific pages
for changed behavior. Not every command has its own manual yet. Use, for example:

```sh
man sh
man vi
man tcl
man less
man 6 wump
man hamurabi
man -w cp
man cp | col -b > cp-help
```

Interactive manuals use `more` (Space/Enter to advance, `q` to quit); set `PAGER`
to choose another pager, such as `env PAGER=less man sh`. Redirected manuals are
formatted directly. `-l` reads a local file, `-w` prints the first matching path,
and `MANPATH`/`MANSECT` select the manual trees/section order. The small built-in
formatter supports man macros and preformatted cat pages, with bounded paths,
line/header storage and `.so` redirects. It does not support compressed manuals,
full groff/mdoc, or source characters above 255.

`wump` is the V7 **Hunt the Wumpus** cave game. Ask for instructions at startup,
then move through connected rooms or shoot arrows while avoiding pits and bats.
Enter `q` or press Ctrl-C to return to the shell.

`hamurabi` is a **Sumerian city management game**: buy/sell land, allocate grain
and plant crops for a ten-year term. Enter `q`, a negative number, or Ctrl-C to
leave. Numeric input and EOF are checked. Resource accounting uses 32-bit
values; yearly feeding/planting resets, starvation cannot wrap, and harvest
losses are deducted. These are text games without saved-game support.

The guest tests exercise manual search, formatting, aliases, custom paths,
missing pages, include loops, oversized headers, live pager exit/Ctrl-C,
actual game prompts/actions, invalid/large input, EOF, and terminal recovery
in both RVM engines. They use isolated images and check filesystem consistency.

## Next userspace batches

The next useful batch is remaining text tools such as `expr` and `awk`,
development tools such as `make`, `m4` and `dc`, and broader manual coverage,
followed by system administration tools. More game candidates in the archive
include Sokoban, Star Trek and adventure interpreters. Larger applications, languages/build tools and
networking depend on further libc, process-capacity and device work; the complete
upstream archive is retained.

## Upstream and licensing

The complete upstream source archive is in `vendor/`, pinned to the latest master
revision fetched on 2026-10-16. A portable working subset is in `upstream/`. See [UPSTREAM.md](UPSTREAM.md) for provenance and
[upstream.patch](upstream.patch) for the core adaptations. Ripple platform
sources, build tools, and bootstrap userspace live outside that snapshot's
platform hierarchy so they can be reviewed together. Upstream sources retain their individual license notices, including GPL/LGPL
variants. Port-authored kernel platform code is GPL-2.0-only; port-authored
userspace support is GPL-2.0-or-later so it can accompany utilities using later
GPL versions. See [upstream/LICENCE](upstream/LICENCE),
[upstream/Library/COPYING](upstream/Library/COPYING), and source-file notices.

The interactive suite covers command/path completion (including quoting),
ambiguous listings, history eviction, wrapped-line editing, prompt cancellation,
six-stage pipelines, and Levee search/substitution, yank/put, native-character
and disk-backed undo, saved permissions, overflow handling and reboot
persistence under both engines. RVM unit tests check Tab and navigation-key
forwarding; disk tests check the shared `vi`/`levee` inode and preservation of
older user hard links during upgrades.

The fun-tool suite checks cowsay wrapping, designs, stdin and input limits;
Picol arithmetic, procedures, lists, channels, subprocesses, malformed input,
large scripts and error statuses; interactive multiline input and Ctrl-C; and
files across upgrades/reboots under both engines.

The everyday-tool suite checks joins with duplicate keys, splitting, formatted
tables, pagination, overstrikes, topological ordering, host-compatible checksums,
temporary permissions, process exit statuses, and invalid inputs under both RVM
engines. Live pager tests cover forward/backward navigation, pipes, Ctrl-C,
unterminated lines and terminal restoration. Filesystem tests cover native
16-bit contents, storage-window crossings, double-indirect sparse writes and
truncation; `libccheck` also allocates and removes 320 files to exercise high
inode numbers. The shared FAT builder retains its default rOS layout.
