# FUZIX on Ripple

This is a working initial port of the FUZIX kernel to RCC and RVM. It boots
from a FAT16 disk, runs separate RCC userspace executables, supports copy-on-write
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
`--rvm` selects the default native VM. Both use the same persistent disk. The launcher rebuilds and installs the
appropriate kernel/userspace profile when switching hosts. RVM defaults to the
MMU profile; RBF selects `MMU=0` and additionally exports `bios.bf` with
LOADC/STORC, IRQs and the instruction timer enabled. `--rbf --run-only` reuses
that export, and `--rbf --build-only` prepares it without starting the host.
The JIT remains enabled by default on either host; use `--no-jit` to disable it.

The BF tape reserves 80 data banks and 140 code banks for dynamically loaded
executables, requiring about **547 MiB for the tape alone**. Brainfuck execution
can be much slower than RVM, including BIOS disk loading. `--telemetry-lite`
shows RBF progress. Interactive launches stream raw terminal bytes to the guest,
including Ctrl-C, Tab and arrow keys; Ctrl-] stops the host and restores the
terminal. `--headless --input TEXT` runs without live terminal input. Use
`shutdown` to save cleanly before switching hosts.

## Protected memory

The default RVM build uses [MMU ABI 1](../../docs/arch/mmu.md), 256 physical RAM
banks (32,000 KiB), and 16 process slots. It allows 16 descriptors per process,
64 system-wide open files, and 64 cached inodes. A 16,000 KiB physical page pool
backs user memory. Each process has up to 16 data/heap banks (1,024,000 C characters,
roughly 2 MiB of physical memory) and a separate 64,000-cell stack. Static
program globals still occupy one bank and RXE images retain their existing
60,000-character initial-data limit. Actual allocations share the physical pool;
16 processes cannot each consume their entire virtual capacity simultaneously.

Process switches select maps instead of copying data and stack banks. `fork`
shares read-only code and COW data/stack pages; writes copy 512-cell pages as
needed. Fork also invalidates compiled code when replacing a child instruction
map, including reused process slots. The allocator supports blocks spanning banks. Arguments and environment
live at the top of the stack bank, leaving the heap region available for growth.
Invalid user accesses terminate the process with SIGSEGV. MMU/user privilege
violations cannot write kernel tables or issue device commands.

The physical layout is kernel GP 3, IRQ stack 4, process kernel stacks 32–47,
tables 64–79, page reference counts 80, and user physical pages 128–255. User
virtual code starts at bank 0, data/heap occupies 8–23, and stack uses 31.
Supervisor data accesses in virtual banks 8–31 use the active process map;
other kernel accesses and all device descriptors are physical. User-copy helpers
validate mapped pages and resolve COW before writing. Heap shrinking retains
only the partial final page; complete pages above the new break are released.
Failed growth rolls back newly allocated pages. There is no swap or demand
paging from disk.

Run `mmucheck` to exercise a large heap, COW, ten concurrent children and memory
protection. The normal `forkcheck` and `preemptcheck` exercises also apply.
For the old copy-based model, build with `make MMU=0`; the launcher chooses this
automatically for Brainfuck. That profile retains eight process slots, GP 8,
stack 9 and the one-bank allocator. The MMU requires a rebuilt RVM and rebuilt
userspace; existing disks keep user files while managed executables are updated.

## Unix userspace

The image now includes **101 utility, scripting and game command names**, built from the pinned
upstream sources and adapted where needed, plus the upstream V7 Bourne shell at `/bin/sh`.
They are real executables using FUZIX system calls:

