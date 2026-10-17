# Fuse-X storage

Fuse-X needs persistent system storage and a convenient way to exchange files
with macOS. The implemented model is **a root disk image plus one read/write host folder**, mounted for the lifetime of the session.

## Root disk image

The hosted adapter uses the existing block-storage model with a host-backed image. This
lets FUZIX retain its filesystem, system files, application installation, and
guest device-file semantics. It also keeps a portable image usable with the
Ripple port and existing image tooling, subject to executable-format differences.

The hosted default uses 524,288 blocks: a sparse **1 GiB C16 V3** root with
1,024 inodes. Installation expands smaller roots and converts C16 V2 while
retaining inode identities, metadata, native cells and hard links, with a backup
before replacement. V3 uses 32-bit block addresses, three indirect levels, and
wide statistics/truncation. Host tools use bounded caches and changed-page
updates. See the [shared filesystem specification](../arch/c16-filesystem.md).
Ordinary file octets occupy 16-bit cells on this image; host-folder files
retain their normal octet layout.

The hosted storage adapter must define block/octet conversion, error reporting,
flush behavior, and orderly shutdown. Give a writable image one session owner;
native kernels use an advisory host lock so two writable sessions cannot
silently create competing filesystem caches. The launcher also rejects images
already open by another process when host `lsof` is available. Offline image
editors must still be used only while the kernel is stopped.

Native storage MMIO is currently unavailable. The hosted kernel instead uses
native C streams for block I/O, with explicit C16/octet conversion. Folder mounts
use a separate native filesystem service and do not masquerade as a block image.

## Shared Mac folders

The launcher creates the gitignored `fuse-x-shared/` folder at the repo root and
mounts it read/write at `/mnt/host` by default. Use `--share HOST_DIRECTORY` to
select another folder or `--no-share` to disable the mount. An explicit path can
contain spaces and is resolved relative to the caller's working directory.
Check modes leave the default folder unmounted. Example layout:

| Guest path | Backing storage |
| --- | --- |
| `/` | FUZIX disk image |
| `/mnt/host` | Repo-root `fuse-x-shared/`, or the directory selected with `--share` |

The bridge implements open/read/write/seek, directory enumeration, creation and
removal, hard links, atomic rename, truncation, chmod and timestamps. Native
Fuse-X packages can execute directly from the export. FUZIX retains path walking,
mount traversal, the inode/open-file tables, shared offsets across dup/fork,
close-on-exec and descriptor cleanup. Build-generated hooks adapt six canonical
kernel units without editing `ports/fuzix/`; generation fails if a hook's source
contract changes. Host references stay behind opaque handles.

Host file contents use one octet per guest cell, including binary data. Host-side
file creation and replacement are visible on subsequent lookups. An open file
retains its identity across rename/unlink, as on the host. Directory scans use
snapshots; a fresh scan sees current entries. Empty-directory checks always use
the current host directory. Closing writable handles and guest sync flush file
data through host sync operations; this is not a claim of crash recovery.

The initial contract is:

- One export, at `/mnt/host`, configured only at launch. Its mountpoint is created
  through normal FUZIX mkdir if absent. The mapping is not stored in the root image.
- Regular files and directories only. Symlinks, FIFOs, sockets and devices are
  not followed or exposed. Directory traversal uses opened directory handles
  and refuses directory handles that have moved outside the selected export.
- Filename components are at most 30 host octets. Longer names are omitted from
  listings and direct access returns `ENAMETOOLONG`; they are never truncated to
  another file's name. Host filename bytes and case behavior are preserved.
- Host user permissions apply, and lookups currently require read permission.
  Guest uid/gid are reported as zero. chmod maps ordinary permission bits;
  setuid/setgid and host ownership changes are not provided.
- Signed 32-bit file offsets apply; existing FUZIX syscall limits still apply
  to truncation. Host identifiers map to bounded 16-bit guest inode numbers.
- Cross-filesystem rename/link return `EXDEV`. Host filesystem capacity reporting
  returns unsupported. Multiple exports, read-only policy selection and live
  unmount controls are follow-up work.

Further compatibility work includes:

- Filename encoding, case sensitivity, and guest name/size limits.
- Ownership, permissions, executable bits, and timestamps.
- Symbolic and hard links, including links that leave the exported directory.
- Visibility of changes made by Mac applications, plus cache invalidation.
- Rename, unlink of open files, error translation, and flush guarantees.
- Mount removal while files are open and the scope of the exported directory.

Use explicit octet conversion at host boundaries. The compiler's 16-bit C byte
does not change the representation of bytes in ordinary Mac files.

If the host application adopts App Sandbox, directory selection and persistent
access can use open panels and security-scoped bookmarks. The selected-folder
model is compatible with Apple's
[sandbox file-access APIs](https://developer.apple.com/documentation/security/accessing-files-from-the-macos-app-sandbox).

## Implementation questions

The current integration dispatches host-backed inode and namespace operations
through generated kernel hooks. Further work can broaden metadata compatibility,
read-only export policies and mount configuration without changing file contents
into a disk-image format.
Whether host folders should eventually back more of the guest tree remains
open; boot continues to use the root disk image.

Keep the root image and shared-folder implementation independently testable.
Image persistence should survive a restart; host-folder tests should verify
that the same file is visible from both macOS and Fuse-X.

`fuse-x/test_hostfs.py` uses a disposable export and root image to exercise
read/write binary data, sparse/truncated files, fork/dup, rename/unlink of open
files, directories, shared-folder exec, live host replacement, path boundaries
and a later reboot without the export. Runtime unit checks also cover host
replacement and moved-directory/symlink rejection.
