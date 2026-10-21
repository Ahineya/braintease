# Riplix package management

Decision accepted 2026-10-20: build and publish applications independently, then
install them from inside Riplix or Fuse-X. Use the Mac mini as a static package
repository. Package creation and repository indexing must also be possible in
the guest; Python is not part of their runtime or package-format contract.

## Targets and implementation

Use one portable package engine for macOS, Ripple/RVM, freestanding x64 and
Fuse-X. RFL provides the command frontend where its application build is
available; portable C supplies streaming archive, hashing and filesystem
operations and an equivalent bootstrap CLI. The first HP client can use that
C entry while freestanding RFL packaging is validated. Existing host build
drivers can invoke the tools without owning their serialization logic.

Identify packages by OS, ISA, executable/runtime ABI and library requirements,
not CPU name alone. Initial identities distinguish `riplix-x64-c16`,
`riplix-ripple-c16`, and `fuse-x-arm64-macos-c16`. LP64 is a future distinct
target. Architecture-independent resources may use `any`; executable scripts
still need their interpreter dependencies. One logical file octet is always
eight bits on the wire, even when the guest stores it in a C16 cell.

Package creation consumes a manifest and a staging directory; compilation is
a separate recipe step. A package made inside a guest must be byte-identical
to the equivalent host package. Keep source build dependencies separate from
installed runtime dependencies. Begin with exact dependency versions and
explicit errors rather than a general-purpose constraint solver. Catalog collections
and independent data packages may use `name@*` to accept the current version;
content-versioned native libraries retain exact dependencies.

## Layout and ownership

Default executable search should become `/usr/bin:/bin`, leaving explicit
`/bin/sh` and recovery commands available. Apply it consistently to normal and
remote login environments and libc fallback search, coordinated with init and
service owners. Existing shells keep their environment until changed.

| Path | Purpose |
| --- | --- |
| `/bin`, `/sbin` | Base boot and recovery commands |
| `/usr/bin`, `/usr/libexec` | Packaged commands and private helper programs |
| `/usr/share` | Manuals and application resources |
| `/usr/local/bin` | Unmanaged personal commands |
| `/etc/pkg` | Repository configuration, target identity and trusted public key |
| `/var/lib/pkg` | Installed metadata, owned payloads and pending transaction |
| `/var/cache/pkg` | Bounded downloads and repository metadata |

Keep existing `/lib/fuse-x` and `/lib/riplix` seed paths for base executables.
Packaged Fuse-X apps use an isolated `/usr/lib/fxp/<content>/` closure and unique
content-derived loader names. Rebinding changes dependency metadata while
preserving native code, initializers and export interfaces. Old running apps
keep their loaded code; relaunch uses the new closure. The catalog never
replaces a live seeded libc under the same loader-cache identity.
Freestanding x64's static packages remain a separate target profile.

The host seed installer and guest manager must share file-ownership policy.
Host rebuilds must not silently overwrite guest-managed packages. Start by
installing optional applications into `/usr`; migrate existing `/bin` paths
through explicit package ownership rather than leaving accidental duplicates.
Do not alter the kernel/init/service interfaces as part of package delivery.

## Package and repository behavior

Start with a bounded, uncompressed stream: a versioned manifest, per-file
lengths/modes/SHA-256 and regular-file payloads. Serialize integers explicitly;
never write native structs. Preserve all binary octets and reject truncation,
duplicate paths, parent traversal, special files and archive-controlled links.
Use deterministic ordering and omit timestamps from the content identity.
Respect the guest's current filename and file-size limits.

Track package-owned files, refuse conflicts and check dependencies before
committing. Keep verified payloads and transaction state on the installation
filesystem. Interrupted operations must be recoverable and repeated operations
must have defined behavior. Per-file rename is not a whole-system atomic
transaction, and crash durability depends on the filesystem's actual guarantees.
Preserve existing `/etc` configuration on adoption, and update a live default
only if it still matches the previous package default. Keep edited files on
upgrade and configuration on removal. Explicit seed migration snapshots
original regular files/simple aliases before installing owned links. Allow
legacy `/bin` forwarding entries only with a corresponding `/usr/bin` entry;
protect `/bin/sh`, `/bin/svcd` and `/bin/console-session` as base boot services.
There are no maintainer scripts or whole-base replacement hooks.