| Purpose | Commands |
| --- | --- |
| Files and directories | `ls`, `cat`, `cp`, `mv`, `ln`, `rm`, `mkdir`, `rmdir`, `pwd`, `touch`, `chmod`, `chown`, `chgrp`, `mkfifo`, `mknod`, `basename`, `dirname`, `find`, `dd`, `tar`, `mktemp` |
| Text | `echo`, `head`, `tail`, `wc`, `grep`, `fgrep`, `sed`, `sort`, `uniq`, `cut`, `tr`, `tee`, `comm`, `rev`, `expand`, `colrm`, `od`, `sum`, `cmp`, `diff`, `diffh`, `join`, `split`, `fmt`, `column`, `pr`, `col`, `tsort`, `cksum` |
| Calculation and development | `expr`, `awk`, `make`, `m4`, `dc` |
| Terminal and paging | `more`, `less`, `clear`, `stty`, `tty` |
| Editing | `ed`, `vi` (Levee), `levee`, `vile` |
| Processes and environment | `ps`, `kill`, `sleep`, `env`, `printenv`, `id`, `whoami`, `uname`, `date`, `which`, `xargs`, `time`, `nice`, `nohup`, `groups`, `logname`, `free`, `uptime` |
| Filesystem information | `df`, `du`, `sync` |
| Manuals | `man` (57 installed manual pages) |
| Fun and scripting | `cowsay`, `picol`, `tcl`, `tclsh`, `wump`, `hamurabi`, `sokoban`, `sok` |
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
has 16 slots in the MMU profile and eight in the legacy profile; pipelines,
subshells, command substitutions and background tasks
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
`$()`/arithmetic expansion and modern `set` options are not provided. Interactive terminal job control is available through
`jobs`, `fg`, `bg`, and Ctrl-Z. Builtin I/O redirection retains upstream restrictions; use a
compound command/subshell when needed. Shebang interpreter selection is not
implemented; executable text uses the Bourne fallback. Utilities retain their
upstream option sets. ASCII text is the supported interchange; file lengths and
`od` addresses count native 16-bit C characters.

The libc layer provides syscall wrappers, buffered file I/O, integer/string
formatting, directories, environment lookup, UTC time conversion, and a reusable
process-local `brk` allocator. Upstream portable string, option, sorting, regular
expression, passwd/group, and mount-table routines are reused. It is not yet the
complete upstream libc; general floating formatted I/O outside AWK,
networking, hardware-specific applications, compilers, and the rest of the
application collection remain later porting work.

## Jobs, monitoring and terminal sessions

Interactive commands and pipelines have their own process groups. Ctrl-Z
suspends a foreground job; `jobs` lists jobs, `bg` continues one in the
background, and `fg` resumes it with terminal ownership. Both resume commands
accept a job number such as `fg %1`. Background terminal reads stop with
SIGTTIN. Shell terminal settings are restored at each prompt; resuming a
stopped editor restores its saved settings. Successful `stty` changes persist;
stopped or failed commands restore the shell settings. Pipeline exit status comes from
its final command. Noninteractive scripts retain the historical wait behavior.

Normal disk updates migrate the older fixed-console `/dev/tty` node to the
controlling-terminal alias. `/dev/tty1` remains the fixed console; user hard
links to the older device retain their original target.

To try this with Levee, run `vi /note`, press `i`, and type some text. Press
Escape to leave insertion mode, then Ctrl-Z (hold Control and press Z). At the
shell prompt, `jobs` shows the stopped editor; you can run another command,
then enter `fg` to return to the same buffer, including unsaved edits. Press
Escape, type `:wq`, and press Enter to save and quit. `:q!` quits without saving.
Use `fg %1` to select job 1 when several jobs exist. Editors need the terminal,
so resume them with `fg`; `bg` is for work that can continue in the background.

`ps -ef` reports parent, group and session IDs, process states and actual CPU
time. `ps -el` adds private/shared memory counts. `top` refreshes CPU usage and
memory interactively (`q` quits); `top -b -n 2` prints two batch snapshots.
CPU percentages use elapsed instruction-driven guest time. Per-process MAP-K
is PRIV-K plus SHRD-K, in physical KiB, covering data/heap/stack pages. Shared
pages appear in each referencing process, so these columns cannot be summed
to obtain physical usage; `free` reports unique pool totals instead. Executable
images and reserved kernel RAM are outside the per-process page counts.

`ptysh` opens a new terminal session running the shell. Exit it to return to
the outer shell; nesting uses distinct pairs. Four pairs are exposed as
`/dev/ptyp0`–`/dev/ptyp3` (exclusive masters) and
`/dev/ttyp0`–`/dev/ttyp3` (slaves, tty minors 2–5). Device nodes default to
0600. `/dev/tty` follows the caller's controlling terminal. PTYs support
canonical/raw input, echo, nonblocking backpressure, window-size ioctls,
foreground signals, EOF and master-close hangup. They provide the kernel
foundation for multiplexers and graphical terminals; `ptysh` itself is a
single-session bridge. These facilities work in both MMU and legacy profiles.

