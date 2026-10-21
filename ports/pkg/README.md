# pkg: Riplix and Fuse-X packages

`pkg` creates, inspects, installs and removes packages on both the host and the
guest. It can refresh a signed repository and install named packages with exact
runtime dependencies. The same C implementation owns the archive format,
streaming SHA-256, metadata and filesystem transactions on every target.
The RFL frontend forwards literal arguments to the companion engine. The x64
bootstrap uses the equivalent C CLI directly.

## Try it in Fuse-X

The prepared bundle is in the normal host share. Inside the guest:

```sh
sh /mnt/host/packages/bootstrap/fuse-x/install.sh
export PATH=/usr/bin:/bin
pkg add pkghello
pkghello
pkg list
pkg info pkghello
pkg remove pkghello
pkg clean
```

The bootstrap writes `/etc/pkg/repo.pub` and `/etc/pkg/repo.conf` if absent,
verifies the repository and installs `pkg` and the `test`/`[` commands under `/usr`. It refuses to replace
a different pinned key. Pass a URL as the script's first argument if the mini's
LAN address changes. The initial guest URL is `http://192.168.100.179:8088`;
`mini.local` works on the Mac host but guest DNS need not support mDNS.

`fuse-x-shared/packages/bootstrap/x64` contains the x64 engine and the same
installer. Copy that folder through a supported guest filesystem/transfer path
and run its installer inside x64 Riplix. Never use the Fuse-X engine on x64 or
vice versa. No HP disk is modified by the build or tests here.

The normal OS disks have not been reimaged or edited. New login/default PATHs
use `/usr/bin:/bin`; existing shells need the export above. To take ownership
of an existing seed application and its resources explicitly:

```sh
pkg update
pkg search
pkg --adopt-seed add rift
pkg --adopt-seed add chat
pkg --adopt-seed add netsurf
```

Use `--adopt-seed` on first migration, including dependencies with existing
seed data. Original regular files and simple `/bin` aliases are snapshotted in
the package store; customized `/etc` files remain in place. Subsequent updates
use `pkg update` then `pkg add NAME` (the same command installs or updates).
`update` refreshes metadata; it does not upgrade every program by itself.

The first small `0.1.x` client must be bootstrapped again from the refreshed
shared folder before using the full catalog: its smaller index bound cannot
read the new catalog. The bootstrap installs the new manager and its libraries.
No OS rebuild is needed for that transition.

Application names, including aliases such as `vi` inside `rift`, appear in
`build/pkg/catalog/<target>/catalog.json`. Collections include `gui-apps`,
`unix-apps`, `developer-apps`, `c-toolchain` (Fuse-X) and `apps`. Installing
`apps` selects the entire target collection and can use hundreds of MiB;
individual app installation is usually more useful on an existing disk.

## Build and publish

Host tools, including an optional native macOS RFL frontend:

```sh
sh ports/pkg/build.sh
python3 rfl/tools/build_os.py rfl/apps/pkg/main.rfl --os macos -o build/pkg/host/pkg
```

On the current development Mac, use
`SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk` if the default
SDK cannot link with the installed toolchain.

Build both guest engines and the Fuse-X RFL frontend:

```sh
python3 ports/pkg/build_guest.py
python3 ports/pkg/build_guest.py --target x64
python3 rfl/tools/build_os.py rfl/apps/pkg/main.rfl --os fuse-x -o build/pkg/pkg.fxe --build-dir build/pkg/fuse-sdk
sh tools/packages/prepare.sh
sh tools/packages/bootstrap-bundle.sh
```

The Python scripts are existing compiler orchestration plus a small host build
driver. Creating a package, indexing it, downloading it and installing it do not
require Python. Neither building applications nor building the OS is part of
`pkg install` or `pkg add`.

`prepare.sh` makes small bootstrap/test fixtures. For the full catalog use:

```sh
python3 tools/packages/update.py --no-build
python3 tools/packages/update.py --target x64 --no-build
python3 tools/packages/publish-catalog.py build/pkg/catalog/fuse-x build/pkg/catalog/x64
```

The catalog includes all built, non-test application artifacts under `apps/`,
including RFL apps, native GUI apps and individual puzzle games. It reuses the
OS installers' public data producers, includes manuals, certificates, themes,
BASIC examples, make support files and the available C compiler/SDK. It excludes
diagnostic executables and the retired combined puzzle launcher. Never copies
personal credentials, history, custom instructions or host-share contents.

To build and publish a changed program without a kernel or disk build:

