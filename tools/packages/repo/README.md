# Mac mini package repository

The dedicated user LaunchAgent is `org.riplix.packages` in `gui/501`. It starts
Caddy on TCP 8088 while ahineya's GUI login session exists. It is not a system
boot daemon. Public files are under `/Volumes/Ahineya/RiplixRepo/public`; signing
keys, incoming packages and the current source catalog are private siblings.
Every setup/publication/service start verifies the external SSD UUID before
using it. The service never creates a substitute mount point on the system disk.

```sh
sh tools/packages/repo/setup-mini.sh
python3 tools/packages/publish-catalog.py build/pkg/catalog/fuse-x build/pkg/catalog/x64
sh tools/packages/repo/check-mini.sh
```

Setup preserves other launchd jobs and existing SSD contents. Caddy and Remote
Login each need their macOS removable-volume permission. The initial deployment
was verified after the user enabled both. Configuration lives in
`~/.local/share/riplix-repo` and the plist in `~/Library/LaunchAgents`.

Publication uses SSH, an exclusive directory lock, private staging and the
portable package engine. A private RSA key is created once on the mini. All
archives are inspected, objects published before the signed index, and public
object retention is bounded by the current and previous index. The private
catalog keeps one active archive per name/target. Unreferenced generated library
and grouping packages are pruned from the current catalog; public objects still
retain the previous index for in-flight downloads. No public write/upload API is
provided. A process killed during publication may leave `private/publish.lock`;
inspect the operation before removing that lock or its private staging.

The pinned public modulus in `mini.pub` was retrieved through authenticated
SSH. `repo.pub` is also downloadable for inspection; HTTP alone must not be used
to establish a new trust anchor. Guest clients use the LAN IP (initially
192.168.100.179), because their DNS resolver need not understand `mini.local`.

Caddy rotates its access/error logs at 5 MiB, retaining three rolled files for
at most seven days. The small launchd diagnostic file is capped at service
start at 1 MiB plus one retained copy. The configuration serves only public
files, without directory browsing or the admin API. This HTTP service is for
the LAN; no router forwarding or public exposure was configured.

To restart only this job:

```sh
ssh mini.local launchctl kickstart -k gui/501/org.riplix.packages
```

Full implementation, guest bootstrap and format details:
[`../../../ports/pkg/README.md`](../../../ports/pkg/README.md).
