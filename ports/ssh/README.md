# Guest SSH tools

This is an explicit, experimental wolfSSH server and client port for Riplix and Fuse-X.
SSH framing, authentication, session keys, encryption and SFTP execute in the
guest. The existing entropy and stateless crypto devices supply randomness and
accelerate supported operations; there is no host SSH proxy.

Both Fuse-X native and RVM JIT have passed real OpenSSH authentication,
remote-command, PTY/resize/Ctrl-C, rekey, binary transfer, ordinary `scp` and
SFTP directory-operation checks. This is not yet an enabled system service or a
security-audited general-purpose sshd.

## Build explicitly

From the repository root, with the corresponding SDK already available:

```sh
python3 ports/ssh/app.py
python3 ports/ssh/app.py --target ripple
```

Outputs are `build/ssh/sshd.fxe` and `build/ssh/sshd.rxe`. Library IR compilation
is incremental and parallel (`-j 3` by default). Neither command changes an
image, installs `/bin/sshd`, enables a service, or adds work to the ordinary OS
launcher. The first Ripple build prepares its SDK and links a roughly 17 MiB
server RXE2 executable; client tools are larger because they include private-key
import/signing support. Crypto is statically linked.

`--sdk`, `--compiler`, `--build` and `-o` override their usual locations. The
native target uses the existing Fuse-X C16 execution model. The separate
`tools/x86_64/ssh_apps.py` packager produces freestanding x64 RXE1 programs.
The SFTP mount now has real x64/QEMU acceptance using hardware entropy and
software crypto; this does not imply physical Wi-Fi or SSH-server acceptance.

## First login

Generate a separate **ECDSA P-256** client key on the host:

```sh
ssh-keygen -t ecdsa -b 256 -f ~/.ssh/id_riplix
```

Copy only its `.pub` file to `fuse-x-shared/ssh/authorized_keys`, and stage the
executable with:

```sh
python3 ports/ssh/stage.py
```

In the guest, explicitly install and configure it:

```sh
cp /mnt/host/ssh/sshd.fxe /bin/sshd
mkdir /etc/ssh
mkdir /root/.ssh
chmod 700 /root/.ssh
cp /mnt/host/ssh/authorized_keys /root/.ssh/authorized_keys
chmod 600 /root/.ssh/authorized_keys
sshd --keygen /etc/ssh/hostkey.der
sshd
```

Use `sshd &` for manual background operation. Key generation creates a private
0600 file with exclusive creation and synchronous writes; it refuses to replace
an existing key. Keep that key in the guest filesystem across sessions.
Do not copy a private client key into the guest or reuse test fixture keys.

From another machine with a route to the guest:

```sh
ssh -i ~/.ssh/id_riplix root@GUEST_IP
scp -i ~/.ssh/id_riplix local-file root@GUEST_IP:/root/
sftp -i ~/.ssh/id_riplix root@GUEST_IP
```

An incoming route or a TCP port forward depends on the networking backend; this
port does not create one. `-p 2222` changes the server port; SSH/SFTP use `-p`
and scp uses `-P` for the corresponding client option. Verify the host key when
accepting the first connection. The acceptance tests pin a known host key and
never disable host-key verification.

## Guest key and client tools

Build the complete opt-in set, for either target:

```sh
python3 ports/ssh/app.py --app all
python3 ports/ssh/app.py --app all --target ripple
python3 ports/ssh/stage.py --all
```

