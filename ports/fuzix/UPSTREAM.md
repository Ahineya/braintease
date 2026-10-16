# Upstream source

- Project: [FUZIX](https://codeberg.org/EtchedPixels/FUZIX)
- Branch: master
- Revision: `0a2336cf8ce57cb94ad82f0c9f4cde2b4d578a78`
- Fetched and verified against the branch API: 2026-10-16
- Archive: `https://codeberg.org/EtchedPixels/FUZIX/archive/0a2336cf8ce57cb94ad82f0c9f4cde2b4d578a78.tar.gz`
- Archive SHA-256: `6dfaab209c84191972e69a8b258ccd667feee6a3f50d8298fe31e8a56ed03ee2`

The **complete, unmodified archive** is committed in `vendor/`. The portable
working tree in `upstream/` contains the kernel C sources, kernel/device headers,
minimal memory-allocation stub, selected application sources, portable library
routines/headers, README, credits, version, and licenses used by this port. This avoids losing upstream `.S`/`.s` pairs on macOS filesystems and
avoids treating upstream's tracked generated assembly as ignored local files.
To explore/build other upstream platforms, extract the full archive on a
case-sensitive filesystem. No network fetch is required to build this port.

Adaptations to files that exist in the archive are recorded in `upstream.patch`,
with paths relative to `upstream/`. Hunks omit context; use
`git apply --unidiff-zero` when replaying against the pristine archive. The kernel changes cover block/directory C16
layout, scalar user-memory validation, integer formatting, durable sync/error
handling, panic shutdown, the complete 32-bit `sbrk` return value, and clearing
recycled inodes before a pipe can reinterpret a deleted device number as a block.

`Library/libs/qsort.c` selects its specialized element paths using C `sizeof`
instead of physical-octet constants. Utility changes add bounded recursive
`mkdir`/`rm`, standard `cut` option parsing, error statuses, trailing-word counts,
`head -n`, missing-operand guards, C16 dump/usage calculations, and matching
format widths. Original copyright and license notices remain with imported
sources; newly rewritten adaptations are marked as such.

`Applications/util/vile.c` uses a direct ANSI terminal backend on Ripple, with
validated dimensions, alternate-screen cleanup, and bounded storage compatible
with its signed 16-bit movement offsets. Adaptations also bound file reads,
remember names supplied to `:w`, and mark backspace-only edits as modified.
Ctrl-C cancels raw-mode input, while `ed` supports the standard `Q` discard/quit
command. The upstream `tcgetattr`/`tcsetattr` wrappers are reused. `vile.doc` is retained
as upstream's historical document; current supported usage is in this port's
README.

Ripple ABI headers, syscall/stdio/format/allocator/directory support, the
PID 1 bootstrap, platform drivers, and build/image tools live in `ripple/` and `tools/`.
The executable list is defined by `APPS` in the port Makefile and exported to
`build/apps.json`. This remains a curated userspace port; the complete archive
also contains applications and libraries that are not built yet.

`Applications/V7/cmd/sh/` is the upstream Bourne shell, with its optional
`BUILD_FSH` input hook enabled. Adaptations preserve 32-bit tagged pointers in
rounding and allocation markers, explicitly strip the parser lookahead bit from
16-bit characters, keep `eval` pointers wide, and integrate PID 1 shutdown,
terminal recovery and adopted-child reaping. Caught signals validate separate
IMEM addresses rather than applying the DMEM validator to function pointers.
The parent V7 COPYRIGHT applies to these imported sources.

The file-tool batch adds MWC `find.y`, util `dd.c`/`tar.c`, and V7
`diff.c`/`diffh.c`. `find.c` is generated reproducibly from the adapted upstream
C and tokens plus `ripple/find-parser.inc`; the port's C99 parser implements the
same expression grammar without depending on a host yacc generator. Directory
traversal closes/reopens streams around recursion, and `-exec` uses argv. DD
adaptations fix counts/offsets and native-word formats. Diff adaptations add
working stdin spooling and final unterminated lines; the heuristic helper is
installed at `/bin/diffh`. Tar retains its upstream license and USTAR header
layout, with adapted native-character record I/O, bounds/checksum validation,
streaming, paths, metadata, and error handling. `mkstemp` and `stpcpy` are supplied
by the Ripple libc support.

The interactive shell hook uses `ripple/shell-input.c` and the imported upstream
readline interface, rather than the upstream readline implementation. It adds
bounded session history, ANSI cursor editing and completion from the live shell
`PATH`/builtin table and filesystem, without mixing libc malloc with the V7
shell's private brk arena. Buffered interactive reads return immediately after
consuming a completed line. RVM forwards ANSI navigation keys in text mode.

`Applications/levee/` is imported from the same pinned archive. Levee was
developed by David L Parsons; its redistribution notice remains in each source
and `levee.h`. The ANSI/Unix build is installed as `levee` and the default `vi`
(hard links on disk); VILE remains separately available. Adaptations provide
explicit C99 return types for legacy implicit-int functions, native-size undo
packing, bounded file/terminal input, checked record I/O and replacement saves,
16-bit character display, names retained after an unnamed buffer is saved,
terminal setup/restoration and window dimensions.
The port uses a 30,000-character core, with one character reserved by the
upstream read/insert algorithms. Unused platform backends remain unmodified.
The process table has eight slots; the code loader reserves one extra image slot
for replacement execs, and the launcher allocates all corresponding snapshot
banks. The compiler ABI and per-process bank layout are unchanged.

`Applications/games/cowsay.c` is the Fedux C implementation by Federico Pomar
and Federico Bond, with the existing FUZIX changes by Brett Gordon. The Ripple
adaptation retains the cow designs and attribution, fixes missing-argument
handling, and adds bounded stdin/argv text, real multiline wrapping, width
options and useful exit statuses.

`Applications/TCL/picol.c` is Salvatore Sanfilippo's BSD-licensed Picol from the
same archive, rather than a separate Tcl dependency. Its complete copyright,
redistribution conditions and disclaimer remain at the top of the source.
The port fixes parser termination/errors, escape decoding, whitespace/comments,
allocation and result ownership, procedure cleanup/redefinition and return
statuses. `ripple/picol-extra.inc` adds the CLI/REPL, checked 32-bit expressions,
lists, control flow, file channels, source/eval/catch and direct argv subprocess
execution. The interpreter deliberately remains a documented Tcl subset, built
with RCC's existing C16 ABI and no GNU C additions. `picol`, `tcl`, and `tclsh`
are hard links to one managed image. The original build/package files are
retained for provenance. `/hello.tcl` is a port-local example seeded from
`examples/hello.tcl`, preserving existing user edits on upgrades.

The everyday/text batch imports util `less`, `mktemp`, `groups`, `logname`,
`nohup`, `fmt`, `column`, `stty`, and V7 `clear`, `col`, `join`, `nice`, `pr`,
`split`, `time`, `tsort`, `tty`. Existing util `more` and `cksum` are enabled.
Adaptations cover raw pager input and cleanup, redirected and piped I/O,
look-ahead index seeks, bounded screen/input sizes, native format widths,
option parsing, temporary creation, complete records, I/O errors and exit
statuses. Original notices remain in each imported source. The small `less`
pager carries its upstream GPL3 notice. Portable `termcap`, `tgetent`,
`ttyname`, `sysconf`, and `cfspeed` library sources are imported; terminal entry
parsing is bounded, tty lookup uses the device number, and sysconf accounts
for native 16-bit characters. Port-local libc adds `_uname` and `mkdtemp`.

C16 V2 increases filesystem blocks from 256 to 1,024 native characters, while
keeping the existing RCC ABI and RXE1 executable format. `blk512.c` uses
`INO_PER_BLOCK` and the native number of indirect pointers rather than fixed
8-bit-platform constants. The Ripple disk driver splits transfers at host
storage-window and FAT-cluster boundaries. Image/check/install tools share a
single V2 geometry definition, and the shared FAT image builder accepts an
explicit cluster size while preserving its rOS defaults. Old C16 V1 disks
require a fresh image and are retained as backups by the launcher.
Inode allocation/free checks also use the configured density. Exec argument
buffers retain their earlier 256-character limit independently of larger disk
blocks; this preserves the single-bank process layout. `libccheck` creates and
removes 320 files to exercise inode numbers beyond the old 255-inode boundary.

The manuals/games batch imports util `man.c`, V7 `wump.c` and its manual,
`games/hamurabi.c`, `Library/libs/rand.c`, and 26 command manual sources from
util/V7. Port-specific pages in `manual/` override selected upstream pages;
`tools/manuals.mjs` installs the combined 43-page set under `/usr/man`.
`man` retains Robert de Bath's GPL formatter with bounded paths, headers,
indentation and line copies, read-error reporting, bounded `.so` redirects,
nonmutating MANPATH/MANSECT search, and direct fork/exec pager management with
checked waits and clean pipe closure. Installed manuals are uncompressed.
Wumpus checks EOF and numeric overflow, supports q, and returns to the shell.
Hamurabi retains Brian L. Troutwine's MIT notice and Alan Cox's FUZIX adaptation;
its resource counters are widened to 32 bits, numeric input/EOF/q are checked,
starvation is clamped and deducted, births follow the stated one-per-two rule
with the 6000 population limit, rats consume their losses, and feeding/planting
allocations reset each year. Its term is ten years. No compiler ABI changes
or additional host dependencies are needed.
