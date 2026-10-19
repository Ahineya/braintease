# Fuse-X — hosted Riplix

Fuse-X runs a native Riplix kernel, bootstrap programs, and independently
compiled applications loaded from the guest filesystem. It boots `/init`, starts
an interactive shell, and uses the Riplix process, scheduler, descriptor and filesystem machinery.
Riplix is derived from FUZIX; original source credits and licenses are retained.
No Ripple instruction interpreter or host `fork` is involved.

## Build and run

From the repository root, with Rust, a host C linker, Python 3 and Node.js:

```sh
scripts/run-fuse-x.sh
scripts/run-fuse-x.sh --run-only
scripts/run-fuse-x.sh --rebuild --build-only
```

Normal launches reuse unchanged host tools, kernel, applications and guest
compiler packages. Content checks also notice missing or changed build outputs.
The first launch after upgrading the build system may rebuild once to record
its inputs. `--rebuild` forces rebuilding while preserving the disk;
`--run-only` skips building and installation. The disk is checked and updated
only when its managed files change.

The [guest C toolchain](../rcc-portable/README.md) is installed as `/bin/cc`
with headers and libraries under `/usr/lib/rcc`. It currently produces Ripple
RXE programs for Riplix/RVM; native Fuse-X executable generation still uses the
host compiler.

The default shell is the existing FUZIX Bourne shell. Try `help`, `ls /bin`,
`echo hello | cat`, `echo saved > /tmp/note`, `cat /tmp/note`, `jobs`, and `levee`.
It supports scripts, expansion, redirection, pipelines, background jobs and
terminal job control. Use `shutdown` for a clean exit. `exit` starts another
console shell; `/init` keeps the kernel alive and reaps orphaned children.
The launcher owns a persistent console session. Press **Ctrl-]** to detach,
then use `scripts/run-fuse-x.sh --attach` to reconnect to the same terminal,
including a running editor or foreground program. Closing the host terminal
also detaches. Host terminal settings are restored when the client exits.
The original small shell remains available as `/bin/fxsh` for debugging.

Use `pkill sleep` to send TERM by process name, `pkill -x lynx` for an exact
match, or `pkill -9 -e lynx` to send KILL and print affected names/PIDs.
Matching uses the eight-character names shown by `ps`; see `man pkill`.

The Bourne shell supports colored and dynamic `PS1`/`PS2` prompts, for example:

```sh
PS1='\[\e[32m\]\u@\h:\w\[\e[0m\]\$ '
```