The repository uses immutable payload identities and a versioned index. Publish
payloads before publishing their index. Authenticate the index with a pinned
public key and bind each package through its full SHA-256 and byte length.
HTTP can transport authenticated content while guest HTTPS support is being
integrated; unsigned network installs must never be silently trusted. Repository
signing is a separate publisher role, not a requirement for creating a local
package. Keep signing private keys outside the served tree.

Support local files and shared mounts before making networking mandatory.
Initial commands cover create, inspect, install, list, info, remove, recovery,
repository indexing and network refresh/install. Dependency-driven upgrades
follow the same ownership and verification paths. Keep only bounded download
caches and explicitly retained previous versions; avoid unbounded `.bak` files.

## Mac mini repository

Host: `mini.local`. Dedicated root: `/Volumes/Ahineya/RiplixRepo`, with only its
`public` subdirectory served. The external SSD is a 1 TB APFS volume, UUID
`E3129F95-3ACD-4BD1-B261-35D449BB4A1D`. Verify the UUID and mount before setup,
publication and service start. Never create a replacement mount-point directory
on the internal disk if the SSD is absent.

Use the existing Caddy installation as a read-only static server on a dedicated
port, managed by a separately named launchd job. Preserve unrelated services.
Limit access/error logs and keep configuration outside the public payload tree.
The first service is a user LaunchAgent; its login-session lifecycle must be
reported honestly rather than advertised as a boot-time system daemon.

Initial inspection found normal Unix permissions but SSH directory access was
denied by macOS privacy controls. After the user enabled Remote Login's full
disk access setting, a fresh SSH connection could read the volume. Do not
replace this with broad chmod/chown changes. Validate the actual launchd
process's access separately from SSH access.

## Acceptance stages

1. Shared host/guest engine: deterministic creation, inspection, hashes, checked
   paths, independent binary installation, ownership, upgrades/removal and
   interrupted-operation recovery on disposable roots. Compare with standard
   SHA-256 oracles and corrupt/truncated package fixtures.
2. Guest execution: create packages inside Fuse-X, compare exact host bytes,
   install and run an app from `/usr/bin`, then remove it. Validate the x64
   build independently without editing that owner's runtime/boot sources.
3. Repository: provision the dedicated SSD directory, verify launchd restart
   and actual HTTP access, publish a real package/index and verify target/key/
   digest checks. Do not expose private keys or unrelated SSD files.
4. Distribution: network refresh/install, dependency upgrades, controlled cache
   retention and shared seed ownership. Kernel/base upgrades, config-file
   merging, compression, delta downloads, native LP64 targets and a guest Rust
   toolchain remain later work.

Implementation coverage and exact commands belong with the package tools.
An accepted plan or a reachable empty server is not a completed guest package
installation milestone.


## Implementation checkpoint — 2026-10-20

The first working implementation is in [`ports/pkg`](../../ports/pkg/README.md),
with an RFL frontend under `rfl/apps/pkg` and publisher/service scripts under
`tools/packages`. The same engine has passed host, native Fuse-X and actual
freestanding x64/QEMU package install/execute/remove checks. Fuse-X also passed
signed downloads and installation of the package manager itself through its
TCP stack; both guest targets verified signatures in software.

The mini's SSD service is live at `http://mini.local:8088/` (LAN address
`192.168.100.179`). Its dedicated user LaunchAgent is `org.riplix.packages`.
Caddy needed its own macOS removable-volume/full-disk permission after Remote
Login was enabled; both permissions are now verified through actual HTTP file
reads. The private signing key remains outside the served tree.

The application catalog includes the built Fuse-X programs (RFL tools,
browsers/chat, GUI apps, games and compiler tools) and the supported x64 C app
collection. Resources, licenses, certificates and matching shared dependencies
are separate packages. Content identities make repeated catalog generation
stable; publication transfers changed packages only. `update.py` can build and
publish named applications without compiling a kernel or changing an OS disk.

Global PATH order is now `/usr/bin:/bin` in init, remote login and libc fallback.
The shared host seed installer preserves package-owned paths on later OS
rebuilds. Normal disks are still unchanged: migration is an explicit guest
operation. Re-run the refreshed bootstrap before using the full index with an
old 0.1.x client. Commands, limits and test coverage are in the tool README.
Physical HP execution remains separate from disposable x64/QEMU acceptance.
