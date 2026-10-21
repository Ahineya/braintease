# x64 filesystem adoption

Status: investigation plan, 2026-10-21. **Adopt an existing filesystem and its
recovery ecosystem. No replacement format or implementation is selected yet.**
This complements the [x64 execution/ABI plan](native-x64.md), the
[current C16 V3 format](c16-filesystem.md), and the
[HP integration plan](../../tools/x86_64/hardware/hp-integration-plan.txt).
An ext2 intermediate is optional; avoid migrating the installed root twice if
a suitably bounded ext4 implementation meets the gates directly.

## What actually needs replacing

The current filesystem has 30-character directory names and no journal. This
is more than a storage-driver limitation:

- [`Kernel/include/kernel.h`](../../ports/fuzix/upstream/Kernel/include/kernel.h)
  embeds the on-disk `dinode` in `cinode`, uses 16-bit inode identities, fixed
  `direct` records and the C16 superblock inside `struct mount`.
  [`filesys.c`](../../ports/fuzix/upstream/Kernel/filesys.c) implements lookup,
  allocation and mounting around those representations. The mount table is
  not already a general filesystem-operations interface.
- [`boot/root.inc`](../../tools/x86_64/boot/root.inc) exposes one `root` stream
  to the compiled C16 kernel. It explicitly translates octets versus LE16
  cells. Adding an ext4 superblock parser there would not replace pathname,
  inode, cache or syscall behavior above it.
- [`boot/block.h`](../../tools/x86_64/boot/block.h) already offers synchronous
  native-octet transfers and a separate `block_flush()`. The
  [NVMe implementation](../../tools/x86_64/boot/nvme.c) implements reads,
  writes, unaligned read/modify/write and the NVMe Flush command behind the
  selected partition's write guard. Those are reusable transport foundations.
- Current [format/build](../../ports/fuzix/tools/disk.mjs),
  [read](../../ports/fuzix/tools/read-root.mjs),
  [check](../../ports/fuzix/tools/fsck.mjs) and install tools know C16 V3.
  A new root needs matching provisioning, package installation, recovery and
  executable-loading paths; teaching only the kernel to mount it is insufficient.

Changing `NAME_MAX`, widening a native C structure or formatting the current
partition is not the first experiment. Keep C16 V3 available during adoption.

## Shortlist: format versus implementation

| Existing format | Why evaluate it | Cost and boundary |
|---|---|---|
| **ext2** | Familiar Unix permissions, links and long names; a smaller read/write feature surface and a useful diagnostic control for the ext family. | **No journal.** It does not satisfy the intended crash-recovery milestone. Use it as a disposable bring-up fixture or optional intermediate, not an obligatory installed-root migration. |
| **ext4 with an explicit supported feature profile** | Journal/recovery, long names, extents and Linux recovery interoperability fit the intended root. | Journal replay, transaction ordering, supported feature masks, allocation and error handling must actually work. “ext4 support” does not mean every image produced by current `mkfs.ext4` is supported. |

