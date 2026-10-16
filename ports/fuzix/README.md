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

## Unix userspace

The image now includes **58 Unix utility commands**, built from the pinned
upstream sources and adapted where needed, plus the Ripple shell at `/bin/sh`.
They are real executables using FUZIX system calls:

| Purpose | Commands |
| --- | --- |
| Files and directories | `ls`, `cat`, `cp`, `mv`, `ln`, `rm`, `mkdir`, `rmdir`, `pwd`, `touch`, `chmod`, `chown`, `chgrp`, `mkfifo`, `mknod`, `basename`, `dirname` |
| Text | `echo`, `head`, `tail`, `wc`, `grep`, `fgrep`, `sed`, `sort`, `uniq`, `cut`, `tr`, `tee`, `comm`, `rev`, `expand`, `colrm`, `od`, `sum`, `cmp` |
| Editing | `ed`, `vi`, `vile` |
| Processes and environment | `ps`, `kill`, `sleep`, `env`, `printenv`, `id`, `whoami`, `uname`, `date`, `which`, `xargs` |
| Filesystem information | `df`, `du`, `sync` |
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

The shell supports single/double quotes, backslash escapes, `$NAME` and `$?`,
`;`, `<`, `>`, `>>`, `2>`, and up to three foreground external pipeline stages
(the kernel currently has four process slots, including the shell). `sh -c`
and `sh scriptfile` work. `cd`, `sync`, `help`, `shutdown`, and `exit` are built in;
other commands execute `/bin` programs. Enter/Ctrl-J submits a line, Ctrl-U
clears it, and Ctrl-D at an empty PID 1 prompt shuts down cleanly.

Ctrl-C interrupts foreground programs and pipelines, returning to the shell;
it does not stop RVM. The shell restores terminal settings after commands and
reaps adopted children. Ctrl-] is the emergency VM-stop key; use `shutdown` for
a clean stop.

This remains the port's small shell, not the full upstream Bourne shell.
Globbing, `&&`/`||`, background jobs, job control, here documents, and executable
shebang scripts are not implemented. Use `sh file` for scripts and `env NAME=value
command` for an altered environment. The utilities retain their upstream option
sets; `ed` provides a line editor. ASCII text is the supported text-processing
interchange; file lengths and `od` addresses count native 16-bit C characters.

The libc layer provides syscall wrappers, buffered file I/O, integer/string
formatting, directories, environment lookup, UTC time conversion, and a reusable
process-local `brk` allocator. Upstream portable string, option, sorting, regular
expression, passwd/group, and mount-table routines are reused. It is not yet the
complete upstream libc; floating formatted I/O, arbitrary signal handlers,
networking, hardware-specific applications, compilers, and the rest of the
application collection remain later porting work.

## Full-screen editing

`vi filename` and `vile filename` run the upstream **VILE** editor. VILE is the
smaller of the two bundled vi clones; Levee remains unported. Use `i` to insert
text, Escape to return to command mode, `h`/`j`/`k`/`l` to move, `x` to delete a
character, `dd` to delete a line, and `r` followed by a character to replace one.
`:w` saves, `:wq` saves and exits, and `:q!` discards unsaved edits. Ctrl-C also
cancels insert/command input. Starting with
`vi` alone works too: `:w filename` assigns a name for subsequent saves.
Existing files keep their permissions; the first overwrite keeps a `filename~`
backup. The editor uses the terminal's alternate screen and restores its input
settings when it exits.

The console uses ANSI cursor controls. The default size is 80 columns by 24 rows;
valid guest window dimensions and explicit environment overrides are supported:

```text
env LINES=30 COLUMNS=100 vi note
```

Host window resizing is not forwarded to the guest. VILE's buffer holds up to
30,000 native characters; larger files are rejected without changing them. This
is a small vi-style editor: arrow keys, undo, regex search, and yank/put are not
implemented by this port. Use `ed` for its existing search/substitution commands.
In `ed`, enter `q` to quit (confirm with `y` if prompted), or `Q` to discard
changes and quit immediately. Ctrl-C terminates `ed` and returns to the shell.

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
updates also save a backup before writing. No-op updates leave the
image and its modification time untouched. An exclusive companion lock covers
updates, VM execution, and offline checks.

Old bootstrap roots are expanded automatically, including their inode tables;
early data blocks are relocated when necessary and the result is checked before
writing. New/default upgraded disks are 64 MiB FAT16 images containing a
65,535-block native root (just under 32 MiB) with at least 1,024 inodes. An old
disk with additional outer FAT entries is refused during expansion to avoid
losing them. Inner guest files are preserved.

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

The disk format deliberately uses **256 C characters per block**, serialized
as 512 physical little-endian octets. Inodes remain 32 cells / 64 octets;
directory entries occupy 32 cells / 64 octets. The superblock uses the native
Ripple structure layout. Files count C characters, and text is stored as 16-bit
characters. RXE files contain one executable-format octet in each file character.
A distinct superblock magic (`0x7252`) and `RIPPLE-FUZIX-C16-V1` signature prevent
confusion with conventional eight-bit FUZIX filesystems. Use the supplied image
builder/checker rather than upstream host mkfs/fsck or ordinary FAT tools on the
inner filesystem. The outer FAT16 filesystem uses its ordinary octet format.

Each process has a 64,000-cell global/data bank and a separate 64,000-cell stack
bank. Fork snapshots both banks into backing RAM; scheduling restores them at
the same execution addresses, preserving pointers. Kernel continuations use a
separate stack bank per process. Code images are reference-counted across fork
and relocated into separate IMEM slots on exec. This currently supports four
process table entries, eight live code images, and up to four IMEM banks per
userspace executable. Userspace allocation must use FUZIX brk/sbrk within its
data bank; RCC's generic multi-bank runtime malloc is not a process allocator.

The timer uses IRQ ABI v2 and a dedicated interrupt stack. All ordinary registers
and CPU return state are preserved. A timer return completes IRET before entering
the scheduler; a software IRQ supplies the trap frame when restoring a preempted
user continuation. No task sleeps with IRQ_BUSY held. Timer ticks are based on
executed VM instructions (100,000 per tick), not a host wall-clock RTC.

The launcher allocates 4,608,000 DMEM cells, enough for banks 0–71, rather than
using RVM's very large general-purpose default. The port has no MMU or privilege
isolation. Networking, the remaining upstream libc/application collection, custom user
signal handlers, swap, dynamic multi-bank user heaps, and RBF execution remain
future work. Default signal handling is in the FUZIX kernel; arbitrary code-pointer
handlers are not supported by the current user-memory validator.

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
