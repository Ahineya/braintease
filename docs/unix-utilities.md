# Unix utility porting priorities

The highest-value gaps are shell-script compatibility, source distribution
handling and inspection tools. Most everyday file and text operations already
have a working port. Add the missing capabilities before importing more editors,
pagers or hardware-specific administration programs.

This 2026-10-18 inventory compares the shared utility build list, existing build
reports and image manifests with the complete pinned FUZIX archive. Priorities
assume an interactive development system on both Riplix/RVM and Fuse-X. This is
a source and packaging audit; candidate effort estimates are not compilation or
runtime acceptance results. No utilities were ported during the audit.

## Current coverage

The [shared utility inventory](../fuse-x/apps.py) contains **115 programs and
aliases**, and the current native application report marks all 115 built, with
no blocked entries. The RVM `build/fuzix/apps.json` contains **144 names**, all
with RXE outputs. That larger list also contains bootstrap, diagnostic and
graphics programs. These counts are not counts of distinct core Unix utilities,
and do not enumerate every separately installed GUI, RFL or compiler package.
The subsequent [mux implementation](terminal-multiplexer.md) adds one shared
program to those audit-time counts.

All direct `.c` utility sources currently extracted under
`ports/fuzix/upstream/Applications/util` are already represented in the shared
inventory. The unextracted source collection lives in the
[vendored archive](../ports/fuzix/vendor/fuzix-0a2336cf8ce57cb94ad82f0c9f4cde2b4d578a78.tar.gz),
whose revision and provenance are recorded in [UPSTREAM.md](../ports/fuzix/UPSTREAM.md).

Already covered:

| Area | Existing commands |
| --- | --- |
| File operations | `ls`, `cat`, `cp`, `mv`, `ln`, `rm`, `mkdir`, `rmdir`, `pwd`, `touch`, `chmod`, `chown`, `chgrp`, `mkfifo`, `mknod`, `mktemp`, `basename`, `dirname`, `find`, `dd`, `tar` |
| Text pipelines | `grep`, `fgrep`, `sed`, `awk`, `sort`, `uniq`, `cut`, `tr`, `head`, `tail`, `wc`, `tee`, `comm`, `join`, `split`, `rev`, `expand`, `colrm`, `fmt`, `column`, `pr`, `col`, `tsort` |
| Comparison and checksums | `diff`, `diffh`, `diff3`, `cmp`, `od`, `sum`, `cksum` |
| Shell and development | Bourne `sh`, `test`, `expr`, `make`, `m4`, `dc`, Tcl subset, guest `cc`, `rcc-c`, `rcc-ripple`, `rcc-as`, `rcc-ld` |
| Terminal and editing | `stty`, `tty`, `clear`, `more`, `less`, `ed`, Levee/`vi`, `vile`, Rift, Commander/`mc`, terminal sessions and job control |
| Process and system reports | `ps`, `top`, `kill`, `pkill`, `sleep`, `time`, `nice`, `nohup`, `free`, `uptime`, `df`, `du`, `sync`, `uname`, `date`, `id`, `whoami`, `groups`, `logname`, `env`, `printenv`, `which`, `xargs` |
| Networking | `ifconfig`, `ping`, `host`/`nslookup`, `dig`, `htget` with HTTPS, Lynx with HTTPS, `telnet`, `telnetd`, `echoping`, `ntpdate` |

