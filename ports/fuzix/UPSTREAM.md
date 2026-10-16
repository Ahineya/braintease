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
The legacy `MMU=0` process table has eight slots with RAM snapshots. The default
RVM `MMU=1` profile has 16 slots, selected page maps, and COW user memory. Both
reserve one extra code-image slot for replacement execs. The compiler ABI is
unchanged; kernel and userspace must be built for the same MMU profile.

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
blocks and the MMU profile's larger heap. `libccheck` creates and
removes 320 files to exercise inode numbers beyond the old 255-inode boundary.

The manuals/games batch imports util `man.c`, V7 `wump.c` and its manual,
`games/hamurabi.c`, `Library/libs/rand.c`, and 26 command manual sources from
util/V7. Port-specific pages in `manual/` override selected upstream pages;
`tools/manuals.mjs` installs the combined 52-page set under `/usr/man`.
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

The development-tools batch imports MWC `expr.y`, `make.c`/`make.h`, default
`makeactions`/`makemacros`, `m4.c`, and V7 `dc.c`/`dc.1` from that same archive.
Expr adds checked 32-bit arithmetic, bounded regex compilation and capture
handling. Make adapts error reporting and include/parser ownership, fixes path
lookup and dependency-cycle handling, and installs its default files under
`/usr/lib`. M4 fixes diversion streams, temporary names, EOF/error handling,
hash arithmetic and recursion bounds. DC retains its arbitrary-precision
engine, with native-width pointer fixes, correct input-stack termination and
Ctrl-C exit. Original source notices remain in place.

## One True Awk

