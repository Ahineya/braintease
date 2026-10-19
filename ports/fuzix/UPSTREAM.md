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

The MWC Make adaptations add phony targets, optional includes, `?=`/`+=`, `$^`,
first-prerequisite `$<`, epoch-zero file detection and `.DELETE_ON_ERROR`.
Picol uses port-local `picol-extra.inc` and `picol-build.inc` for the documented
Tcl build subset, with environment variables, argument expansion and bounded
lists/output. Ripple exec argument buffers use private kernel stack storage
with a shared 4,096-character argument/environment budget instead of temporary
disk-block buffers; other upstream platforms retain their original limit.

`Library/libs/qsort.c` selects its specialized element paths using C `sizeof`
instead of physical-octet constants. Utility changes add bounded recursive
`mkdir`/`rm`, standard `cut` option parsing, error statuses, trailing-word counts,
`head -n`, missing-operand guards, C16 dump/usage calculations, and matching
format widths. Original copyright and license notices remain with imported
sources; newly rewritten adaptations are marked as such.

The socket header includes `sys/types.h` through the include search path so
Ripple's 32-bit size and syscall types apply even when sockets are included
first. Its syscall carrier selects 32 bits for the Ripple ABI.
The kernel and library interface headers rename private union members to
avoid self-referential field macros in RCC; the public names and layout are
unchanged. Socket syscalls and inode hooks are implemented in
`ripple/sockets.c`, using the repository's guest Ethernet/IP/TCP runtime;
upstream's network daemon and socket engine are not linked. Telnet tools
and their bounded protocol codec are port-local sources, backed by the
existing PTY driver and Bourne shell. The public socket header also declares
`socketpair`; `ripple/localsockets.c` and the port-local `sys/un.h` provide
pathname Unix stream sockets with filesystem permissions and select readiness.
Ripple sockets allocate cache-only inodes with `NO_DEVICE`; `filesys.c`
skips disk writes and disk inode freeing for these entries, including sync
and shutdown with a remote shell still connected.

`Applications/netd/htget.c`, `dig.c`, `echoping.c`, `ntpdate.c` and `netdb.h`
come from the same pinned archive. The clients retain their upstream command
roles, with substantial C16 adaptations: DNS and NTP fields are explicit wire
octets rather than native C structure overlays; HTTP is parsed incrementally,
with partial I/O, chunked bodies, redirects and temporary-file replacement.
`htget` additionally uses the shared guest wolfSSL client in `ports/tls` for
verified HTTPS, with an explicit guest trust store and downgrade rejection.
`Library/include/arpa/inet.h` includes `netinet/in.h` so its address types do
not depend on the consumer's include order.
Queries and downloads use monotonic deadlines rather than replacing process
alarm handlers. NTP validates its peer and request timestamp before exposing
an optional UTC guest-clock update. The original netdb license notice remains.
`ripple/libc/resolver.c` implements shared hostname lookup over guest sockets,
with local hosts/config files, bounded compressed-name/record parsing, CNAME
following and TCP fallback. IPv4 hostent addresses use the packed in_addr ABI,
so h_length is sizeof(struct in_addr), two C16 characters. `ripple/host.c`
provides the small host/nslookup client. The libc also supplies stime/_stime
wrappers for the existing kernel clock-setting syscall. Upstream's incomplete
HTTP server and IRC client are not included in this batch.

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
`ripple/shell-prompt.c` expands PS1/PS2 backslash escapes in fixed buffers,
including identity, directory, clock, ANSI color and multiline prompts. Main's
prompt hook formats once before editing or fallback output; the editor measures
the final logical prompt line and honors nonprinting spans without sending
marker bytes to the terminal. The same module is built for Fuse-X.

`Applications/levee/` is imported from the same pinned archive. Levee was
developed by David L Parsons; its redistribution notice remains in each source
and `levee.h`. The ANSI/Unix build is installed as `levee`; the default `vi` is a relative
symbolic link to Rift. VILE remains separately available. Adaptations provide
explicit C99 return types for legacy implicit-int functions, native-size undo
packing, bounded file/terminal input, checked record I/O and replacement saves,
16-bit character display, names retained after an unnamed buffer is saved,
terminal setup/restoration and window dimensions.
The port uses a 30,000-character core, with one character reserved by the
upstream read/insert algorithms. Unused platform backends remain unmodified.
FUZIX/Ripple now requires the MMU on RVM, with 64 process slots, selected page
maps, and COW user memory. One extra code-image record supports replacement
execs. The compiler ABI is unchanged. The old non-MMU/RBF build, bank snapshots,
one-bank allocator, and physical code-base relocations are retired; only the
MMU build and its scalar/bulk I/O alternatives are maintained. Native Fuse-X
shares common filesystem configuration and PTY code through a separate hosted
platform configuration; it does not depend on the retired VM build.

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
use relative symbolic links to one managed image. The original build/package files are
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