Shell builtins already supply `cd`, `read`, `wait`, `umask`, `trap`, `export`,
`eval`, `.`, and related control operations. `jobs`, `fg` and `bg` are supplied
by the port's job-control integration. They do not need separate executable ports.
The [userspace reference](../ports/fuzix/README.md#unix-userspace) describes
supported options; having the command does not imply GNU or modern POSIX option
coverage. A built artifact is also weaker evidence than a behavior test.

## Highest priority

The order below favors capabilities that unblock scripts and importing source.

| Order | Capability | Why it comes first | Source and likely work |
| --- | --- | --- | --- |
| 1 | `/bin/[` alias for `test` | Makes ordinary `if [ ... ]; then` scripts usable. | Existing MWC `test` already recognizes an invocation ending in `[`. Mainly installation/alias work; Fuse-X's application-name validation currently excludes brackets. |
| 2 | `printf` command | Predictable formatted output for scripts; avoids relying on `echo` escape behavior or launching awk for each small output. | No standalone application in the pinned archive. Small implementation using existing guest formatting and I/O, with C16 width checks. |
| 3 | Unified `diff -u` plus `patch` | Source porting needs a way to exchange and apply normal patches inside the guest. | Extend the current V7 diff and select a small patch implementation. Our current diff has normal/ed output, not unified/context output. Neither a patch source nor an installed patch command is present. Medium work as a pair. |
| 4 | `gunzip` and `zcat`, then `gzip` | Lets the guest consume compressed source releases and data with pipelines such as `gunzip -c source.tar.gz \| tar xf -`. | No gzip application in the pinned FUZIX archive. The repo already has adapted tinf and gzip decoding under `ports/telegram`, but its current API buffers complete bounded messages. Streaming files, larger lengths, concatenated members and error/output handling need additional work. Compression needs an encoder. |
| 5 | `install` | Common build/install recipes need copying, directory creation and permission setting in one command. | No application in the pinned archive. Small command on existing file APIs; begin with a deliberate option subset and useful failures. |
| 6 | `readlink`, `realpath`, `stat` | Makes symlinks, resolved paths and metadata inspectable from scripts and the desktop development workflow. | No command sources in the pinned archive. `readlink`/`symlink` syscalls already exist; canonical path resolution must handle links and loops correctly rather than just normalize `..`. Small to medium. |

Two existing-tool upgrades belong alongside these ports:

- Add **symlink creation/extraction to tar**. It currently handles USTAR and
  hard-link extraction but rejects symbolic links, which occur in source trees.
  Compression and tar are separate layers; a pipeline is enough initially.
- Add **`find -print0` and `xargs -0` together**, with matching argument-budget
  and exit-status handling. Current `xargs` splits whitespace, has a 255-character
  cap, and advertises `-s` without implementing its limit. Current `find -exec`
  already passes a direct argument vector and is the useful existing alternative
  when filenames contain spaces.

These improve script compatibility, but do not make the historical shell a
complete modern POSIX shell. Functions, `$()`, arithmetic expansion and modern
`set` behavior remain separate shell work. Do not promise that adding a dozen
commands makes arbitrary modern `configure` scripts run unchanged.

## Next priority

| Order | Commands | Value and source considerations |
| --- | --- | --- |
| 7 | `strings`, `file`, format-aware `size` | Useful when debugging imported files and generated programs. Add RXE/FXE/RPO/RPA recognition; upstream `size` understands older FUZIX/binflat formats, not ours. No general `file` or `strings` commands occur in the pinned application archive. Start with bounded identification rather than a large external magic database. |
| 8 | `paste`, `fold`, `nl`, `unexpand` | Fills practical text-processing gaps at relatively low cost. None has a command source in the pinned archive; awk can cover many current use cases. |
| 9 | `sha256sum`, `base64` | Useful for downloaded-source verification and binary/text interchange. Existing `sum`/`cksum` cover different roles. Reuse octet-aware hashing where suitable; these are new command ports, not entries waiting to be enabled in the FUZIX archive. |
| 10 | `timeout`, `pgrep` | Helps guest test scripts and managing desktop services. `pkill` now provides name matching and signals (see `man pkill`). Timers/signals/process reporting exist. Further process matching must respect the currently exposed bounded process names; group cancellation and child cleanup need care. Do not substitute upstream's SysV `killall`. |
| 11 | `tree`, `cal`, corrected `getopt` | Useful, fairly contained upstream candidates. `tree` depends on `nftw`, which is not currently linked into this port. `getopt` is only 36 lines, but lacks an argc guard and prints arguments without safe quoting; fix those semantics. A proper shell `getopts` builtin is separate work and preferable for new scripts that can use it. |
| 12 | `nc`/`netcat`, small `httpd` | Good local networking diagnostics and a way to serve guest files. The socket/select foundation exists. No netcat command is in the pinned archive, and its `httpd` is a stub rather than an HTTP implementation. Treat the server as new work with bounded parsing and file serving. |

Small commands such as `hostname` and `tput` are reasonable additions to a later
convenience batch. They do not unblock as much as patching, archive handling and
script compatibility.

## Later milestones

| Group | Priority rationale |
| --- | --- |
| `cron`, `at`, `atrun` | Valuable for persistent sessions and background tasks after the basic toolset. Source exists in V7/MWC; decide daemon startup, spool format, ownership, output handling and calendar behavior. Add a usable crontab-management interface rather than only a daemon binary. |
| `ar`, `nm`, other binary tools | Useful for development, with format work. The guest linker already creates RPA archives using `rcc-ld --archive`, so ar is not a missing prerequisite for our own libraries. Upstream util `ar.c` only lists archives; the separate BSD `Applications/ar` tree is the fuller implementation. Old assembler `nm` expects its own object formats. |
| `unzip`, then additional compression formats | Useful for imported projects and assets. Establish gzip and safe streaming first; no matching applications are supplied in the pinned archive. |
| `cpio`, `uue`/`uud`, `decomp16` | Existing upstream interchange tools, but tar plus gzip is more useful first. `decomp16` decodes historical Unix compress/LZW streams, not gzip; its multi-process implementation is not an automatic best fit for our resources. |
| `bc`, `units`, `factor` | Useful calculations, with dc and awk already covering many needs. `units` and `factor` exist upstream; bc is a separate source choice. |
| Login/accounts and messaging | `passwd`, `su`, `newgrp`, `who`, `mesg`, `write`, `wall` need a coherent authentication/session milestone. `getlogin` currently falls back to account lookup because the port does not maintain utmp login records. Old crypt/makekey are not a modern authentication plan. |
| Guest `fsck`, `mkfs`, `mount`/`umount` | Important for eventual standalone administration. Host launch/check/repair tooling already serves today's workflow. Guest tools must understand C16 V3, block/inode widths and available block/mount interfaces; compiling old raw-disk utilities unchanged is insufficient. |
| Actual SSH and version control | Useful larger applications after the utility foundation. SSH is a separate protocol/client port; TLS support alone does not implement it. Upstream `util/ssh.c` is a simple local shell, not Secure Shell. Git is a separate larger application investigation, not one of the missing small FUZIX commands. |

## Low priority or unsuitable imports

More pagers/editors (`pg`, `fleamacs`, `marksman`), another shell (`ssh`) and
another long listing command (`ll`) mostly duplicate existing roles. `line`
duplicates a role already covered by shell `read`/existing filters. Import these
for a particular feature, rather than to increase the installed-command count.

`chmem`, legacy `size`, boot/root substitution commands and physical device
tools have assumptions tied to other upstream FUZIX platforms. `reboot` and `telinit`
expect the upstream init-control protocol; our PID 1/host lifecycle is different.
`swapon` requires swapping support. `netd`/SLIP should not replace the existing
guest network stack merely to match upstream filenames.

The archived **SysV `killall` takes a signal and targets nearly every process
except itself, its parent and PID 1**. It is not the familiar name-based command.
Reusing its name without a deliberate behavior decision would be misleading.

Calendars/reminders, mail, old accounting programs and retro transfer/hardware
utilities can follow demonstrated demand. Games are a separate backlog.

## Complete archive candidate catalog

The following **80 unique unported names** come from the actual source entries
in util/V7/MWC common build lists, plus util `ar`/`line`, V7 `cpio`, and the
unported `httpd`/`tinyirc` programs. Duplicate V7/MWC implementations count once.
Our existing bootstrap covers `init`. Helpers, alternate implementations of
already ported commands, the rest of the games and CPU-specific compiler suites
are excluded. Source membership is not a claim of useful or complete behavior.

Archive member paths begin with `Applications/`; use the pinned archive above.

| Category | Names |
| --- | --- |
| Useful command candidates | `getopt`, `tree`, `cal`, `ar`, `size` |
| Scheduling and local services | `cron`, `at`, `atrun`, `httpd` |
| Optional calculations | `factor`, `units` |
| Accounts and sessions | `passwd`, `su`, `newgrp`, `who`, `mesg`, `write`, `wall`, `ac`, `accton` |
| Disk administration | `fsck`, `fsck-fuzix`, `mkfs`, `mount`, `umount`, `remount`, `labelfs`, `fdisk`, `gptparse`, `blkdiscard`, `swapon`, `bd` |
| Legacy interchange and text | `cpio`, `decomp16`, `uue`, `uud`, `look`, `deroff`, `ptx` |
| Optional personal applications | `calendar`, `almanac`, `leave`, `banner`, `tinyirc` |
| Serial and retro transfer | `cu`, `rx`, `sx`, `dosread` |
| Covered roles and platform-specific commands | `ssh`, `pg`, `ll`, `fleamacs`, `marksman`, `line`, `chmem`, `pagesize`, `prtroot`, `substroot`, `reboot`, `telinit`, `setdate`, `setboot` |
| Hardware, terminal helpers and diagnostics | `border`, `gpiotool`, `nvtool`, `kbdrate`, `mode`, `tget`, `tchelp`, `termcap`, `manscan`, `socktest`, `while1` |
| Mail and old crypto | `from`, `mail`, `makekey`, `crypt` |
| Incompatible kill semantics | `killall` |
| Games occurring in these command lists | `moo`, `ttt` |

The top-priority missing commands absent from that catalog need another source
or a small local implementation; they are not hidden in the extracted directory.
Importing from the archive should retain provenance and record adaptations in
the normal port workflow. A command should join the normal images on both OS
targets with behavior coverage, a manual page and documented option/size limits.

## Recommended first batch

Start with **`[` + `printf` + `readlink` + `realpath` + `stat` + `install`**.
These provide immediate utility and avoid a large dependency port.

Then complete **unified diff/patch**, followed by **streaming gunzip/zcat and
tar symlinks**. Those are the larger pieces that most directly make future
source ports possible from inside the guest. Add the text convenience and
diagnostic tools after that foundation.

Useful companions to this audit are the
[userspace/build manifest](../ports/fuzix/Makefile),
[native application recipes](../fuse-x/apps.py),
[historical shell builtin table](../ports/fuzix/upstream/Applications/V7/cmd/sh/msg.c),
[existing test implementation](../ports/fuzix/upstream/Applications/MWC/cmd/test.c),
[C16 filesystem format](arch/c16-filesystem.md),
[guest toolchain formats](../rcc-portable/README.md), and
[existing bounded gzip decoder](../ports/telegram/inflate.c).
