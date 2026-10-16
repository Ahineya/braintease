# Fuse-X storage

Fuse-X needs persistent system storage and a convenient way to exchange files
with macOS. **A root disk image plus selected shared host folders is the current
proposal**, rather than a settled filesystem implementation.

## Root disk image

Start by adapting the existing block-storage model to a host-backed image. This
lets FUZIX retain its filesystem, system files, application installation, and
guest device-file semantics. It also keeps a portable image usable with the
Ripple port and existing image tooling, subject to executable-format differences.

The hosted storage adapter must define block/octet conversion, error reporting,
flush behavior, and orderly shutdown. Give a writable image one session owner;
opening it from two independent kernels must not silently create competing
filesystem caches.

Native storage MMIO is currently unavailable. Existing native C file streams
are building blocks for host I/O, but do not implement the kernel's block-device
interface or make a Mac directory into a guest mount.

## Shared Mac folders

The proposed UI lets a person choose a Mac directory and expose it at a guest
mount point, with read-only or read/write access. Example layout:

| Guest path | Backing storage |
| --- | --- |
| `/` | FUZIX disk image |
| `/mnt/host/projects` | An explicitly selected Mac projects directory |

This requires a filesystem bridge for open, read, write, seek, directory
enumeration, metadata, creation, removal, and rename operations. Host references
stay behind opaque handles; guest pointers and native host pointers have
different representations.

Specify the following before enabling general read/write mounts:

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

Choose the FUZIX filesystem integration point before committing to a bridge
protocol. Begin with a narrow read-only directory/file subset if that makes the
semantics easier to validate, then add writes with explicit metadata rules.
Whether host folders should eventually back more of the guest tree remains
open; the first boot should not depend on solving that question.

Keep the root image and shared-folder implementation independently testable.
Image persistence should survive a restart; host-folder tests should verify
that the same file is visible from both macOS and Fuse-X.