Individual names are `sshd`, `ssh-keygen`, `ssh`, `sftp`, `scp`, and
`sftp-mount` via `--app`. The mount helper is native-only in this entry point;
Ripple `--app all` retains the five existing SSH tools.
Client crypto has a separate build profile/cache under `build/ssh/client`; the
server transport and the existing TLS profile retain their settings. The server
accepts ECDSA P-256/384/521, Ed25519 and RSA user keys. RSA signatures use
RSA-SHA2-256/512; legacy SHA1 `ssh-rsa` signatures stay disabled. The RSA
public-key blob and `.pub` line are still named `ssh-rsa`, as required by SSH. Rebuild/restart the VM or
Fuse-X kernel for the crypto device's higher iteration ceiling; an older running
host capped at 100000 rounds rejects the new 200000-round request explicitly. The three client commands
share one executable and select their interface by invocation name. The staging
command atomically copies executables to `fuse-x-shared/ssh/`; it never copies
keys or changes the default launch/build/install lists. In Fuse-X, copy the
commands you want from `/mnt/host/ssh/NAME.fxe` to `/bin/NAME` and make them
executable. Keep the invocation names when installing.

Generate a guest-owned identity and inspect its public half:

```sh
ssh-keygen
ssh-keygen -y -f /root/.ssh/id_ecdsa
ssh-keygen -l -f /root/.ssh/id_ecdsa.pub
ssh-keygen -p -f /root/.ssh/id_ecdsa
```

The default is ECDSA P-256 in `~/.ssh/id_ecdsa`, with a matching OpenSSH `.pub`
line and SHA256 fingerprint. `-b 384` and `-b 521` select the other enabled NIST
curves. `-t ed25519` creates `~/.ssh/id_ed25519`; `-t rsa` creates
`~/.ssh/id_rsa` (3072 bits by default; `-b 2048`, `3072` and `4096` are
accepted). RSA generation and private signing currently run in guest software
and can take substantially longer than ECDSA (generation takes minutes in RVM); supported public RSA arithmetic
uses the crypto device. `-f` changes the path and `-C` the comment. Existing key files are never
overwritten by generation. Private files are 0600; `~/.ssh` is created as 0700.
Passphrases are read without echo and confirmed. `-N` and `-P` supply new/old
passphrases for batch use. An empty passphrase writes EC/RSA PEM or Ed25519 PKCS#8 PEM;
a nonempty one writes standard encrypted PKCS#8: PBES2,
PBKDF2-HMAC-SHA512 (200000 iterations, fresh 16-octet salt), AES-256-CBC with a
fresh IV and PKCS#7 padding. The crypto device accelerates the KDF; if unavailable,
the same derivation is available in guest software. Key and passphrase buffers
are cleared on completion.

Import supports EC/RSA DER and PEM, Ed25519 PKCS#8, unencrypted PKCS#8 PEM,
the above encrypted PKCS#8 profile, and **unencrypted** OpenSSH private-key
containers for these key types. Bcrypt-encrypted OpenSSH private files, explicit
EC domain parameters and other PBES2 parameter combinations are not supported.
OpenSSH itself does not import Ed25519 PKCS#8 private files; authorize the
guest-generated `.pub` half on the remote server, or import an unencrypted
OpenSSH Ed25519 identity into the guest. Protect private files during conversion. OpenSSH public lines are supported, independently of
private-container format. On macOS, `ssh-keygen -m PEM` can emit explicit curve
parameters: normalize a disposable/imported EC key with
`openssl ec -in INPUT -param_enc named_curve -out OUTPUT` before importing it.
Prefer generating a fresh identity in the guest and authorizing only its public
half on the remote host.

```sh
ssh user@host
ssh -p 2222 -i /root/.ssh/id_ecdsa user@host 'uname -a'
scp local.txt user@host:/remote/file.txt
scp user@host:/remote/file.txt /root/local.txt
scp -r /root/project user@host:/remote/
scp -r user@host:/remote/project /root/
sftp user@host
```

`ssh` supports interactive PTYs, window-size changes, terminal control
characters, remote commands, stdin streaming, separate stdout/stderr and the
remote exit status. Use `-t`/`-T` to force/disable a PTY. IPv4 and guest DNS are
used; there is no host networking/SSH shortcut. Authentication currently uses
one public-key identity, not passwords or an SSH agent. Without an explicit IdentityFile, the client searches for `id_ecdsa`, then
`id_ed25519`, then `id_rsa`, selecting the first existing file. The client verifies
Ed25519, ECDSA and RSA-SHA2 host keys, uses ECDH P-256/384/521 and AES-GCM, and does not enable CBC transport.