- Project: [One True Awk](https://github.com/onetrueawk/awk)
- Release: `20220122` (the pre-Unicode implementation)
- Revision: `b9c01f51224fd302519e8a35bd06effc06f6d3d1`
- Archive: `https://codeload.github.com/onetrueawk/awk/tar.gz/b9c01f51224fd302519e8a35bd06effc06f6d3d1`
- Archive SHA-256: `8f6af020326b4c81c5ae7afa36ae99cf0ebbfdced3e4a4ead9b41fc1cca76a3d`
- License: Lucent permissive notice, retained in `awk/LICENSE` and source headers.

The complete unmodified archive is retained in `vendor/`. Selected root source,
manual and license files live in `awk/`; `awk.patch` records adaptations to those
originals, replayed with `git apply --unidiff-zero`. The generated
`awkgram.tab.c`/`.h` and `proctab.c` are port build inputs rather than original
archive files. `tools/generate_devtools.py` regenerates both expr/AWK parsers
with Bison 2.3 and emits the AWK dispatch table without a host C compiler.
`--check` checks the dispatch table alone. Ordinary builds use the checked-in
files, requiring neither Bison nor network access.

Both generated, non-reentrant parsers normalize RCC tagged pointers through
`ripple_absolute_pointer` and keep dispatch state in memory. Their ordinary
local-state loops were sensitive to register pressure across backward gotos
with RCC; enabling parser tracing changed the failure. These port-local
adaptations work with tracing disabled and preserve the compiler ABI.

AWK adaptations remove GNU attributes and locale/wide-character dependencies,
use a bounded octet regex alphabet over native C16 storage, validate input
characters, bound buffers, reduce initial record/cache allocations, and retain
32-bit integer printf values. Files, pipes and `system` use real guest
processes. Port-local libc supplies `popen`/`pclose` and `raise`. AWK alone links
binary64 software arithmetic, selected FUZIX math sources, decimal conversion
and `ripple/float-format.inc`; ordinary utilities keep their smaller integer
formatter. Math adaptations cover decimal constants, explicit 32-bit masks,
C-locale number scanning, and 16-bit-safe exponent parsing. The decimal output
formatter expands the exact binary rational, rounding to nearest with ties to
even, with a precision cap of 30.

The executable loader reserves twelve code banks per live image: nine images
in the legacy profile and seventeen in the default MMU profile. It decodes
aligned RXE instruction records from the cached disk block. MMU userspace code
starts at virtual bank zero; legacy code is relocated to its physical slot.
The compiler ABI is unchanged. Static globals and the stack each retain a
one-bank limit, while MMU user heaps can span sixteen data/heap banks. RVM
dynamically grows instruction memory up to 256 banks of 65,536 instructions; its bounds test
covers the highest Ripple userspace reservation. RBF exports reserve 140 code
banks and 80 data banks, requiring about 547 MiB for the tape alone.

Bulk-memory integration redirects the Ripple kernel's `blktok`, `blkfromk` and
`blkzero` cache helpers through port-local buffer functions when `BULK_IO=1`.
The ordinary helper macros remain for other platforms and `BULK_IO=0`.
`ripple/usermem.c` supplies bounded RAM copy/fill helpers and the common bank-copy
entry used for legacy fork/scheduling snapshots and kernel-stack cloning in both
profiles; `lowlevel.asm` retains the original whole-bank fallback. MMU physical
page initialization and COW copies also use these helpers. Copies preserve
native 16-bit characters, and the mailbox is protected by saving/disabling/restoring
IRQ enables around each submission. Physical buffers use bulk operations from
256 cells upward. Translated user buffers use CPU accesses with mapped-page
validation and COW resolution. Unsupported operations use scalar fallbacks;
other host errors are propagated. The compiler/runtime's general memcpy and
memset implementations are unchanged. Extended `forkcheck --stress` and
`libccheck` cover fork isolation, high character bits, transfer boundaries,
stack/heap file I/O, and zero fills.

The protected profile's port-local `ripple/mmu.c` implements map selection,
physical-page reference counts, COW faults, user-memory validation, and heap
growth across banks. Its userspace entry uses SYSCALL and its kernel enters
user mode with ENTERU. `ripple/libc/heap.c` supports allocations spanning banks
without changing RCC's pointer representation or calling convention. `mmucheck`
covers the larger heap, COW, ten live children, and privilege protection.


The resources/games batch imports util `free.c`/`uptime.c`, library
`clock_gettime.c`/`clock_getres.c`/`getloadavg.c`, and games `sok.c`, `sok.h`,
`sokmap.c` and `ljh.sok` from the pinned archive. The clock helpers propagate
syscall errors and load conversion uses wide arithmetic. The tools report
native physical KiB, process capacity, sharing, monotonic guest uptime and
load averages; no login-session count is fabricated. A read-only, versioned
`RIPPLE_SYSINFO` ioctl on `/dev/proc` exposes counts without kernel pointers.

Sokoban retains the upstream movement/undo algorithm, with fixes for boxes on
goals, undo history/counting, bounds, level record sizes and seek arithmetic.
Its Ripple terminal backend supplies ANSI drawing, arrows/hjkl/WASD, bounded
escape reads, alternate-screen cleanup, Ctrl-C/EOF handling, and checked numeric
arguments. Progress is decimal text in `$HOME/.sokoban`; explicit levels/custom
collections do not change it. The checked host converter in `tools/sokoban.mjs`
replaces the original host `sokmap` build and encodes native C16 records.
The supplied 107 puzzles retain Lee J Haywood's designs, attribution and
redistribution terms; those terms are installed alongside the levels.

MMU robustness changes release complete heap pages on shrink, track new pages
for failed-growth rollback, preserve ENOMEM on failed COW copy-out, and preflight
exec's initial page requirements before replacing the old map. The protected
profile allows 16 descriptors per process, 64 system-wide open files, and 64
cached inodes so the larger process table can support simultaneous I/O. Fork
flushes compiled code when replacing a child instruction map, preventing stale
JIT code after process-slot reuse. Process memory is cleared from the table on
exit. `mmucheck --stress` exercises page and process exhaustion, failed exec/copy-out, zeroed regrowth, isolation and repeated recovery.

The terminal/job-control batch enables a focused CONFIG_JOB_CONTROL subset of
level-2 facilities: sessions, process groups, foreground tty ownership and stop
signals. Ripple's implementation in ripple/jobcontrol.c draws on the upstream
level2.c and syscall_level2.c algorithms, with checked user pointers, stricter
session/group changes and parent/exec ownership checks. The dormant upstream
PTY block is not enabled; ripple/pty.c implements four checked BSD-style pairs.
Kernel adaptations notify parents on stops, align wait status with libc, restart
signal-set scans correctly after continuation, allow SIGKILL to wake stopped
tasks, and retain surviving pipeline groups when a group leader exits.

The Bourne shell integrates ripple/shell-jobs.c for jobs/fg/bg, group-aware waits,
a fork/foreground handshake, terminal preservation and job cleanup. Levee gains
raw Ctrl-Z handling, screen cleanup and redraw on continuation. Ripple ps/top
use the address-free RIPPLE_PROCINFO ioctl instead of interpreting kernel
pointers from the historical raw process table. The original upstream ps source
is retained for provenance but is no longer the selected build source. Resource
counts distinguish private and multiply referenced data/heap/stack pages.