Linux v6.12's [ext2 documentation](https://www.kernel.org/doc/html/v6.12/filesystems/ext2.html)
describes the format and compatibility flags; its historical implementation
notes are not a current portability guarantee. The same version's
[ext4 layout](https://www.kernel.org/doc/html/v6.12/filesystems/ext4/overview.html)
and [journal source documentation](https://github.com/torvalds/linux/blob/v6.12/Documentation/filesystems/ext4/journal.rst)
are reference specifications. ext4 and JBD2 use different on-disk endianness.
Default metadata journaling does not promise every recent user-data write
survives a crash. File sync, directory sync and package replacement need an
explicit durability contract.

Two implementation approaches merit a bounded comparison:

1. **Port an existing portable ext implementation first.**
   [lwext4 at commit `58bcf89a121b72d4fb66334f1693d3b30e4cb9c5`](https://github.com/gkostka/lwext4/tree/58bcf89a121b72d4fb66334f1693d3b30e4cb9c5)
   is a concrete candidate, with ext2/3/4 configurations and
   [journal/recovery code](https://github.com/gkostka/lwext4/blob/58bcf89a121b72d4fb66334f1693d3b30e4cb9c5/src/ext4_journal.c).
   Its core uses C-library services and dynamic allocation, plus supplied
   block/locking callbacks; it is not a ready Riplix VFS backend. Audit the
   selected allocation, locking, error and endian paths under ordinary native
   C, then define checked C16/native calls. Do not compile disk structures with
   C16 layout assumptions. Its
   [feature masks](https://github.com/gkostka/lwext4/blob/58bcf89a121b72d4fb66334f1693d3b30e4cb9c5/include/ext4_types.h)
   and configuration, rather than README claims alone, must drive a stricter
   Riplix allowlist. In particular, upstream's ignored-feature mask includes
   recovery and multi-mount protection: recovery must be performed correctly,
   and unsupported multi-mount behavior must not be silently accepted.
2. **Port Linux ext2/ext4 code with its required services.**
   The v6.12 [ext2 VFS glue](https://github.com/torvalds/linux/blob/v6.12/fs/ext2/namei.c)
   and [ext4 sync path](https://github.com/torvalds/linux/blob/v6.12/fs/ext4/fsync.c)
   show dependencies on Linux VFS/page-cache, buffer/block, journal and locking
   APIs. This is substantially broader than adapting a block driver. Use these
   sources to validate semantics even if a portable implementation wins.

License facts are source-specific, not a reason to invent a new format:
The cited Linux C implementation files carry GPL-2.0 SPDX notices. lwext4's
[top-level license](https://github.com/gkostka/lwext4/blob/58bcf89a121b72d4fb66334f1693d3b30e4cb9c5/LICENSE)
is GPLv2 with file-specific exceptions; its
[extent](https://github.com/gkostka/lwext4/blob/58bcf89a121b72d4fb66334f1693d3b30e4cb9c5/src/ext4_extent.c)
and [xattr](https://github.com/gkostka/lwext4/blob/58bcf89a121b72d4fb66334f1693d3b30e4cb9c5/src/ext4_xattr.c)
files explicitly say GPLv2-or-later, while the inspected block/config headers
carry BSD-style terms. Record the actual imported file set and notices.
[e2fsprogs v1.47.2 NOTICE](https://github.com/tytso/e2fsprogs/blob/v1.47.2/NOTICE)
separately identifies utility and library licenses; using its recovery tools
does not mean those utilities must be linked into Riplix.

## Non-negotiable adoption boundaries

**Octets and C16 compatibility.** Standard ext files store octets. Existing
C16 file contents may contain cells above 255, with lengths measured in cells.
Specify how C16 reads/writes, offsets, `stat`, directory iteration, UTF-8 path
octets and native callers meet before importing applications or data. Reject
lossy conversions. Preserve such legacy files on C16 volumes until an explicit
application-aware conversion exists. An ext name limit is measured in encoded
octets, not user-perceived characters. Keep large disk inode numbers and sizes
outside narrow compatibility structures; marshal or return overflow explicitly.

**Ordering and durability.** DMA memory fences, completed NVMe writes,
filesystem-cache writeback, device-cache Flush and a committed journal are
different events. Existing read/write tests and clean reboot persistence do
not establish crash consistency. lwext4's inspected
[block-device interface](https://github.com/gkostka/lwext4/blob/58bcf89a121b72d4fb66334f1693d3b30e4cb9c5/include/ext4_blockdev.h)
has no explicit hardware Flush/FUA callback;
[cache flushing](https://github.com/gkostka/lwext4/blob/58bcf89a121b72d4fb66334f1693d3b30e4cb9c5/src/ext4_blockdev.c)
ultimately calls `bwrite`. Before writable adoption, prove how journal barriers
reach `block_flush()` and how errors propagate. Flushing every completed
device write is a possible conservative experiment, not a performance design
or an assumption that its transaction semantics are correct.

**Recovery interoperability.** Freeze a feature allowlist and exact formatting
recipe; record `dumpe2fs` feature output. Refuse unsupported features, invalid
geometry, checksums and dirty/recovery states safely. Test Linux mount/readback,
journal replay and `e2fsck` on unmounted copies. Ubuntu should be able to inspect
and repair the standard volume without custom C16 tooling. Never permit
simultaneous mounts from Ubuntu and Riplix. A read-only mount must not replay a
journal behind the caller's back: reject a dirty fixture or recover a private
copy under an explicitly writable test.

## First experiment and promotion gates

Start **off the HP**, on a disposable regular-file image, without changing the
kernel or importing code into production. Pin the candidate source and build a
small host adapter around its existing block interface. Use e2fsprogs v1.47.2
to create a 256 MiB, 4 KiB-block ext4 fixture from a known directory tree,
with an internal journal and a deliberately small profile: `has_journal`,
`extent`, `filetype`, `sparse_super`, `large_file`; clear implicit defaults and
disable lazy inode/journal initialization. Verify the actual resulting masks
before opening it. This is an experiment profile, not the accepted root format.
Optionally create an ext2 control fixture with the same tree to isolate journal
problems; neither fixture changes the installed root.

1. **Read-only interoperability:** enumerate 31- and 255-octet names, UTF-8
   paths, links, sparse files, all 256 byte values and boundary-spanning data;
   compare exact contents/metadata with Linux tools. Inject malformed lengths,
   feature bits, block addresses and short reads. Image hashes must not change.
2. **Durability experiment:** add a modeled volatile device cache with explicit
   flush completion, write reordering/torn-write and I/O-failure injection.
   Trace transaction start, journal data/commit, checkpoint and device flushes.
   Crash after each transition; recover copies with candidate replay and Linux
   tooling. Check acknowledged durable operations, reachability, allocation,
   links and stale-data exposure. Do not promise unsynced data retention.
3. **Riplix integration design:** settle mount/inode/file-operation dispatch,
   octet marshalling, error/locking/cache ownership, sync semantics and offline
   installation. Agree exact shared-source ownership before implementation.
4. **Disposable QEMU volume:** mount beside C16 root, first read-only, then
   writable only after the preceding gates; exercise package create/rename/
   replacement, executable loading, full-disk and recovery failures through
   virtio and NVMe, including 512-byte and 4 KiB sectors.
5. **Physical secondary volume, then root:** validate partition bounds, restart
   and explicit power-loss recovery before migration. Keep Ubuntu, USB recovery
   and a working C16-root boot available. An EFI rollback alone cannot restore
   a root that its older kernel cannot understand.

Select the format/profile and implementation after these results: correctness
under crash injection, existing recovery tools, porting surface, memory use,
sync latency, small-file/package workloads and maintainability all matter.
Do not use installed HP media for the first experiment.
