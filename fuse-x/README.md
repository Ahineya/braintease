# Fuse-X hosted FUZIX

Fuse-X runs a native FUZIX kernel, bootstrap programs, and independently
compiled applications loaded from the guest filesystem. It boots `/init`, starts
an interactive shell, and uses FUZIX's process table, scheduler, file descriptors and filesystem.
No Ripple instruction interpreter or host `fork` is involved.

## Build and run

From the repository root, with Rust, a host C linker, Python 3 and Node.js:

```sh
scripts/run-fuse-x.sh
scripts/run-fuse-x.sh --run-only
```

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

The [launcher](../scripts/run-fuse-x.sh) builds the compiler, runtime and kernel,
creates `fuse-x/build/ROOTFS.IMG` if missing, and starts the console shell.
Networking defaults to shared Ethernet with NAT internet access on macOS and off elsewhere.
Files persist across rebuilds and launches. Normal builds inventory the ported applications and install successful local
executables, preserving other files and saving a `.bak` (or `.bak.N`) before
changing an existing image. Smaller roots expand to 65,535 blocks (128 MiB minus
2 KiB) and at least 1,024 inodes with their contents preserved. Fresh roots use
that size too. This is the current C16 V2 filesystem's block-count limit;
ordinary file octets occupy 16-bit cells on this disk. `--build-only` prepares
everything without running. `--fresh` backs up and replaces the default image.
`FUSE_X_BUILD` changes the build directory; build output goes to `build.log` there.
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
Attachments replay at most the latest 64 KiB. A client that stops reading is
disconnected once its output backlog reaches 1 MiB, leaving the kernel running.

For finite piped scripts or the old shell-exit behavior, use `--one-shot`.
It runs the console directly, without a persistent broker. The native executable
also accepts `--headless`, `--telnet` and `--one-shot` after `--run IMAGE`.
Direct native runs normally remain alive after console EOF, with existing
sessions continuing; the launcher keeps its console input open across detach.

## Existing images

The executable needs a raw C16 V2 `ROOTFS.IMG`, rather than the outer Ripple FAT
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
results from runtime checks. The shell runs these through FUZIX exec. Build and install
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

FUZIX resolves paths and permissions and owns fork, scheduling, wait, exit,
zombies, descriptors and terminal queues. The native runtime supplies clonable
continuations and private copy-on-write memory. It resumes only the process
selected by FUZIX. Kernel translation units are linked with `--kernel`, placing
their globals in the shared kernel arena; application globals remain private.
Successful exec replaces saved frames and initializes the selected program's
globals. Failed loading leaves the calling program intact.

Bootstrap `/init`, `/bin/fxsh`, `hello`, and the original `check` still select
prelinked entries through `FXN1` manifests. Ordinary macOS executables and Ripple
RXE files cannot run as Fuse-X native programs.

## Networking and remote shells

The existing FUZIX socket bridge and guest IPv4/ARP/ICMP/UDP/TCP stack now run
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

Telnet starts independent Bourne shells on the four guest PTYs. It is an
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

The hosted configuration has 16 FUZIX process slots, including init, the shell
and unreaped children. Private RAM uses 1 KiB copy-on-write pages. Banks 1–4
hold globals and continuation stacks, 8–23 provide a bounded user heap window,
and 31 holds arguments. Shared kernel globals and allocations use banks 240–255.
Current RAM figures are provisional accounting, not enforced physical budgets;
the Ripple port's variable executable allocation is not yet integrated here.

Newly compiled native applications have scheduling checkpoints at loop edges;
function-call continuations also provide checkpoints. About every 5 ms, subject
to reaching a checkpoint, the kernel polls input/networking, advances timers,
delivers signals and schedules ready processes. Pure computation loops can be
interrupted and cannot indefinitely exclude other runnable tasks. Kernel work
and signal handlers executing inside a kernel callback are not interrupted;
blocking host services still need their own nonblocking adapters. Rebuild older
application packages to obtain checkpoints. This is compiler-assisted fairness,
not asynchronous host-thread or machine-instruction preemption. Host clocks advance
kernel timers at these checkpoints and platform services. Interrupt masking records
logical state on the single guest execution thread. Console input is collected
by a host worker and polled without blocking that thread. Four guest PTY pairs
provide independent terminal sessions. Custom handlers run on completed syscall
returns; privilege isolation and GUI windows remain future work.
User-copy validation checks allowed regions, but trusted compiled code can still
address kernel memory directly.

See the [architecture](../docs/fuse-x/architecture.md),
[roadmap](../docs/fuse-x/roadmap.md), and
[native continuation APIs](../rcc-native/PROCESSES.md).