Host-key verification is mandatory. The default asks for explicit `yes` for an
unknown host and saves its key to `~/.ssh/known_hosts` only after the SSH
handshake authenticates the key. Later connections verify it. Changed and
revoked keys fail. Plain, comma/pattern and OpenSSH SHA1-hashed host entries are
recognized. `StrictHostKeyChecking=yes` rejects unknown hosts too;
`accept-new` explicitly allows first-use enrollment but still rejects changed
keys. There is deliberately no `StrictHostKeyChecking=no` bypass. Files must be
owned by the guest user, regular files rather than symlinks, and not writable by
others. Private keys also require no group/other permission bits.

`~/.ssh/config` supports `Host` patterns and these options: `HostName`, `User`,
`Port`, `IdentityFile`, `UserKnownHostsFile`, `StrictHostKeyChecking`, `BatchMode`,
`ConnectTimeout`, `RequestTTY`, and `IdentitiesOnly yes`. First matching values
win; command-line values take priority. Unsupported active options fail rather
than being silently ignored. During the SSH handshake `ConnectTimeout` is an
I/O-idle timeout, with a separate total bound of four times that value; this
allows an active RVM handshake to make progress across expensive crypto/JIT
phases. TCP connection establishment keeps its ordinary timeout. `-F` selects another configuration file, and
`-o Option=value` supplies an option. Example:

```text
Host work
    HostName 192.168.1.25
    User alice
    Port 22
    IdentityFile ~/.ssh/id_ecdsa
    StrictHostKeyChecking ask
```

SFTP commands are `pwd`, `lpwd`, `ls`, `cd`, `lcd`, `get`, `put`, `mkdir`, `rmdir`,
`rm`, `rename`, `chmod`, `help`, and `quit`. Quote paths containing spaces.
`sftp -b /path/to/batch host` stops at the first failed command and never prompts
for host trust or a key passphrase. `scp` uses SFTP, supports one local/remote
pair, directory destinations and **`-r` directory-tree copies** in either
direction, including empty directories. These are copies: sources remain in
place. Recursive traversal is bounded to 32 levels and rejects symlinks and
special files explicitly. Errors leave a possibly partial tree and return
failure. Legacy SCP, wildcards, compression, forwarding, SSH-agent support and
metadata preservation are not implemented. Downloads use an exclusive temporary file and
rename only after success, so failed downloads preserve an old destination.
Uploads currently write/truncate the selected remote file and can leave partial
remote data on interruption. Transfers use bounded octet buffers and signed
32-bit guest file offsets.

## Mounted SFTP directories and Commander

The native guest kernel (Fuse-X and freestanding x64) now has a guest userspace
filesystem bridge. `sftp-mount` keeps SSH/SFTP inside a guest process and exposes
an ordinary directory, usable by Commander, the shell, editors and other POSIX
programs. Restart/rebuild an older Fuse-X kernel before using it. Mounts are
explicit; they are not added to the normal application inventory or boot profile.

Build and stage the Fuse-X helper explicitly:

```sh
python3 ports/ssh/app.py --app sftp-mount
python3 ports/ssh/stage.py --source build/ssh/sftp-mount.fxe \
  --destination fuse-x-shared/ssh/sftp-mount.fxe
```

Authorize a guest identity on the remote server as for the SSH client above.
Save connection settings in `~/.ssh/config`, e.g. the `Host work` profile above.
In the guest:

```sh
mkdir /mnt/work
/mnt/host/ssh/sftp-mount.fxe --background work:/home/alice/project /mnt/work
ls /mnt/work
cp /mnt/work/notes /root/notes
sftp-mount --status
cd /
sftp-mount --unmount 0
```

