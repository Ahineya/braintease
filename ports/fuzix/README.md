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
`sync` commits filesystem changes without stopping. `exit` also shuts down this
initial PID 1 shell. The supplied shell is bootstrap userspace written for this
port; the upstream FUZIX shell, libc, and application collection are not yet
ported. It supports the commands shown by `help`, simple whitespace arguments,
and `echo` redirection. Enter/Ctrl-J submits a line, Ctrl-U clears the line,
and Ctrl-D at an empty prompt shuts down cleanly. Quoting, pipelines, and job control are not implemented.

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

A normal launch updates **only FUZIX.SYS**, preserving every byte of ROOTFS.IMG.
The updater saves the previous disk before changing it, writes a temporary
image, fsyncs it, renames it, and fsyncs its directory. No-op updates leave the
image untouched. A stable companion lock prevents concurrent launcher sessions
or offline checks from modifying the same image.

New userspace executables are seeded only when creating a disk. To install an
updated seed image, explicitly use `--fresh`; it resets the guest filesystem
and saves the previous disk as a backup. `--disk IMAGE` boots an existing image
without a build-time update. `make clean` removes compiled artifacts and keeps
disks and backups.

## Storage durability and recovery

RVM COMMIT and COMMIT_ALL now wait for host `fsync`/`sync_all`; failed barriers
retain dirty data and set STORE_CTL's sticky ERROR bit. The block driver checks
this status. Kernel `sync` flushes both filesystem buffers and the VM cache,
including when all guest metadata buffers have already been written. Shutdown
flushes ordinary changes before writing and committing the clean superblock.
It reports a failed barrier instead of claiming successful shutdown.

This filesystem has no journal. A timeout, Ctrl+C, SIGKILL, or host crash can
leave it dirty; unsynced data may be lost. FUZIX mounts dirty roots read-only.
With the VM stopped:

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
The compiler fixes here are standard C support for `register` parameters and
arrow access through pointer typedefs; no GNU C extensions were added.

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
isolation. Networking, upstream libc/application builds, custom user signal
handlers, swap, dynamic multi-bank user heaps, and RBF execution remain future
work. Default signal handling is in the FUZIX kernel; arbitrary code-pointer
handlers are not supported by the current user-memory validator.

## Validation

With the release compiler, assembler, and VM built:

```sh
make -C ports/fuzix test
# Or for the launcher's output directory:
python3 ports/fuzix/tools/test.py --build build/fuzix
node --test ports/fuzix/tools/disk.test.mjs
python3 ports/fuzix/tools/test_terminal.py --build build/fuzix
```

The integration test boots real disks with both JIT and interpreter, verifies
forked mutable data and child exit status, preempts a computing process, checks
arguments to a separate executable, writes and reopens a 77,777-character file
through direct/single/double-indirect blocks, seeks/truncates/deletes it, checks
filesystem persistence and cleanup, repeatedly reuses executable slots, and
exercises offline dirty-filesystem recovery. Host tests cover image structure,
root preservation, backups, no-op updates, and malformed disk mappings. A live
pseudo-terminal test exercises ordered input, control keys, the run-only
launcher, and exclusive disk locking.

## Upstream and licensing

The complete upstream source archive is in `vendor/`, pinned to the latest master
revision fetched on 2026-10-16. A portable working subset is in `upstream/`. See [UPSTREAM.md](UPSTREAM.md) for provenance and
[upstream.patch](upstream.patch) for the core adaptations. Ripple platform
sources, build tools, and bootstrap userspace live outside that snapshot's
platform hierarchy so they can be reviewed together. Sources follow FUZIX's
GPL-2.0 license; see [upstream/LICENCE](upstream/LICENCE).