```sh
python3 tools/packages/update.py chat --publish
python3 tools/packages/update.py rift --publish
python3 tools/packages/update.py gterm nxeyes --publish
python3 tools/packages/update.py --target x64 cp --publish
```

Application builders keep their incremental caches. The x64 command uses the
existing offline C application builder; it does not imply every Fuse-X/RFL/GUI
app is ported to x64. Override `--build DIR` to package another matching build.
Unchanged archives are reused and the publisher transfers only changed
name/target/hash identities. Versions are deterministic content fingerprints;
repackaging identical artifacts produces no update.

The engine embeds libc/verification code for reliable bootstrap. The catalog
rebinds Fuse-X shared dependencies into content-versioned `/usr/lib/fxp` paths
and unique loader names. It preserves all native code/initializer payload bytes
and interface hashes, recursively includes the matching library closure, and
avoids replacing `/lib/fuse-x` or reusing an older live loader cache entry.
Existing processes continue with their loaded code; restart an app to use its
update. x64 packages remain ordinary static RXE1 executables.

The native host engine uses system OpenSSL for RSA verification. Guest builds
reuse wolfSSL's public RSA verifier and its optional crypto-device adapter,
including software fallback.

The mini publisher retains its private signing key under
`/Volumes/Ahineya/RiplixRepo/private/signing.pem`, mode 0600. It never downloads
that key. `tools/packages/repo/mini.pub` was obtained through authenticated SSH;
this is the bootstrap trust anchor. Downloading a replacement public key over
HTTP would not establish trust.

## Create packages inside the guest

A staging directory contains the eventual relative installation paths:

```sh
mkdir -p /tmp/stage/usr/bin
cp /path/to/my-program /tmp/stage/usr/bin/my-program
chmod 755 /tmp/stage/usr/bin/my-program
cat > /tmp/app.manifest <<'MANIFEST'
name=my-program
version=1
target=fuse-x-arm64-macos-c16
depends=
MANIFEST
```

Use `riplix-x64-c16` for x64 executables and `any` only for portable data/scripts.
Then:

```sh
pkg create /tmp/app.manifest /tmp/stage /tmp/my-program.rpk
pkg inspect /tmp/my-program.rpk
pkg install /tmp/my-program.rpk
```

The output archive must be outside the staging directory. Runtime dependencies
use exact identities such as `depends=library@1,other@2` or `name@*` for a
resource/application whose current repository version is acceptable. Immutable
native-library dependencies use exact identities; collections use wildcards.
`/bin/sh`, `/bin/svcd` and `/bin/console-session` remain protected boot services;
packaging their commands under `/usr/bin` does not replace those base entries.

A local `pkg install FILE.rpk` explicitly trusts that file: it validates format,
paths and content hashes, but does not establish publisher authenticity.
`pkg add NAME` uses the signed repository index for that authentication.

The host uses identical commands. Give installation commands a disposable root
with `--root /absolute/real/directory`; use `--target ABI` when testing a guest
package from the host. `PKG_ROOT`, `PKG_TARGET` and the RFL frontend's
`PKG_ENGINE` are explicit overrides. An example host invocation is
`build/pkg/host/pkg --root /private/tmp/pkg-root --target fuse-x-arm64-macos-c16 list`.

## Format and update behavior

`RIPKG1` is an uncompressed ASCII header followed by sorted regular-file records
and exact-length binary payloads. All wire data uses eight-bit octets, including
on C16 guests. No native structs, timestamps, archive symlinks, device nodes,
maintainer hooks or arbitrary installation scripts are accepted. The inspector
checks file modes, paths, lengths and SHA-256 before installation.

Current explicit bounds: 128 files/package, 16 directory levels when creating,
30 characters/component, 255 characters/relative path, 512 MiB total payload
(and a 512 MiB archive read/download cap), 1024 repository entries, a 1 MiB
index, and 16 dependency levels. Metadata and payload overhead count toward the
archive cap. Package names are at most 24 characters. These are initial
implementation bounds rather than hardware limits.

Installed payloads live in content-identified directories under
`/var/lib/pkg/objects`; `/usr` entries are owned relative symlinks into them.
The full 256-bit digest is checked even though object directory names use a
24-hex prefix to respect guest filenames. Unmanaged files and other packages'
paths are never silently overwritten. Installable prefixes are `/usr/bin`,
`/usr/libexec`, `/usr/lib`, `/usr/include`, `/usr/man`, `/usr/src`, `/usr/share`,
`/lib/fuse-x`, `/lib/riplix` and `/etc`. A `/bin/NAME` path is permitted only
with the same package's `/usr/bin/NAME`, excluding the protected boot commands.
Catalog `/bin` entries forward absolute legacy launches to the managed command;
aliases preserve their invoked name. OS seed rebuilds consult `/var/lib/pkg/db`
and preserve package-owned paths, including resources/configuration.