The last commands assume the helper is installed as `/bin/sftp-mount`; its full
`/mnt/host/ssh/sftp-mount.fxe` path works for every invocation too. `--status`
reports attachment numbers, paths, daemon PIDs and connected/disconnected state.
Without `--background` the helper remains in the foreground. With it, host-key
confirmation and key-passphrase entry still happen in the foreground; the parent
returns only after its child has attached the directory successfully. The service
then leaves the terminal session, so quitting Commander does not close the mount.
Exit any shell whose cwd is inside the mount, and close its files, before
unmounting. `--unmount NUMBER` refuses busy attachments. Mounts end at shutdown;
remote file changes persist on the server.

In **Commander, press `s`**, then choose **Connect remote directory**. Enter
`work:/home/alice/project` (or `user@host:/absolute/path`) and an empty directory
under `/mnt`. The active panel opens it after authentication. Use ordinary F3/F4,
F5/F6, rename, mkdir and delete operations. Press `s` again and choose
**Disconnect / connection status** to select a mount for removal. Profiles and
identities are shared with SSH; the UI stores no additional credentials. It uses
`/bin/sftp-mount` when installed and otherwise the staged helper above.
Authentication temporarily suspends curses for the normal protected SSH prompts.
Directory reads and file operations are synchronous, bounded filesystem calls;
this first UI does not yet provide background transfer progress/cancellation.

The opt-in staged `ssh/commander.fxe` includes this menu; ordinary Commander
builds also compile the integration. The x64 helper is built separately:

```sh
python3 tools/x86_64/ssh_apps.py --app sftp-mount --output build/ssh/x64
```

It produces `build/ssh/x64/sftp-mount.rxe`, for an explicitly selected x64 image.
The Commander UI is exercised on Fuse-X; x64 acceptance exercises the filesystem
through a separate POSIX program, without claiming a new x64 desktop port.

Save and failure semantics:

- Four attachments, one serialized mailbox per attachment, 4096-cell I/O
  chunks. Supervisor-owned buffers carry copied values and data; process
  pointers never become daemon pointers. Request sequence/version/owner checks
  reject stale replies. The mount adapter preserves pathname-parser scratch
  around sleep and reserves inode slots before any remote operation can block.
- SFTP file handles retain open-file identity across unlink/overwrite rename.
  Writes, append, truncate, file/directory creation/removal and chmod are supported.
  Directory snapshots cache READDIR attributes for following stat calls. Metadata
  may be cached for up to two seconds; reopening a directory refreshes its listing.
- Atomic replacement requires `posix-rename@openssh.com`. There is no unsafe
  remove-then-rename fallback. Dirty handle release and `sync` request
  `fsync@openssh.com`; `O_SYNC` writes request it immediately. A failed remote
  flush is reported, including through `close`. These are remote-server barriers,
  not a promise about hardware or storage beyond that server's fsync contract.
  These mount-specific extensions are not yet implemented by our wolfSSH server;
  use an OpenSSH-compatible remote for full writable mount/save semantics.
- A disconnected/killed daemon makes filesystem calls fail explicitly. There is
  no automatic replay of possibly completed writes. Remount to reconnect. Socket
  waits have a 30-second idle bound; the kernel also has a 120-second bound for
  a stopped/wedged service or mailbox queue. Fatal signals are deferred while a
  kernel request owns resources, then delivered after cleanup.
- **The existing 30-cell name limit still applies**, including remote directory
  entries. An unrepresentable name rejects the listing/lookup with
  `ENAMETOOLONG`; names are never truncated. Guest paths remain below 512 cells,
  file offsets are signed 32-bit, and each service has a 512-node cache budget
  (including directories/root). Large directories may exhaust that budget.
  Symlinks, hard links, ownership changes and device nodes are unsupported.
  Extending names remains a coordinated filesystem/libc/RFL task, not an SFTP
  buffer change. Mounts currently require the root guest account.

