# Kernel I/O and filesystem costs

The full RVM and native Fuse-X profiles enable `CONFIG_RIPPLE_IO`, independently
of scheduler selection. The syscall numbers, executable ABI and C16 V3 disk
format are unchanged. Other configurations retain their original filesystem
and pipe paths.

## Anonymous pipes

`ripple/pipes.c` gives anonymous pipes supervisor-memory rings of 8,192 C16
characters (16 KiB of physical storage). Their inode-cache entries use
`NO_DEVICE`; creating, using and closing a pipe allocates no disk inode/block.
Pipes therefore also work with a read-only root. Named FIFOs retain their
existing disk-backed implementation and image compatibility.

Writes fitting in the ring wait for the complete record to fit, or return
`EAGAIN` without writing it in nonblocking mode. Larger writes can return a
short count. Readers return available data, then EOF after the last writer
closes. Activity timestamps remain visible through fstat without disk writes.
Broken writes generate SIGPIPE/EPIPE. Dup/fork share endpoint lifetime
through the existing open-file table. Queue observation, copying and the sleep
transition use the existing IRQ discipline. Select registration and fatal-exit
cleanup stay with the common inode implementation.

A buffer is allocated on first transfer, then pooled across inode slots after
last close. Reuse avoids clearing/reallocating storage on every shell pipeline.
Only published ring contents can be read. The supervisor allocator retains the pool's high-water storage until reboot;
it is bounded by inode/open-file capacity, rather than growing with the number
of pipelines executed. RVM includes pinned pages in pool usage. Fuse-X uses its
shared kernel arena; precise native resident-memory accounting remains separate
work.

## Directory and inode lookup

The existing public `DIR` structure already has room for sixteen raw entries.
Libc now uses it, requesting whole-record batches with syscall 24. Old callers
requesting one record remain valid, and the new libc also accepts the old
kernel's single-record response. The stream tracks its logical position;
`telldir`, `seekdir` and `rewinddir` account for buffered entries. Rebuild an
application to obtain libc batching; existing binaries benefit from kernel
lookup and I/O changes without an ABI migration.

`ripple/namecache.inc` supplies 64 bounded, direct-mapped positive/negative
lookup entries, keyed by device, parent inode and component. Permissions and
mount/symlink traversal still run normally. Directory changes invalidate that
parent's entries; inode freeing, device-cache removal and directory truncation
invalidate affected identities. Cached mutation offsets are verified before
reuse. A miss scans a block at a time and records an unused insertion slot,
so creation/deletion do not repeatedly invoke the complete read path for each
32-character directory entry.

A separate 256-slot inode index validates device/inode identity before using a
cached slot. A collision or recycled slot falls back to the ordinary inode-table
search. Host-folder lookup bypasses the disk name cache entirely: host edits,
replacement and containment checks retain their own contract.

Free-inode refill resumes from a per-mount cursor and wraps once. It makes
that device's dirty inode records visible to the block cache rather than
synchronizing unrelated filesystems and file data. Ordinary inode writes keep
their prior immediate-write behavior.

## Block cache and writeback

RVM uses sixteen block buffers; Fuse-X uses thirty-two. Their storage comes from
the supervisor allocator after its initialization, outside the kernel's static
global bank. Blocks retain the existing 1,024-C16-character geometry. Hash
buckets and bounded collision chains accelerate hits; replacement still uses
the existing age policy on a cache miss.

Buffer writeback writes dirty buffers and then issues one flush per participating
device, instead of one per buffer. The enclosing sync retains its final barriers for mounted devices, including
when only immediate metadata writes were pending.
Failed writes/barriers leave affected buffers dirty for retry. Explicit inode
writes, synchronous I/O and clean-shutdown ordering remain in place. This is
not a journal or a new crash-atomicity guarantee.

## Terminals and PTYs

`ripple/kernel-io.c` provides wrap-aware queue copies. Raw PTY input and output
use bulk user-memory transfers, with notifications per run/queue transition.
Canonical editing, signals, flow control and output transformations retain
the existing line discipline. Output runs stop before characters requiring
transformation and fall back to the scalar path when blocked.

Pipes preserve all sixteen bits of a C character. Terminal queues preserve the
existing octet behavior, masking high bits even on bulk transfers. Large
requests are clamped before conversion to a 16-bit count, including requests
of exactly 65,536 characters. VMIN/VTIME, partial progress, nonblocking errors,
hangup, foreground process checks and select semantics are retained.

## Fuse-X host folders

Descriptor validation no longer performs an unconditional host metadata refresh.
Path lookup, `fstat` and SEEK_END refresh where their results need current
metadata; actual reads/writes still operate on the opened host object. Writes
use the returned offset to update local size instead of an extra stat request.

Close is no longer an implicit global durability barrier. Dirty objects retain
capabilities after last close/unlink until sync; clean writable opens do not.
At most 64 dirty objects are retained before a batch flush. Guest sync,
O_SYNC writes and orderly shutdown keep their durability paths. Host namespace
visibility, identity checks and directory containment are unchanged.

## Validation

From the repository root:

```sh
python3 ports/fuzix/tools/test_kernel_io_unit.py
make -C fuse-x test-kernel-io
make -C ports/fuzix BUILD="$PWD/build/fuzix" test-kernel-io
python3 ports/fuzix/tools/test_directory_moves.py --os both
python3 ports/fuzix/tools/test_pipe_recovery.py
cargo test -p rcc-native-runtime --lib
node --test ports/fuzix/tools/disk.test.mjs
```

The host fixture compiles production ring/cache helpers with undefined-behavior
sanitization. It checks wrap/full/empty behavior, failed copies/allocation,
atomic records, interrupted waits, inode-slot exhaustion, buffer pooling across
slots, name invalidation, hash collisions and write/barrier failures.

The actual-kernel probe checks competing writers, nonblocking operations,
160-file directory growth, cursor seeking, negative lookup invalidation and
inode reuse, binary/raw/cooked PTYs, 65,536-character requests, shared-folder
writes, read-only-root pipes, and shutdown/reboot consistency. It also runs the
existing select/signal/terminal and symlink syscall probes. Separate suites
exercise the Bourne shell, jobs, live Commander moves and host-folder editing.
Only disposable images/exports are used. Historic orphaned-pipe image repair
remains covered by the host disk tests; new anonymous pipes leave no orphan
inode to repair.

Use `tools/test_kernel_io.py --benchmark-only --repetitions 3 --output DIR`
for measurements. `--executable SAVED_KERNEL --probe SAVED_FXE` reuses the same
application bytes for a native comparison. Results include timings, hashes and
reboot transcripts. Avoid concurrent builds/tests during comparison. Native
results are recorded in [Fuse-X performance](../../fuse-x/PERFORMANCE.md).