`/etc` defaults are regular copies. Existing configurations survive initial
installation. Later defaults replace a live file only when it still matches
that package's previous default exactly; edited files and links survive.
Removing a package leaves its configuration in place. Defaults remain available
in its verified object payload; no automatic text merge or post-install hooks
are performed.

An update first verifies and stages the payload, prepares a transaction, then
replaces individual links and commits metadata. `pkg recover` resumes a prepared
transaction after process interruption and removes orphaned extraction staging. This is not a whole-system atomic
transaction or a claim of power-loss durability on every filesystem. Packages
in a dependency chain commit individually; exact-version conflicts fail rather
than guessing a coordinated upgrade solution. A package's previous record is
retained; `pkg clean` keeps current plus one previous payload for installed
packages and releases uninstalled/unreferenced payloads. Add
`--discard-previous` before `clean` to explicitly release retained old versions
and seed snapshots. Cleanup also removes unused generated `fxlib-<hash>` and
`bundle-<hash>` support packages, preserving dependencies of retained previous
records. These names are reserved for catalog support packages. Recovery is forward
completion, not a user-facing rollback command.

`RIPINDEX1` has one active version per name/target. The publisher signs its exact
bytes using RSA-2048/PKCS#1 v1.5/SHA-256. Clients verify the signature using the
pinned modulus/exponent, then match every downloaded archive's full SHA-256,
length, target, identity and dependency metadata. Authentication covers content;
HTTP does not hide traffic. This first protocol has no expiration or monotonic
anti-replay counter. Keep the key pinned and use a trusted publisher.

The HTTP client handles bounded static files with Content-Length. Guest
transport uses IPv4; the conventional host also supports scoped IPv6 and tries
the addresses supplied by the system resolver. It
rejects redirects, chunked/compressed bodies and missing lengths. Caddy serves
this profile directly. Index/signature publication uses separate renames: a
refresh racing publication may fail signature verification; repeating update
gets the matching pair. A failed refresh leaves the last verified cache usable.
The cache contains current and previous indices, one staging set and at most
one bounded package download, rather than accumulating timestamped backups.

## Verification

```sh
python3 ports/pkg/tests/test_host.py
python3 ports/pkg/tests/test_guest.py
node ports/pkg/tests/test_aliases.mjs
python3 ports/pkg/tests/test_x64.py
python3 ports/pkg/tests/test_catalog.py
python3 ports/pkg/tests/test_catalog.py --catalog build/pkg/catalog/x64/catalog.json
python3 ports/pkg/tests/test_catalog_guest.py
sh tools/packages/repo/check-mini.sh
```

Host tests use disposable roots, SHA-256/OpenSSL oracles, UBSan, malformed input,
conflicts, dependencies, upgrades, cleanup, recovery checkpoints and signed HTTP
tamper tests. The Fuse-X test boots a private disk, compares host/guest package
bytes, installs/runs/removes an app, verifies signatures in default and forced-software modes, and downloads/installs the actual package manager through an
isolated Ethernet fixture. It exercises the actual bootstrap script from a
fresh configuration and repeats it with the existing pin/configuration. The x64 test reuses an existing normal boot image,
creates a private expanded root, drives the shell through PS/2/QMP, executes the
RXE engine, verifies signatures and checks the final filesystem. It does not
rebuild or alter x64 sources, normal disks or firmware.

The live mini check verifies signed refresh/install/run/remove on a disposable
host root. The initial published catalog contains 190 Fuse-X applications and 109 x64 C
applications (477 repository entries including support packages and pkghello).
Catalog checks install every package on disposable host roots,
validate all dependency edges and unchanged native payloads, and exercise the
host seed ownership guard on a real C16 filesystem. The catalog guest check
uses a signed full index over isolated guest Ethernet, migrates existing
commands, runs packaged BASIC/Tcl and updates the manager through its isolated
library closure. These checks do not claim physical HP execution or exhaustive
runtime testing of every application.

The bootstrap uses `test` directly and does not require an existing `[` command.
Normal Fuse-X and Ripple seed images provide `/bin/[` as a symlink to `test`;
the x64 image producer already supplies that alias. The `test` package includes
both executable names, and the bootstrap adopts it so existing sessions gain
`[` without an OS rebuild. The package catalog explicitly retains the actual
`test` utility while excluding diagnostic programs.