The kernel bridge is transport-independent: its ABI is
`ports/fuzix/ripple/remotefs-abi.h`, with native inode dispatch under `fuse-x/`.
The SSH-specific service and bounded SFTPv3 wire parser live in `mount.c` and
`mount-wire.inc`; pinned wolfSSH sources are unchanged. The native x64 kernel
shares this same bridge through its existing Fuse-X kernel source integration.
RVM mounts are deliberately not enabled in this milestone.

## Filename and filesystem limits

The current local filesystem still stores names in **30 cells per component**,
inside a fixed 32-cell directory record (one inode cell, 30 name cells and
one padding cell in the current C16 layout).
The disk-capacity extension changed block addressing, not this directory format.
This also remains reflected in the guest `NAME_MAX`, libc `dirent`, the RFL
POSIX adapter and host-directory bridge. Changing only the SSH buffers or
`NAME_MAX` would not extend the local filesystem.

Client transfers validate every local path component before lookup, rejecting
longer names with `ENAMETOOLONG` instead of allowing the kernel's historical
prefix truncation. Recursive downloads validate remote directory names before
copying their children. Temporary download filenames are short sibling names,
so a valid 30-character destination remains usable. Private-key and known-host
replacement writes also use short sibling names; credential reads reject
overlong components before lookup. Paths are bounded to 511
cells and file offsets to signed 32-bit values.

Removing this local filesystem restriction needs a versioned directory format
and coordinated kernel, libc, RFL, hostfs, image-builder, reader and fsck changes,
plus explicit migration or rebuilding of existing images. SSH key-algorithm
support and recursive copying do not reformat any disks. Git over SSH already
has a remote-command transport foundation here; a Git application port remains
separate work.

## Current scope

- Four concurrent connections, one session channel each, bounded buffers and
  nonblocking socket/PTY I/O driven by `select()`.
- Root login by authorized public key only. No passwords, PAM or multiuser
  account policy. The server intentionally requires root; an authenticated
  client receives root access to the guest filesystem.
- The server retains an ECDSA P-256 host identity, and accepts ECDSA
  P-256/384/521, Ed25519 and RSA-SHA2 user authentication. P-256 ECDH,
  AES-128-GCM and upstream strict KEX remain enabled. The client also verifies
  RSA-SHA2 and Ed25519 remote host identities. ECDSA keeps priority when the server offers
  several host-key types, retaining the existing accelerated path.
- Authorized-key comments and whitespace are accepted. Unsupported bare key
  types are ignored. Key options are rejected rather than silently discarding
  restrictions such as `no-pty` or `command=`. Credentials must be root-owned,
  must not traverse symlinks or writable directories, and host private keys must
  have no group/other permissions.
- Shell and command execution use the real `/bin/sh`. PTYs propagate terminal
  modes, window-size changes and control characters. Exec sessions preserve
  stdout, stderr and command exit status separately.
- SFTP v3 runs in the connection process. Ordinary modern OpenSSH `scp` uses
  that SFTP subsystem. Legacy `scp -O` is not implemented. SFTP supports ordinary
  file transfers and directory operations; offsets above signed 32-bit guest
  `off_t` are rejected. Host file octets are stored as guest cells; attempts to
  download cells above 255 fail explicitly instead of truncating them.
- Some optional metadata operations remain unsupported, including descriptor
  ownership changes. This is not a promise of every SFTP extension or `scp -p`
  metadata feature.
- Authentication/session-request timeout is 30 seconds. Active sessions have no
  hard lifetime limit. Disconnect and daemon termination clean up session
  children and PTYs.

`rsync` is a later application port, not a shortcut to file transfer. It requires
an rsync executable and its additional protocol on both ends, usually carried
by SSH remote exec. SFTP gives us interoperability with existing `sftp` and
modern `scp` first.

## Service-manager handoff