The read-only `/dev/proc` ioctl `RIPPLE_PROCINFO` provides versioned records
without kernel pointers, alongside `RIPPLE_SYSINFO`. Process groups and
sessions use the existing FUZIX syscall numbers 61 and 77–79. No MMIO commands
or RCC ABI extensions are required.

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

The legacy `MMU=0` profile reserves 80 data banks (10,000 KiB), funding eight
process slots with private data/stack snapshots. Its one-bank heap remains
limited to 64,000 characters. The default MMU profile supports larger heaps and
more processes as described above; each profile retains one spare executable
slot so `exec` can replace an image when every process slot is occupied.

## Memory reporting and Sokoban

`free` reports physical memory in KiB: total RAM, the reserved platform area,
the process page pool, physical pages in use, shared pages, and total process
mappings. A shared COW page consumes physical RAM once while appearing in several
process maps. The reserved area is not a count of live kernel allocations.
`ps -l` reports each process's mapped data/stack size; these sizes must not be
summed to estimate physical usage. `free` also shows occupied/total process slots
and the data/heap and stack capacities in native cells. Slots include zombies.

`uptime` reports elapsed guest time, process slots and kernel load averages.
The timer is instruction-driven, so these are guest seconds, not elapsed host
wall time. The port has no login accounting or swap.

```sh
free
uptime
sokoban
```

`sokoban` (also `sok`) runs the upstream tiny Sokoban game with 107 SokEvo
puzzles by Lee J Haywood. Push each `$` onto a `.`; `*` marks a box on a goal.
Use arrows, hjkl or WASD to move, `u` to undo, `r` to restart, `q` to quit,
and Ctrl-R to redraw. Quit/restart ask for confirmation. Ctrl-C and Ctrl-D
exit and restore terminal settings. A 32-column by 18-row ANSI terminal is
required. Completing a puzzle saves the next level in `$HOME/.sokoban`; normal
shutdown preserves progress across launches. `sokoban -l 1` starts at level 1
without changing saved progress. Custom native C16 level files are supported;
see `man sokoban` for their checked format. Credits and distribution terms are
installed in `/usr/lib/sok/README`.

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

## Bulk loading

Current RVM and RBF hosts accelerate BIOS kernel loading, filesystem block I/O,
executable code uploads, and physical RAM copies/clears through the existing
DEVICE_* mailbox. In the default MMU profile, this includes COW page copies,
page initialization, and cloning the parent's kernel stack during fork. Process
switches select maps without copying user banks. The legacy profile also uses
bulk transfers for executable data installation, fork bank snapshots, and
scheduling bank save/restore. Large physical kernel-buffer and filesystem
cache-buffer copies/clears use them in both profiles. Translated user buffers,
including executable data and `/dev/zero` destinations in the MMU profile, use
checked CPU accesses so virtual addresses are never submitted as physical device
addresses. The guest still parses FAT/RXE files and validates relocation records;
the legacy profile also applies physical code-base relocations. Code uploads
preserve Harvard memory semantics and JIT invalidation; disk transfers share the
legacy storage cache and commit barriers. Unsupported bulk operations fall back
to the original loops; the MMU profile still requires an MMU-capable RVM.
Rebuild `bios.bf` to enable bulk code uploads in RBF.

The [bulk I/O ABI](../../docs/arch/bulk-io.md) documents formats, limits and C helpers.
`BULK_IO=0` forces scalar transfers without changing the MMU profile. For a
comparison against the default `MMU=1` build:

```sh
make -C ports/fuzix BUILD=/tmp/fuzix-slow MMU=1 BULK_IO=0 -j4 kernel bios
python3 ports/fuzix/tools/test_bulk.py --fallback-build /tmp/fuzix-slow
python3 scripts/test-bulk-io.py
```

Use `MMU=0` for both builds when comparing the legacy profile. The C helper test
exercises RVM and RBF, with and without their JITs, using private temporary
disks. The Fuzix comparison uses identical userspace with fast
and fallback kernels of the same MMU profile under JIT by default; add
`--engine both` for interpreter coverage too. It checks
execution counts, repeated nested forks, native-word data/stack isolation, large
zero reads, preemption, signals, file-buffer copies and clean shutdown. In the
legacy profile, full-bank copies cross the 32,768-cell submission boundary;
the MMU profile copies shared user pages in 512-cell units. `forkcheck --stress`
runs the extended fork/isolation test directly; plain `forkcheck` retains its
quick parent/child check. Transfers of fewer than
256 cells use direct loops, and small trap/signal frames retain their assembly
paths. Toggling bulk I/O preserves each profile's memory layout and disk format.

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