See the [shared shell's prompt escape table](../ports/fuzix/README.md) for the
supported escapes. Directory and clock values refresh at each prompt; line
editing accounts for colors, nonprinting sequences and multiline prompts.

The [launcher](../scripts/run-fuse-x.sh) builds the compiler, runtime and kernel,
creates `fuse-x/build/ROOTFS.IMG` if missing, and starts the console shell.
Networking defaults to shared Ethernet with NAT internet access on macOS and off elsewhere.
Files persist across rebuilds and launches. Normal builds inventory the ported applications and install successful local
executables, preserving other files and saving a `.bak` (or `.bak.N`) before
changing an existing image. Smaller roots and C16 V2 images upgrade to a sparse 1 GiB C16 V3 root
with at least 1,024 inodes, preserving file contents, metadata and hard links.
V3 uses 32-bit block references; ordinary file octets still occupy 16-bit cells.
See the [format and migration guide](../docs/arch/c16-filesystem.md).
`--build-only` prepares everything without running. `--fresh` backs up and
replaces the default image.
`FUSE_X_BUILD` changes the build directory; full build output goes to `build.log` there.
The terminal shows elapsed time, build stages and current compilation activity,
with an update every ten seconds during quiet work or build-lock waits.
Interactive sessions have no default timeout; `--timeout N` sets one in seconds.

```sh
scripts/run-fuse-x.sh --boot-check
scripts/run-fuse-x.sh --write-check 'hello from Fuse-X'
scripts/run-fuse-x.sh --read-check 'hello from Fuse-X'
make -C fuse-x test
```

`--boot-check` mounts read-only and checks kernel initialization, PID 1, file
syscalls and host clocks, then stops at the `/init` loading boundary. The write
check replaces `/tmp/fuse-x-check` through kernel syscalls, syncs and exits; the
read check verifies it in a separate read-only launch. Checks default to a
ten-second timeout. These test persistence after orderly exits, not crash recovery.

## RFL applications

Normal builds also install `rfl-demo` and `rfl-fm`, the modular RFL Commander,
with `mc` as its short alias. Try `mc / /tmp` for two panels, file
viewing/copy/move/mkdir/delete, Ctrl-P for one panel, and Ctrl-O for a guest
interactive shell (return with `exit` or Ctrl-D). Ctrl-A/F1 opens About, F4 edits with VISUAL/EDITOR (default vi),
`r` renames, and Enter runs executable files. F10 restores the shell screen.
Regular files move between `/mnt/host` and guest storage by copy-then-remove.
F9 selects a color theme; Classic Blue is the subdued default. Example themes
live in `/usr/share/mc/themes`, defaults in `/etc/mc.ini`, and user overrides in
`$HOME/.config/mc/config.ini`. Prompts and the viewer use floating windows;
long listings show scroll indicators and visible selection in both panels.
The application is written in RFL using `std/posix` and `std/curses`; small C
adapters connect those libraries to guest libc. Reusable INI/theme companions
are available to C and RFL applications; see [application libraries](../libraries/README.md).

Build another guest application on the host with
`target/release/rfl build app.rfl --os fuse-x -o app.fxe`.
See [RFL OS builds and Commander](../rfl/os/README.md) for installation,
controls, module boundaries, current limits and `make -C fuse-x test-rfl`.

## Persistent and headless sessions

```sh
scripts/run-fuse-x.sh --run-only --detach  # Leave the console running in the background
scripts/run-fuse-x.sh --attach             # Reconnect; recent console output is replayed
scripts/run-fuse-x.sh --stop               # Stop guest processes and sync storage
scripts/run-fuse-x.sh --run-only --headless --telnet
```

`--headless` starts in the background without a local shell. `--telnet` explicitly
starts the guest Telnet service; a remote shell can exit without stopping the
kernel. In headless mode `--attach` shows diagnostics and has no local shell.
On macOS, background launches from a terminal keep the console connected through
network authorization and detach once init starts. For noninteractive launches,
use `--attach` to answer any pending network authorization prompt.
Telnet is the existing unauthenticated development service described below.

One host console client can attach at a time. Reconnection resumes that terminal;
it does not allocate another guest PTY or shell. Session state and console logs
live in `fuse-x/build/session/` (or `$FUSE_X_BUILD/session/`). The local socket is
accessible only to its owner. `--attach` and `--stop` operate without building;
use them alone, with `FUSE_X_BUILD` selecting a different session directory.
A running session prevents the launcher from rebuilding/installing its image.
Native kernels also take an advisory image lock, shared for read-only checks and
exclusive for writable sessions. Keep direct image-editing tools offline.

Shutdown asks live guest processes to terminate, allows a short grace period,
kills remaining tasks, reaps them and flushes the filesystems and host image.
Host SIGTERM follows this path. Host crashes, SIGKILL and execution timeouts are
not orderly shutdown. The broker's `console.log` records guest output while no
client is attached; it rotates at 8 MiB and retains one previous log.
Attachments replay at most the latest 64 KiB. A slow attached client applies
backpressure at a 1 MiB output backlog while input and session controls remain
available; output floods no longer force detachment.
Attached clients also forward terminal dimensions on connection and resize;
the kernel exposes the current physical-console size through `TIOCGWINSZ`.
Commander rebuilds/repositions its layout and dialogs when dimensions change.
Remote PTYs retain the sizes negotiated by their own clients.

For finite piped scripts or the old shell-exit behavior, use `--one-shot`.
It runs the console directly, without a persistent broker. The native executable
also accepts `--headless`, `--telnet` and `--one-shot` after `--run IMAGE`.
Direct native runs normally remain alive after console EOF, with existing
sessions continuing; the launcher keeps its console input open across detach.

## Existing images

Guest development tools include `cc`, the expanded MWC `make` and the Picol Tcl
build subset. Compiler sources and their guest-only build coordinator are
installed under `/usr/src/rcc`; see [guest source builds](../rcc-portable/README.md#guest-source-builds).
Use the shared folder for build outputs that should survive a disk replacement.
The portable backend currently produces Ripple RXE executables, which run in
Riplix/RVM; native FXE emission remains a separate self-hosting stage.

The executable needs a raw C16 V3 `ROOTFS.IMG`, rather than the outer Ripple FAT
disk. The signature and physical image size are checked before boot.

```sh
scripts/run-fuse-x.sh --from-ripple /path/to/ripple-disk.img
scripts/run-fuse-x.sh --disk /path/to/ROOTFS.IMG --install-apps
```

Extraction creates a separate default image; the source disk is unchanged.
An existing default requires `--fresh`. `--disk` selects an existing raw image
and cannot be combined with extraction or `--fresh`. Explicit images are not
modified during preparation unless `--install-apps` is supplied. Shell sessions
and write checks subsequently mount the selected image read/write. Keep these
images separate from a simultaneously running Ripple instance.

## Native programs and process ownership

Applications can now be compiled independently and loaded from guest files.
The build includes the ported application collection, the Bourne shell,
and `native-hello`; [application coverage](APPLICATIONS.md) distinguishes build
results from runtime checks. The shell runs these through Riplix exec. Build and install
your own program without relinking the kernel:

```sh
python3 fuse-x/cc.py fuse-x/examples/hello.c -o /tmp/hello.fxe
node fuse-x/image.mjs put fuse-x/build/ROOTFS.IMG /bin/outside /tmp/hello.fxe
scripts/run-fuse-x.sh --run-only
```

Install while the session is stopped, then run `outside` in the guest shell.
See [native executables](EXECUTABLES.md) for compiler options, libc coverage,
the package format, loading, code lifetime and remaining application work.

## Shared Mac folders

The launcher automatically creates `fuse-x-shared/` at the repo root and mounts
it read/write at `/mnt/host`. Git ignores this folder. Use `--share DIRECTORY`
to select another folder, or `--no-share` to disable the mount for a session.
Check modes do not mount the default folder.

```sh
scripts/run-fuse-x.sh --run-only
# In Fuse-X:
cd /mnt/host
ls
echo hello > guest-note.txt
```

The file is an ordinary Mac file, and host changes become visible on subsequent
guest lookups. The root disk remains the system filesystem. Older roots get an
empty mount directory through the normal guest filesystem when needed; a share
does not require importing the folder or reinstalling applications.

You can compile straight into that directory with `fuse-x/cc.py` and execute
the result from `/mnt/host`, without copying it into the root image. The guest
still owns descriptors, permissions, offsets, fork/dup and exec. Binary files use
one octet per guest cell at this boundary, rather than the disk image's C16
encoding. Writes, directory creation/removal, hard links, atomic rename,
truncate, chmod and timestamps reach the host filesystem directly.

The initial bridge supports regular files and directories with component names
up to 30 octets. Longer names and host symlinks/special files are omitted from
listings; direct access rejects them. Host permissions apply, with read access
needed for lookup. Guest ownership is reported as root; host ownership changes
and filesystem capacity reporting are unsupported. Mounts last until shutdown;
multiple exports and live mount/unmount controls remain future work. See
[storage details](../docs/fuse-x/storage.md).

Riplix resolves paths and permissions and owns fork, scheduling, wait, exit,
zombies, descriptors and terminal queues. The native runtime supplies clonable
continuations and private copy-on-write memory. It resumes only the process
selected by Riplix. Kernel translation units are linked with `--kernel`, placing
their globals in the shared kernel arena; application globals remain private.
Successful exec replaces saved frames and initializes the selected program's
globals. Failed loading leaves the calling program intact.

Bootstrap `/init`, `/bin/fxsh`, `hello`, and the original `check` still select
prelinked entries through `FXN1` manifests. Ordinary macOS executables and Ripple
RXE files cannot run as Fuse-X native programs.

## Networking and remote shells

The existing Riplix socket bridge and guest IPv4/ARP/ICMP/UDP/TCP stack now run
in shared hosted kernel memory. Native applications use the existing socket and
resolver libc. `ifconfig`, `ping`, `host`, `nslookup`, `dig`, `echoping`,
`ntpdate`, `htget`, `telnet`, and `telnetd` install with the other applications.

```sh
scripts/run-fuse-x.sh
# In Fuse-X:
ifconfig
ping -c 1 192.168.64.1
telnetd &
# From a host terminal with a Telnet client:
telnet 192.168.64.250
```

The guest uses `192.168.64.250/24`, with gateway and default resolver
`192.168.64.1`. Interactive consoles attach the macOS vmnet helper before guest
input handling starts, so its sudo prompt does not compete with the console.
Noninteractive launches attach lazily on first socket use. Shared NAT is the macOS
default; `--net host` restricts access to the host and guest, and `--net off`
keeps the session offline. `--net /path/to/socket` selects the existing raw
Ethernet backend. `RCC_NATIVE_NET` (then `RIPPLE_NET`) supplies the default.
Avoid running another guest with the same address on the same network.

Telnet starts independent Bourne shells on the sixteen guest PTYs. It is an
unauthenticated, unencrypted development service; start it explicitly on a
trusted network. Closing one remote session leaves the main shell and other
sessions running. The init supervisor owns the overall session independently
of any shell. `--headless --telnet` starts a remote-only session; network menus
and live attachment changes remain future work.

`test_network.py` exercises socket descriptors across dup/fork, binary UDP/TCP,
nonblocking reads, half-close, bidirectional ICMP, DNS, HTTP, UDP NTP and
concurrent Telnet sessions against an independent Ethernet peer. It also checks
remote job control, Levee, the Telnet client, reconnect and offline persistence.
The opt-in `test_network_host.py --executable fuse-x/build/fuse-x` checks
real host ping and TCP Telnet through macOS vmnet. Run it separately with vmnet
authorization available and no other guest using the default address.

## Platform boundary and current limits

The build reads `ports/fuzix/` without changing its sources. Hosted configuration,
platform adapters, applications and separate build output live here. RCC retains
the 16-bit cell and logical bank/offset pointer ABI while generating host machine
code. Disk layout remains compatible with the Ripple C16 filesystem.

The hosted configuration has 64 Riplix process slots, including init, the shell
and unreaped children, with 192 system-wide open-file entries and 128 cached
inodes. Private RAM uses 1 KiB copy-on-write pages. Banks 1–4
hold small globals and continuation stacks, 31 holds arguments, and larger
applications reserve banks 32–47 for globals. Current SDK allocators select
banks 48–239 for up to 24,000 KiB of private heap; old packages retain the
bounded 8–30 heap window. Shared kernel globals and allocations use banks
240–255. The arena switch preserves arguments, globals and legacy allocations.
The shared shell tracks up to 64 jobs and pipeline members.
`make -C fuse-x test-process-capacity` checks all process slots, high-slot
timeouts, process snapshots, cleanup and 40 simultaneous background jobs using
a disposable disk.
Current RAM figures are provisional accounting, not enforced physical budgets;
the Ripple port's variable executable allocation is not yet integrated here.

Newly compiled native applications have scheduling checkpoints at loop edges;
function-call continuations also provide checkpoints. About every 5 ms, subject
to reaching a checkpoint, the kernel polls input/networking, advances timers
and delivers signals. Runnable/event queues use the shared scheduler, with a
native 5 ms slice and bounded wakeup-driven rescheduling. Pure computation loops can be
interrupted and cannot indefinitely exclude other runnable tasks. Kernel work
and signal handlers executing inside a kernel callback are not interrupted;
blocking host services still need their own nonblocking adapters. Executable
loading explicitly yields between released file chunks, prepares native code on
two host workers, and installs at most 128 functions between scheduler polls.
Rebuild older
application packages to obtain checkpoints. This is compiler-assisted fairness,
not asynchronous host-thread or machine-instruction preemption. Host clocks advance
kernel timers at these checkpoints and platform services. Interrupt masking records
logical state on the single guest execution thread. Console input is collected
by a host worker and polled without blocking that thread. Sixteen guest PTY pairs
provide independent terminal sessions. Custom handlers run on completed syscall
returns; privilege isolation remains future work.
User-copy validation checks allowed regions, but trusted compiled code can still
address kernel memory directly.

See [native performance](PERFORMANCE.md) for COW page-table sharing, compact
continuations, bounded direct-call helpers, compatibility and benchmark commands.

See the [architecture](../docs/fuse-x/architecture.md),
[roadmap](../docs/fuse-x/roadmap.md), and
[native continuation APIs](../rcc-native/PROCESSES.md).

### Descriptor readiness

The shared guest `select()` interface supports regular files, pipes, terminals,
PTYs and internet/local guest sockets. Unix-domain stream sockets and
`socketpair()` work without Ethernet. Both targets have 64 internet socket
endpoints, 64 active TCP connections, 16 TCP listeners, 16 bound UDP ports and a separate
pool of thirty-two local endpoints. See [local IPC](../docs/networking/local-ipc.md).
Both targets provide 32 descriptors per process; sets
have the same two-word C16 layout as RVM. Waiting suspends the native
continuation while other sessions and host idle processing proceed. TCP
asynchronous connect exposes `getsockopt(SOL_SOCKET, SO_ERROR)`. Both graphics
backends expose per-owner input readiness through `select()`.
See [the readiness contract](../docs/networking/select.md).

Run `python3 ports/fuzix/tools/test_select.py --native` from the repository root
after building Fuse-X and its applications. It uses a disposable root and a
local Ethernet fixture, with no internet dependency.

### Persistent shell settings

Interactive Bourne shells load `$HOME/.shrc` once before the first prompt,
including console and Telnet sessions. Root starts in `/root` with `HOME=/root`,
so edit `~/.shrc`. `cd` and `cd ~` return home; `~/path` also works in commands,
redirections and filename completion. Scripts and ordinary `sh -c` do not source it. Put prompt assignments
there, followed by a separate `export PS1` command, and use `. "$HOME/.shrc"`
to apply changes in the current shell. See the
[shared shell instructions](../ports/fuzix/README.md) for a colored example.
The file survives session restarts and application upgrades.
Image upgrades move the known personal dotfiles, app settings and saved Telegram
login from their old root locations into `/root`, retaining their permissions
and file identities. Conflicting old/new files stop the upgrade without changing
the image. See [home directories and app state](../ports/fuzix/README.md#home-directories-and-app-state).

## Native graphics

`scripts/run-fuse-x.sh --gui` starts a persistent Nano-X service and opens a
native graphical terminal as well as the reconnectable console. Normal interactive sessions also allow a window to open
on demand: run `gterm`, `nxdraw`, or `startx` from the guest shell. `--headless`
keeps presentation disabled. Rebuild/install the applications before using
`--run-only` with an older image.

All native windows belong to `Fuse-X.app`, with one Dock icon. **Command-N** or
**File → New Terminal** starts another guest shell on its own PTY. The **Window**
menu and Dock menu list current windows and restore minimized windows. Closing
the last window keeps the kernel running; New Terminal works afterwards.
**Quit Fuse-X** / Command-Q requests the same orderly guest shutdown and disk
flush as `shutdown`. The bundle is launched by the script with the selected disk
and host-folder options; standalone Finder launch is not configured yet.

The hosted `/dev/gfx` implements GX RGB565 creation, upload, fill, overlapping
copy, presentation, titles, input/state, Unix readiness and destruction. Up to
16 surfaces have independent input queues and opaque, non-reused handles.
Riplix checks process ownership; inherited descriptors cannot destroy a parent's
windows. Successful exec and process exit release owned windows. Sixteen PTYs
currently limit the total of native terminals, Nano-X terminals and Telnet
shells. Nano-X windows resize their guest canvases and notify clients (up to
1920×1080, subject to guest memory). Both `nxterm` and `gterm` resize their
terminal grid and PTY, so shells and editors receive the new dimensions.
Resize with the normal Mac window edges/corners or the green window control.
Fixed-size clients retain that restriction. Other GX programs must handle
`GX_RESIZE` and acknowledge accepted dimensions through `GX_CONFIGURE`.

On macOS, `gterm` uses cached CoreText glyphs and Retina-resolution frames.
It reads the default iTerm2 profile's font and ANSI palette, including its
separate light/dark colours. Without that profile it uses Menlo 13 and a muted
palette. Host environment overrides are `FUSE_X_TERM_FONT='Menlo 14'` and
`FUSE_X_TERM_APPEARANCE=dark` (or `light`); restart the hosted session after
changing these. `FUSE_X_TERM_STYLE=classic` selects the bitmap renderer.
`nxterm` and RVM keep their existing bitmap fonts. The shared VT still
approximates extended colours to its sixteen-colour cell palette.

Native text rows cross an optional `GX_CAP_TERMINAL` interface as bounded
UTF-8 graphemes, widths and styles; guest font rasterization and per-glyph
pixel uploads are bypassed. Font metrics determine resize and mouse mapping.
Mux compresses blank runs and brackets redraws with synchronized-output
markers, which `gterm` presents together. A one-second timeout recovers from
an interrupted update; resizing briefly coalesces foreground SIGWINCH redraws.

Drag with the mouse to select terminal text, then use **Command-C** or
**Edit → Copy** to put UTF-8 text on the Mac clipboard. Background output,
cursor reports and colour changes preserve the selection while its characters
remain unchanged. Copy uses a text snapshot across fragmented synchronized
redraws; mux also waits for complete pane frames. Typing, pasting, resizing,
switching terminal screens or changing the selected characters clears it.

Run `nxclock &`, `nxeyes &` or `nxterm &` in GUI mode: each Nano-X top-level
opens its own Mac window with native chrome. Child widgets stay in their
parent's private canvas; overlapping virtual coordinates do not overlap pixels
across native windows. macOS owns top-level placement and window management.
`startx` ensures the service exists, opens `gterm`, and returns
to the shell. It reuses a running service and does not start NanoWM. Closing
client windows leaves the service and kernel running. Native GUI clients release
inherited terminal stdio and survive launching-terminal closure; redirected
files/pipes and explicit process termination still work. The RVM desktop and
shared application sources remain unchanged. See the
[desktop design](../docs/fuse-x/desktop.md) for lifecycle details and limits.

The adapter copies guest buffers synchronously and publishes immutable frames.
The Mac main thread owns windows/GPU/menus; the guest worker owns the kernel.
No UI callback retains a guest pointer or directly forks a guest process. New
Terminal requests are bounded and consumed by PID 1. Shared graphics sources
are adapted in the build directory, leaving the VM desktop sources unchanged.

Run the integrated checks on a disposable disk:

```sh
make -C fuse-x test-graphics
cargo test -p rcc-native-runtime --lib
```

The checks cover C16 layouts, ownership across fork, stale handles, title
updates, overlapping copies, exit/exec cleanup, Nano-X drawing, a PTY shell,
client timers/malformed requests/resource limits, private top-level/child
rendering, resize/reparent/map/title/close, persistent service startup and GUI
survival across terminal hangup with PTY reuse. Native window/menu interactions also require a graphical Mac
session; the restricted command runner cannot register an AppKit application.