`examples/sshd.ini.example` is an optional definition for the current services
interface. It is deliberately not installed or enabled. The daemon stays in the
foreground and writes one `R` byte to `SVC_READY_FD`, closing that descriptor,
only after credentials, bind and listen succeed. SIGTERM stops the listener and
connection children. The service manager owns restart policy, logging and boot
ordering; add the eventual network dependency using that manager's agreed name.
Host-key generation is explicit provisioning, never a restart-loop side effect.

## Source provenance and C16 adaptations

`upstream/` contains original wolfSSH **1.6.0** files from the October 2026
[stable release](https://github.com/wolfSSL/wolfssh/releases/tag/v1.6.0-stable).
`source.json` pins the archive SHA-256 and every imported file. It is GPLv3, with
commercial licensing also offered upstream; see `upstream/COPYING` and
`upstream/LICENSING`.

The port reuses the existing wolfSSL **5.9.4** crypto sources and adaptations
under `ports/tls/`. `crypto/` adds original private-key export and Ed25519 sources;
`crypto/source.json` pins their source archive and each file.
No TLS configuration or vendored TLS file is changed by this port.

`adapt.py` verifies originals and creates generated working copies under
`build/ssh/source/`. It makes octet truncation explicit, namespaces conflicting
non-inline helpers, replaces optional-comma GNU variadic macros with equivalent
C99 forwarding, and supplies checked filesystem wrappers. SFTP's internal
protocol/request arithmetic explicitly retains C32 widths, and its handles are
eight protocol octets rather than `sizeof(word32) * 2` guest cells. Large READ
requests receive bounded short DATA replies, as permitted by SFTP. RCC's ABI
remains unchanged. The SSH-only crypto adaptation explicitly sign-extends
8-bit masks in wolfCrypt's integer selectors: RCC signed characters are 16-bit,
so the original `(signed char)255` would incorrectly remain positive. An additional SFTP fix preserves direct SSH flush errors so
socket backpressure resumes a queued reply instead of discarding it. The
directory size formatter preserves all 64 protocol bits without depending on
the guest's unsupported `%lld` formatting; directory string lengths are checked
before transmission. This avoids sending uninitialized cells to the crypto
device. The generated adaptation report lists these transformations.

To reproduce imports from the exact pinned archives:

```sh
# Initial wolfSSH import only; refuses an existing upstream directory.
python3 ports/ssh/vendor.py /path/to/wolfssh-1.6.0.tar.gz
# Import/verify the additional crypto source using the existing pinned archive.
python3 ports/ssh/vendor.py --wolfcrypt-archive build/tls-audit/wolfssl-5.9.4.tar.gz
```

## Acceptance checks

```sh
python3 ports/ssh/tests/interop.py
python3 ports/ssh/tests/interop.py --target ripple \
  --executable build/ssh/sshd.rxe --output build/ssh/tests-ripple
python3 ports/ssh/tests/platform.py
python3 ports/ssh/tests/keys.py
python3 ports/ssh/tests/keys.py --target ripple
python3 ports/ssh/tests/imports.py
python3 ports/ssh/tests/imports.py --target ripple
python3 ports/ssh/tests/client.py
python3 ports/ssh/tests/client.py --trust-only --output build/ssh/client-trust
python3 ports/ssh/tests/client.py --target ripple --output build/ssh/client-tests-ripple
python3 ports/ssh/tests/interop.py --key-types-only
python3 ports/ssh/tests/client.py --features-only --key-type ed25519
python3 ports/ssh/tests/client.py --features-only --key-type rsa
python3 ports/ssh/tests/client.py --features-only --key-type rsa --rsa-bits 4096
python3 ports/ssh/tests/platform.py --target ripple
```

Tests generate temporary keys and disposable images, boot the real guest,
and connect the host OpenSSH tools through an independent raw Ethernet peer.
The local TCP relay only forwards encrypted octets; it does not authenticate,
encrypt or implement SSH. RVM runs in JIT mode. The suite checks generated keys,
credential rejection, authentication, exit status, separate stderr, binary data
across C16 I/O and offset boundaries, rekey, PTYs, reconnects, scp and SFTP
operations. Clean shutdown is followed by filesystem validation. Logs stay under
the selected output directory; production images and running sessions are not
used. `--files-only`, `--terminal-only` and `--list-only` narrow a run to the
corresponding cases. Tests share the application's build lock and record the
executable hash in `input.json` so a build in progress cannot supply an older
executable unnoticed.

The expanded checks cover RSA-2048/4096 server authentication on both engines,
RSA-3072/4096 client sessions and recursive transfers on Fuse-X, RSA-2048 client
sessions on RVM JIT, and Ed25519 host/user authentication on both. Ed25519's
empty-message key/signature vectors are from RFC 8032 section 7.1; the regression
also checks tampering and the C16 octet-mask selectors. OpenSSH private-container
imports (including RSA-4096), encrypted conversion, full-length key-file
replacement and overlong-alias rejection are checked on both engines.

**Remaining performance limit:** native RSA-2048 generation was independently
validated with OpenSSL/OpenSSH. The RVM software-generation attempt exceeded its
ten-minute validation budget, so successful RSA generation in RVM is not yet
claimed. Existing RSA identities can be imported; ECDSA and Ed25519 generation
are verified there. No host-crypto primitive or compiler ABI was changed for
this key-algorithm extension.

## Next integration work

Connect the optional service definition after the service manager and inbound
network route are ready. Verify the x86 executable, entropy source and sustained
network path independently; a working Wi-Fi association alone is insufficient.
Then extend authentication/account policy and SFTP metadata compatibility, and
stress concurrent sessions, disconnects and large directories. A standalone
guest rsync port can follow once SSH remote execution is established there.

The client suite reverses the interoperability direction: guest tools connect to
a disposable host OpenSSH server through a blind Ethernet relay. On macOS that
fixture uses `StrictModes no` solely because its private temporary directory is
below the world-writable `/private/tmp`; the guest's credential and host-key checks
remain enabled. Production configuration is never changed. Key tests compare
fingerprints with OpenSSH and decrypt/check generated keys with OpenSSL.
`keys.py` checks RSA generation by default on native Fuse-X; add
`--rsa-generation` on RVM for the expensive software-generation case. RVM
private-key import/signing and RSA host verification are separate checks.

A focused compiler regression, `test_ssh_nested_array_zero_initializer`, records
RCC rejecting a valid aggregate `{0}` when its first member is an array. It was
confirmed red before using explicit nested braces in this port; the compiler
fix is separate from the SSH implementation.

## SFTP mount acceptance (Fuse-X and x64)

```sh
python3 ports/ssh/app.py --app sftp-mount
python3 rfl/tools/build_os.py rfl/apps/commander/main.rfl --os fuse-x \
  -o build/ssh/commander.fxe
python3 ports/ssh/tests/mounts.py --output build/ssh/mount-tests-final
python3 ports/ssh/tests/mounts.py --deny-extensions --output build/ssh/mount-tests-extensions
python3 tools/x86_64/ssh_apps.py --app sftp-mount --output build/ssh/x64
python3 tools/x86_64/boot.py build --output build/ssh/x64-boot \
  --stage riplix --network user --root-mib 128 --font rom
python3 ports/ssh/tests/mounts.py --target x64 --output build/ssh/mount-tests-x64
```

These create private keys, a local OpenSSH server, disposable roots and, for
x64, a private QEMU guest with a copied firmware variables file. The real SFTP
connection tests binary I/O, append/truncate, atomic overwrite and directory
moves, metadata/listing, concurrent processes, and open-file identity after
unlink/rename. Fuse-X additionally drives Commander's connection dialogs and
checks Commander copies in both directions, long-name rejection, a killed
daemon/disconnected mount, and server refusal of flush/atomic-rename requests. Both finish with clean shutdown and
filesystem validation. No production disk, running session, physical device or
host SSH configuration is changed. New mount tests do not run RVM.