In the legacy `MMU=0` profile, each process has a 64,000-cell global/data bank
and a separate 64,000-cell stack bank. Fork snapshots both banks into backing
RAM; scheduling restores them at the same execution addresses, preserving
pointers. Kernel continuations use a separate stack bank per process. Code
images are reference-counted across fork
and relocated into separate IMEM slots on exec. This profile supports eight
process table entries, nine live code images, and up to twelve IMEM banks per
userspace executable. Userspace allocation must use FUZIX brk/sbrk within its
data bank; RCC's generic multi-bank runtime malloc is not a process allocator.

The timer uses IRQ ABI v2 and a dedicated interrupt stack. All ordinary registers
and CPU return state are preserved. A timer return completes IRET before entering
the scheduler; a software IRQ supplies the trap frame when restoring a preempted
user continuation. No task sleeps with IRQ_BUSY held. Timer ticks are based on
executed VM instructions (100,000 per tick), not a host wall-clock RTC.

The launcher allocates 16,384,000 DMEM cells (banks 0–255) for the default
MMU profile, which provides privilege isolation and bank-spanning user heaps
as described in [Protected memory](#protected-memory). The legacy profile uses
5,120,000 cells (banks 0–79) and has no privilege isolation. Networking, the
remaining upstream libc/application collection, swap, and disk-backed demand
paging remain future work.

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
python3 ports/fuzix/tools/test_resources_games.py --build build/fuzix
python3 ports/fuzix/tools/test_jobs.py --build build/fuzix
```

Guest tests default to JIT throughout, including reboots. Each Python guest
suite accepts `--engine interpreter` or `--engine both` for explicit interpreter
coverage. `test_terminal.py --no-jit` remains an alias for interpreter testing.
For the complete suite, use `make -C ports/fuzix test BUILD="$PWD/build/fuzix"`;
add `TEST_ENGINE=both` or `TEST_ENGINE=interpreter` to choose other coverage.
Disks created by the tests are temporary.

The integration test boots real disks, verifies
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
after a reboot. Device-to-pipe inode recycling is checked before and after reboot
to prevent stale device numbers from becoming file blocks. Linker tests
verify that executable data reservations include the complete GP extent so the
heap cannot overlap globals.

For `MMU=1` builds, the integration test also runs `mmucheck` under the selected engines:
it checks a 200,000-cell heap, COW isolation, ten concurrent children, and
protection against kernel/MMIO access and privileged instructions.
`mmucheck --stress` additionally fills the process table and physical page pool,
checks ENOMEM rollback, kernel copy-out and failed exec, shrinks/regrows heaps,
verifies large malloc/realloc/calloc blocks, and repeats cleanup/reallocation.
The resources/game suite runs this stress workload on private disks.

The editor suite drives live terminal input, creates and
reopens files across a reboot, checks exact saved text, backups and permissions,
discards changes, assigns filenames, restores canonical input, exercises screen
dimensions, and checks the buffer boundary. RVM tests cover Escape/Alt decoding
so a quick Escape followed by `:` reaches the editor intact.
Terminal regressions also interrupt `ed`, a pipeline, and a computation with
Ctrl-C, then verify that the shell and process slots still
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

`man intro` provides a command overview. The image installs 57 plain manual
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
under the selected engines. They use isolated images and check filesystem consistency.

## Text processing and development tools

The image includes MWC `expr`, `make` and `m4`, V7 `dc`, and **One True Awk
20220122**. These run as guest executables, including their subprocesses and
file I/O. Each has an installed manual page (`man awk`, for example).

```sh
expr 12 + 3 '*' 4
awk -F: '{ total += $2 } END { print total }' records
awk 'BEGIN { for (i=1; i<=5; i++) print i, sqrt(i) }'
echo '12345678901234567890 2 * p' | dc
m4 macros.m4
make -n
make TARGET=Ripple
```

AWK supports associative arrays, functions, regex matching/substitution,
`getline`, redirected output, `system`, and command pipes. It uses binary64
software arithmetic and a dedicated decimal formatter, including `%f`, `%e`
and `%g` with precision up to 30. Integer formatting is 32-bit. The C locale
and input/regex characters 0–255 are supported; Unicode and hexadecimal floating
formatting are not supplied. Dynamic records and format buffers have a
16,000-character ceiling, with the total process heap imposing an additional
limit; a large record/program can exhaust that heap sooner.

`make` is the historical MWC implementation: makefile/Makefile lookup, includes,
macros, command-line assignments, timestamps, suffix rules, automatic variables
and `-f`, `-r`, `-n`, `-q`, `-s`, `-i` are available. Its `$<` expands the
prerequisite list. GNU make extensions such as `.PHONY`, pattern functions and
parallel `-j` builds are not supplied. Default rules/macros live in `/usr/lib`.
There is no guest C compiler yet, but make can run the existing shell tools.

`m4` provides definitions, arguments, quoting, includes, conditionals, integer
evaluation, diversions and shell commands. `dc` provides arbitrary-precision
stack arithmetic, scale, bases, macros and arrays; `q` exits. `expr` provides
checked 32-bit arithmetic and anchored basic regex matching/captures; quote
operators so the shell passes them through. Ctrl-C interrupts these programs
and returns to the shell.

Each executable has a twelve-bank IMEM reservation. The legacy profile has
eight process slots and the MMU profile has 16; both retain the RCC calling ABI.
Individual functions can span code banks through the shared linker's
[branch relaxation](../../docs/arch/code-banks.md). The RXE loader handles the
generated far-jump relocations in the legacy profile and virtual code mappings
in the MMU profile; the twelve-bank limit applies to the entire linked image,
including padding and expanded branches. Rebuilding `rlink` automatically
relinks the BIOS, kernel and userspace images.
RVM's dynamic instruction-memory limit accommodates these larger guest images.
Generated parsers are checked in; optional regeneration uses
`python3 ports/fuzix/tools/generate_devtools.py` with Bison 2.3. Parser state
is kept in memory and pointer tags normalized to work around an RCC sensitivity
to local state across backward parser jumps. Builds need no parser generator
or network access.

`python3 ports/fuzix/tools/test_devtools.py` checks all five tools under the
selected engines, including calculations, regexes, arrays/functions,
formatting, command pipes, files, dependency/include rules, errors, interrupts,
reboot persistence and filesystem consistency. It uses isolated disks.

`python3 ports/fuzix/tools/test_code_banks.py --build build/fuzix` compiles a
function containing 65,536 NOPs and loads it into a private guest disk. It checks
forward/backward branches, sequential bank crossings, function pointers, signal
handlers, nonlocal jumps, fork and repeated exec. It defaults to JIT and accepts
the same `--engine` choices as the other guest suites; use an `MMU=0` build to
exercise physical code-base relocation too.

## Upstream and licensing

The complete upstream source archive is in `vendor/`, pinned to the latest master
revision fetched on 2026-10-16. A portable working subset is in `upstream/`.
The separate pinned AWK archive and adapted sources are in `vendor/` and `awk/`;
`awk.patch` records its source adaptations. See [UPSTREAM.md](UPSTREAM.md) for provenance and
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
persistence. RVM unit tests check Tab and navigation-key
forwarding; disk tests check the shared `vi`/`levee` inode and preservation of
older user hard links during upgrades.

The fun-tool suite checks cowsay wrapping, designs, stdin and input limits;
Picol arithmetic, procedures, lists, channels, subprocesses, malformed input,
large scripts and error statuses; interactive multiline input and Ctrl-C; and
files across upgrades/reboots.

The everyday-tool suite checks joins with duplicate keys, splitting, formatted
tables, pagination, overstrikes, topological ordering, host-compatible checksums,
temporary permissions, process exit statuses, and invalid inputs. Live pager
tests cover forward/backward navigation, pipes, Ctrl-C,
unterminated lines and terminal restoration. Filesystem tests cover native
16-bit contents, storage-window crossings, double-indirect sparse writes and
truncation; `libccheck` also allocates and removes 320 files to exercise high
inode numbers. The shared FAT builder retains its default rOS layout.
