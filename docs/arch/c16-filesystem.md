# Ripple C16 V3 filesystem

Riplix and native Fuse-X share this Unix filesystem format. Blocks retain
1,024 C16 characters (2,048 physical octets). Block numbers, indirect pointers,
volume block counts and free-block counts are now unsigned 32-bit values.
Open-file descriptor numbers and RCC's calling convention do not change.

The default root contains 524,288 blocks: **1 GiB of physical storage**, with
1,024 inodes. Ripple keeps it in `ROOTFS.IMG` inside a 2,047 MiB FAT16 boot disk;
the small margin below 2 GiB keeps 32 KiB clusters below FAT's reserved cluster
numbers. Native Fuse-X uses the raw root image. File lengths and seek positions
count C16 characters: ordinary guest characters occupy two physical octets.

## Format

All integers are little endian. Offsets below count 16-bit cells, not octets.
Block zero begins with `RIPPLE-FUZIX-C16-V3`. The superblock is in block one,
with magic `0x7254`. Unknown formats fail validation before disk writes.

| Superblock field | Cell offset | Width in cells |
|---|---:|---:|
| Magic | 0 | 1 |
| Inode-table end block | 1 | 1 |
| Volume block count | 2 | 2 |
| Cached free-block count | 4 | 1 |
| Cached free-block addresses, 50 entries | 5 | 100 |
| Cached free-inode count | 105 | 1 |
| Cached inode numbers, 50 entries | 106 | 50 |
| Dirty/clean state | 156 | 1 |
| Reserved time-high field | 157 | 1 |
| Calendar timestamp | 158 | 2 |
| Total free blocks | 160 | 2 |
| Total free inodes | 162 | 1 |
| Reserved extent shift | 163 | 1 |

Free-list continuation blocks contain a one-cell count followed immediately by
up to 50 two-cell block numbers. Allocation and release preserve this packed
layout. Zero terminates the list; `0xffffffff` denotes an absent mapped block
inside the kernel. The image checker validates ownership independently of free
lists before repairing totals or marking the filesystem clean.

Inodes occupy 64 cells (128 octets), 16 per block. Cells 0–11 retain V2 mode,
link count, owner, group, length and timestamps. Cells 12–51 contain twenty
32-bit block addresses: seventeen direct, one single-indirect, one
double-indirect and one triple-indirect root. Cells 52–63 are reserved zeroes.
Each indirect block contains 512 addresses. Device numbers occupy the first
address field; directory entries remain 32 cells with 16-bit inode numbers and
30-character names. Inode zero remains reserved. The formatter accepts up to
65,520 inode slots, independently of volume capacity.

Logical file offsets remain signed 32-bit C-character positions. Files may have
up to `0x7fffffff` characters, including holes. The third indirect level avoids
introducing a roughly 513 MiB physical file-length ceiling when pointers widen.
Truncation handles wide logical block counts, releases all tree levels, and
zeroes retained partial tails so shrink/regrow cannot reveal old contents.

`statvfs` block counters are 32-bit. The internal `statfs` syscall requires V3's
`0x7254` tag in its third argument. Older statically linked callers receive
`EOVERFLOW` before any record is copied into their smaller buffer. Rebuild
custom userspace that needs filesystem statistics; managed applications are
rebuilt and installed normally.

## Symbolic links

Mode `0120000` (`S_IFLNK`) stores a symbolic link in an ordinary data block.
Its length is 1–511 C16 characters, excludes a terminating NUL, and the target
contains no NUL cells. No inode/superblock layout change is required. Both
kernels and offline tools must support links before using an image containing
them; older V3 kernels cannot resolve the new managed executable aliases.
Rebuild statically linked userspace together with the kernel. The previous
libc implemented `lstat` as `stat` and `readlink` as an unsupported stub;
old recursive file tools can therefore follow links unintentionally. Normal
managed builds track these SDK/header changes; custom binaries need rebuilding.

Guest syscall slots 68/69/70 implement `symlink`, `readlink`, and `lstat`.
Ordinary path lookup follows links, resolving relative targets from the link's
parent and absolute targets from the process root. Expanded paths must fit in
512 cells including NUL; at most 40 links are followed per lookup (`ELOOP`).
Intermediate links work across mount points. `lstat` and `readlink` inspect the
terminal link; unlink/rename/rmdir do not redirect a namespace mutation to its
target. `open(O_CREAT|O_EXCL)` rejects existing dangling links. Hard-linking a
symlink links its inode, without following it. Trailing slashes require a
directory. Readlink returns the copied length without appending NUL.

Managed aliases (`vi`, `mc`, `ln`, `mv`, `tcl`, `tclsh`, `sokoban`, `nslookup`)
are relative symlinks beside their canonical `/bin` executable. Installation
migrates previous copies/hard links while preserving other user-created hard
links. It never follows a symlink when replacing an installed path or looking
for an installation directory. The checker validates link storage, accounts for
its blocks and link count, and permits dangling targets and cycles. Growth and
V2 migration preserve link inodes and target text.