C16 V3 now retains 1,024-character blocks while widening disk block references
and volume/free-space counts to 32 bits. Inodes grow to 64 cells with 17 direct
and single/double/triple indirect roots. File mapping and truncation retain wide
counts; writes reject signed-offset overflow, and truncated partial tails are
cleared. Internal statfs records require a V3 tag to protect older callers from
an oversized record copy; statvfs counters are wide. See the
[V3 specification](../../docs/arch/c16-filesystem.md). V2 roots are converted by
the host tools with backups, retaining inode identities and Unix metadata.
The Ripple FAT chain moves from kernel GP to supervisor banks 98–99. Sparse
host tools avoid whole-image copies. The default root is 1 GiB on both Ripple
and native Fuse-X; no compiler calling convention or VM device changes are needed.

The earlier C16 V2 increased filesystem blocks from 256 to 1,024 native characters, while
keeping the existing RCC ABI and RXE1 executable format. `blk512.c` uses
`INO_PER_BLOCK` and the native number of indirect pointers rather than fixed
8-bit-platform constants. The Ripple disk driver splits transfers at host
storage-window and FAT-cluster boundaries. Image/check/install tools share a
single geometry definition, and the shared FAT image builder accepts an
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

The executable loader tracks 33 live image records, allocating variable-size contiguous code
regions instead of twelve-bank slots. Fork shares an image; exit releases its
last reference. Failed exec frees the staged region and preserves the old image.
The kernel decodes
aligned RXE instruction records from the cached disk block. MMU userspace code
starts at virtual bank zero through as many directory leaves as the configured
pool needs.
The compiler ABI is unchanged. Static globals and the stack each retain a
one-bank limit, while MMU user heaps can span sixteen data/heap banks. RVM
dynamically grows instruction memory up to a configurable ceiling, defaulting to
67,108,864 instructions (512 MiB before JIT allocations). Binary loading, STORC
and bulk uploads enforce the same ceiling. MEMORY_INFO reports actual host
capacity, and the kernel clamps its pool accordingly. The default pool ceiling
is 1024 banks; CODE_BANKS overrides it.

Bulk-memory integration redirects the Ripple kernel's `blktok`, `blkfromk` and
`blkzero` cache helpers through port-local buffer functions when `BULK_IO=1`.
The ordinary helper macros remain for other platforms and `BULK_IO=0`.
`ripple/usermem.c` supplies bounded RAM copy/fill helpers and the common bank-copy
entry used for kernel-stack cloning; `lowlevel.asm` retains the original whole-bank fallback. MMU physical
page initialization and COW copies also use these helpers. Copies preserve
native 16-bit characters, and the mailbox is protected by saving/disabling/restoring
IRQ enables around each submission. Physical buffers use bulk operations from
256 cells upward. User buffers use mapped-page validation and COW resolution, then physical DMA
spans for large copies or CPU accesses for small transfers. Unsupported operations use scalar fallbacks;
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

`CONFIG_PLATFORM_CLOCK` separates instruction-based scheduling interrupts from
host elapsed time. The portable timer code accepts batched decisecond advances,
expires each queued deadline once, updates calendar time arithmetically, and
bounds load-average catch-up after long host pauses. The Ripple platform reads
the shared DEVICE host clocks with IRQ protection, samples running-task CPU time,
and seeds the calendar after mounting root. Clock syscall/resource snapshots
synchronize before returning data. Guest calendar changes leave monotonic time
and alarm deadlines unchanged. `tools/test_timers.py` tests real durations,
calendar isolation, cancellation, timestamps and recovery from a host SIGSTOP.

The Bourne shell integrates ripple/shell-jobs.c for jobs/fg/bg, group-aware waits,
a fork/foreground handshake, terminal preservation and job cleanup. Levee gains
raw Ctrl-Z handling, screen cleanup and redraw on continuation. Ripple ps/top
use the address-free RIPPLE_PROCINFO ioctl instead of interpreting kernel
pointers from the historical raw process table. The original upstream ps source
is retained for provenance but is no longer the selected build source. Resource
counts distinguish private and multiply referenced data/heap/stack pages.