Fuse-X's `/mnt/host` remains a separate contract: host symlinks are hidden and
rejected even when their targets stay inside the export. Guest `symlink` in
that mount returns `EOPNOTSUPP`. A link stored on the guest disk can point to a
visible regular file/directory under `/mnt/host`; target lookup still obeys the
host mount's restrictions.

`ln -s` creates links; `ls -l` prints their targets. Plain `cp` follows a source
link, whereas `cp -P`, recursive copying (unless `-L`, or top-level `-H`) and
cross-device `mv` preserve link text. Recursive rm/find and tar creation do not
traverse symlink directories. Find supports `-type l`. USTAR stores/extracts
link targets of up to 100 characters; tar refuses extraction through symbolic
link parents and replaces a terminal symlink before extracting a regular file.

## Execution permissions

Both kernels enforce execute permission before checking an executable's format.
Even root needs at least one of the owner/group/other execute bits on a
nondirectory file; its read/write and directory-search overrides remain intact.
Other users need execute permission from their matching owner/group/other class.
`access(path, X_OK)` uses the same mode check, and symlink paths use the target's
permissions. Files with no execute bits return `EACCES`, rather than `ENOEXEC`.

The Bourne shell's text-script fallback runs only after `ENOEXEC`, so direct
paths, quoted command paths and PATH lookup all respect chmod. For example,
`chmod 644 script` prevents `./script`, while `chmod +x script` enables it.
Explicit `sh script` and `. script` read shell input and do not require an
execute bit; this is intentional. The same rules apply to visible files under
Fuse-X's `/mnt/host` mount.

`python3 ports/fuzix/tools/test_exec_permissions.py` tests both targets on private
images. It covers direct/PATH/quoted commands, chmod toggles, symbolic links,
binaries, explicit interpretation, root and ordinary-user permission classes,
`access`/`execve` error codes, host-mounted scripts and filesystem integrity.

## Storage and memory use

RVM storage addresses physical octets with 32 bits: 4 GiB is the transport's
address-space ceiling, independently of filesystem pointer width. FAT's
32-bit file length and outer volume geometry further constrain an embedded
root. The present 1 GiB default needs no ISA, MMIO, compiler-ABI or VM changes.
Larger defaults require auditing those enclosing limits as well.

Ripple's validated FAT chain lives in supervisor banks 98–99, separate from
kernel GP and process mappings. It supports every usable FAT16 cluster rather
than the former 16,384-entry limit. Guest disk transfers still split at FAT
cluster and 64 KiB storage-window boundaries and retain DMA/scalar fallbacks.

Host image tools use sparse, page-based copy-on-write images. File reads have a
bounded cache; whole images are not loaded into JavaScript buffers. New images
are truncated to their logical length and only populated pages are written.
Updates clone the existing backing file when available, then write changed
pages. The atomic writer flushes the temporary image, renames it, and flushes
the parent directory. Backups precede replacement. Guest sync and shutdown keep
the existing host flush barriers. This format does not add journaling.

## Upgrading

Normal Riplix installation recognizes C16 V2, validates or repairs its shutdown
metadata under the image lock, and converts it to V3 before installing rebuilt
programs. Conversion preserves inode identities, hard links, device numbers,
permissions, timestamps, native characters and sparse mappings. It repacks data
and pointer blocks; it does not reinterpret old pointer tables in place.
The previous outer disk is retained as `.bak` or `.bak.N`. Roots with ambiguous
ownership or directory damage are refused. Unexpected outer FAT entries are
also refused during repacking, rather than discarded. C16 V1 still requires a
fresh image. `--run-only` performs no upgrade; use a normal launch first.
Native `image.mjs install` and `extract` use the same conversion logic.

## Verification

`node --test ports/fuzix/tools/disk.test.mjs` covers V2 conversion, native cell
preservation, hard links, devices, sparse and high block mappings, copy-on-write
image snapshots, backups, no-op identity, metadata repair and corruption refusal.
The V2 fixture was generated by the previous committed V2 formatter.

`make -C ports/fuzix BUILD="$PWD/build/fuzix" test-largefs` builds a private
probe and uses JIT guests. It writes 70,000 real data blocks, confirms physical
allocation above block 65,535, crosses direct/single/double/triple boundaries,
checks sparse holes and the signed offset boundary, reboots and rereads data,
then tests wide truncation, zeroed regrowth, open-file unlink and complete block
reclamation. It also boots a 1,001 MiB root with 64,064 FAT clusters and reads
its final cluster through the second supervisor cache bank. Offline checks follow
each clean shutdown. Normal kernel, graphics, job-control and application tests
use disposable V3 images too.

`python3 ports/fuzix/tools/test_symlinks.py` runs syscall/utility acceptance on
both kernels using disposable images, including real `vi` alias editing,
relative/absolute/dangling/cyclic targets, creation through a link, namespace
operations, copy/delete/archive traversal, hidden host links and reboot
persistence. Use `--os fuzix` or `--os fuse-x` to select one target.