The graphics foundation raises the protected profile to 64 process slots,
32 descriptors per process, 192 open-file objects and 128 cached inodes, and eight PTY pairs.
Kernel close-on-exec masks are conditionally 32 bits and descriptor shifts use
wide operands. Legacy profiles retain their smaller tables. Saved task data and
MMU metadata are relocated to reserved banks; the process page pool is unchanged.
The local GX device and graphical PTY terminal are new port code. Nano-X is an
independent pinned import under `ports/nanox`, with a separate C16 adaptation
audit; the Nano-X server is not linked into FUZIX.

`CONFIG_SELECT` enables inode waiter maps/helpers without the unrelated level-2
facilities. Ripple's syscall 72 uses the shared versioned C16 request and wide
fd sets in `ripple/selectwait.c`; the old upstream syscall is retained only for
other platforms. Pipe reads/writes and last-close notify selectors; writes
preserve nonblocking flags and enforce queue capacity. TTY readiness honors
canonical lines, EOF and noncanonical thresholds, and queue flush/erase notify
writers. Canonical EOF no longer emits a spurious bell. Process exit removes
select registrations before slot reuse. The socket header exposes the port's
SO_ERROR subset; its implementation remains in the existing guest socket bridge.

The Bourne shell's `main.c` loads `$HOME/.shrc` through the existing dot-command
parser after interactive setup and before the first prompt. A once-only guard
survives syntax-error recovery; missing files, empty/unset HOME and nonregular
files are skipped. Script/command-mode shells do not load it unless explicitly
interactive. The existing login `.profile` path remains separate and precedes
interactive `.shrc` loading. `tools/test_shrc.py` checks native and JIT guest
startup, prompt persistence, HOME resolution, script isolation and recovery;
it is included in the shell and native prompt regression suites.

The shared curses sources under `Library/libs/curses/` are imported from this
same archive. Their adaptations are recorded separately in `curses.patch`
(relative to `upstream/`, replay with `git apply --unidiff-zero`); local input,
window allocation and screen lifetime replacements live in `ripple/curses/`.
The superseded upstream routines remain available for provenance but are not
linked. See [CURSES.md](CURSES.md) for the API subset and native/RVM tests.

The color/window extension widens imported cell pointers and cell temporaries
to `chtype`, preserving attributes while adding color-pair bits.
Those mechanical edits are in `curses.patch`. `ripple/curses/color.c` and
`render.c` replace termcap-dependent attribute output and the old refresh path
with ANSI color/DEC line output and clipped, complete-layer composition. The
old `doupdt`, `refresh` and `winscrol` units remain as provenance and are excluded
from both userspace builds. Local scrolling copies cells so derived windows
retain aliases; color-pair changes and terminal restoration invalidate output.

The Unicode extension repacks `chtype` into 21 glyph bits, five attribute bits
and six color-pair bits, retaining its two-C16-character size. Imported curses
movement/edit routines invalidate the preceding grapheme, and ACS/box glyph
carriers are wide. Port-local `characters.c` replaces upstream `waddch` and
`waddstr`; input/rendering use the shared Unicode 16 library. The Bourne shell
reserves bit eight for quote marking so every UTF-8 octet remains data; ASCII
classification guards its legacy tables, and glob placeholders use `QUOTE`.
Its interactive hook edits graphemes and measures display columns. Original
upstream platforms retain their existing quote encoding. These adaptations
remain in `curses.patch` and `upstream.patch` respectively.

The fine-deadline work adds port-local `ripple/deadline.c` and `ripple/clock.c`
on both targets. The original `_time` in `syscall_proc.c` is compiled as
`ripple_legacy_time`, preserving its existing selectors. The port adds a fine
monotonic sample selector without changing the old timer/accounting units.
`ripple/libc/clock.c` replaces the selected upstream clock_gettime/getres units;
the original imported sources remain unchanged for provenance and other ports.
See [TIMERS.md](TIMERS.md) for the clock ABI and actual guest validation.

The Bourne shell's `scan()` dequotes every expanded word, including argv[0].
Upstream skipped that final chain entry, leaving quote marker bits in the command
name: `getpath()` then missed quoted slashes, searched PATH for absolute paths,
and passed a marked argv[0] to scripts. Dequoting after expansion preserves
literal glob characters while fixing quoted builtin names and executable paths.
`tools/test_shell_paths.py` covers both kernels, including single/double quotes,
variables, spaces, punctuation, relative commands, script $0 and exit status.

The 64-process profile packs multiple MMU tables into each reserved physical
bank and passes both bank and offset when selecting roots. Kernel stacks use
banks 32–95; the default table layout uses 96–111. Metadata and graphics follow
below the unchanged 128–255 user page pool. Both hosted and Ripple kernels use
the shared process-limit header. Timeout readiness has two 32-bit words, and
ps/top snapshot buffers cover all 64 slots. Process exhaustion/reuse and
high-slot select timers are covered by `tools/test_process_capacity.py`; deadline
queue tests include expiration and clock-error delivery for slots 32 and 63.

The main RVM and native Fuse-X kernels enable `CONFIG_RIPPLE_SCHED`: shared process transitions
maintain a port-local FIFO and hashed event wait queues, with monotonic slice
budgets (20 ms RVM, 5 ms native) and RVM's existing instruction-count ceiling as a fallback. Other profiles
retain their original state/counting and scheduling paths through
`Kernel/include/process-state.h`. RVM and Fuse-X exit no longer force a global sync;
explicit sync and shutdown keep their barriers. The shared select path tracks
registered inode indices for bounded cleanup and skips empty notification maps.
The port also bounds MMU walks by populated bank prefixes and clones only live
kernel stack cells. See [SCHEDULER.md](SCHEDULER.md) for invariants and validation.

The diff3 batch imports `Applications/V7/cmd/diff3.c` from the same archive,
retaining its V7 range-coalescing algorithm and parent copyright. Adaptations
add the three-file command front end, argv-based diff subprocesses, private
cleaned temporary reports, stdin spooling, checked dynamic change tables,
32-bit offsets, streaming lines, correct empty/deletion handling and escaped
single-dot ed text. `diff.c` gains option termination/separate groups, wide
long-line/sort arithmetic, geometrically grown line/candidate tables, candidate
bounds, checked output and safe ed text.
`tools/test_diff.py` compares reports with host reference ranges, applies scripts
with both host and guest ed, and tests both executable backends on private disks.

The guest symlink extension restores `ln -s` and link-preserving recursive
copy/move, fixes directory-listing readlink paths, and makes find/tar traversal
inspect link inodes. Tar supports USTAR links while rejecting extraction through
symlink parents. Shared kernel changes add bounded iterative pathname expansion,
no-follow namespace operations, link block reclamation and syscalls 68–70.
The public headers expose `symlink`, `lstat`, `readlink` and `ELOOP`; managed
executable aliases now use guest symlinks. Host exports retain their separate
no-host-symlinks policy. See the C16 filesystem specification and
`tools/test_symlinks.py` for the contract and both-target acceptance coverage.

Directory rename is supported in the shared kernel on both FUZIX/Ripple and
Fuse-X. Same-filesystem moves retain the tree's inodes, update `..` and parent
link counts, and permit replacement of an empty directory. Destination ancestry
is checked from the resolved parent to reject cycles even with relative paths.
A kernel-only parent-link helper updates `..` without requiring write permission
on the moved directory itself; both containing directories must be writable.
Simple renames reuse the existing entry, including in a full directory block.
Commander F6 Move and Rename use this syscall without an application workaround.
Cross-filesystem directory moves remain unsupported in Commander.
`tools/test_directory_moves.py --os both` checks the syscall and live Commander
operations, then reboots and runs filesystem consistency checks on private roots.

The full RVM and Fuse-X profiles select `CONFIG_RIPPLE_IO`: cache-only anonymous
pipe inodes route to port-local supervisor rings, while named FIFOs retain their
existing disk path. Directory reads support ABI-compatible batches; lookup and
inode/buffer indexes are bounded, with namespace invalidation and verified
mutation offsets. Free-inode refill uses a cursor and device-local inode
writeback. Block sync groups device barriers without delaying ordinary inode
writes. PTY/TTY hooks use wrap-aware raw queue transfers, preserving canonical
processing and octet conversion. Original notices remain, and modified kernel
units carry a dated Ripple adaptation notice. See [KERNEL-IO.md](KERNEL-IO.md)
for storage bounds, host-folder durability, compatibility and validation.
