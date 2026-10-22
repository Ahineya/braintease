# HP SSH connections

These recipes use the recorded HP address `192.168.100.107`. Run them from the
repository root on the Mac. They do not identify which OS is currently booted;
check the remote OS after login. If the address changes, verify the new address
and host identity through an existing trusted session or the physical console.

## Riplix

The private client key is local, ignored and must remain private:
`build/x86_64-system/keys/client`. The checked-in file below contains only the
server's public host key.

```sh
ssh -F /dev/null -o BatchMode=yes -o IdentitiesOnly=yes \
  -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile=tools/x86_64/hardware/ssh/riplix-known-hosts \
  -o ConnectTimeout=5 -i build/x86_64-system/keys/client \
  root@192.168.100.107
```

For file transfer, use `sftp` with the same options and destination instead of
`ssh`; its port option is `-P` if needed. This preserves host verification and
uses the guest SFTP server.

Recorded client public-key fingerprint (ECDSA P-256):
`SHA256:cfyOl6VPlTD/chU/jXgBb1ETtLKJ0GHYn3pTfnDkokQ`.
The guest authorizes it in `/root/.ssh/authorized_keys`. The server's private
host identity is separate, at `/etc/ssh/hostkey.der`.

## Ubuntu development listener

```sh
ssh -F /dev/null -o BatchMode=yes -o PubkeyAuthentication=no \
  -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile=tools/x86_64/hardware/ssh/ubuntu-known-hosts \
  -o ConnectTimeout=6 -p 2222 root@192.168.100.107
```

This is the owner's existing **passwordless root listener**:
`AuthenticationMethods=none`, not public-key authentication. No local client
key or Ubuntu password is required for this listener. Host verification is
still required. Do not substitute the Riplix or rescue host pin.

The [development-loop runbook](ubuntu-dev-loop.txt) documents the systemd
service, its temporary root-password-setting change, restoration and removal.
The original setting is kept only on the HP in
`/var/lib/riplix-dev-ssh/root-password.original`; do not copy or print it.
If the listener is unavailable, use Ubuntu's physical console or the owner's
ordinary Ubuntu access. A timeout is not a reason to change authentication or
remove a host pin.

## USB SystemRescue

```sh
ssh -F /dev/null -o BatchMode=yes -o IdentitiesOnly=yes \
  -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile=tools/x86_64/hardware/ssh/rescue-known-hosts \
  -o ConnectTimeout=5 -i build/x86_64-rescue/private/id_ed25519 \
  -p 22 root@192.168.100.107
```

Recorded maintenance client public-key fingerprint (Ed25519):
`SHA256:JkiIw90mPM57Jn8ztiyIxMKNfWIcS2V9S+lhsnw8fhM`.
Physical rescue used port **22**. The earlier rescue port-2222 child listener
was superseded by `sshd.service`; Ubuntu's port 2222 is a different service.
See [rescue history](hp-rescue-usb.txt) for the provisioned USB lifecycle.

## Public host pins and their provenance

These are exact copies of the previously used local pins, retained on
2026-10-22. They are public identity material, not credentials.

| OS | Host-key fingerprint | Original local pin file |
| --- | --- | --- |
| Riplix | `SHA256:4uAhpYl39kMKfkiwM5riFk6Lb7H0h+0uHC9a9JxVRRE` | `build/x86_64-hp-system/known-hosts` |
| Ubuntu | `SHA256:oR/zE3AkEVaga3yFKxYtN1x6n1XOYpq/RKK8kQNLZug` | `build/x86_64-internal-boot/ubuntu-known-hosts` |
| Rescue | `SHA256:9YwmXc7cHS3Pa/vHhT7P2dn/skSRf4SpG+8TJ6UgIvc` | `build/x86_64-rescue/private/hp-rescue-known-hosts` |

Inspect them locally with, for example:

```sh
ssh-keygen -lf tools/x86_64/hardware/ssh/riplix-known-hosts
```

Riplix and rescue share an address/port but have different host identities.
Keep the three pin files separate. The recipes use `-F /dev/null` to avoid
unrelated user SSH configuration affecting the connection.

## Missing files or changed identities

- **Missing host-pin file:** restore the tracked file from the repository.
  Old local scripts may still expect the paths in the table. Copy the matching
  tracked pin there only if missing; if an existing file differs, stop and
  establish why before replacing it. Prefer the recipes above for new work.
- **Missing private client key:** obtain it from the owner's existing backup
  through a private channel, restore mode `0600` and check its public fingerprint
  with `ssh-keygen -y -f KEY | ssh-keygen -lf -`. Neither the repository nor a
  host pin can reconstruct it. Do not overwrite another existing key.
- **No recoverable client key:** create a new key at a new local path and enroll
  its `.pub` using an already trusted session or physical console. Riplix uses
  ECDSA P-256 (`ssh-keygen -t ecdsa -b 256`); rescue uses Ed25519
  (`ssh-keygen -t ed25519`). Riplix image seeding also supports
  `boot.py --ssh-tools DIR --ssh-authorized-key PUBLIC_KEY` during the normal
  reviewed image build/install process. Do not rebuild or replace the running
  root merely to recover login, or regenerate the server host key.
- **Host mismatch/recreated USB/replaced root:** verify the server's public key
  from its physical console or another trusted channel. Ubuntu's service uses
  `/etc/ssh/ssh_host_ed25519_key`; Riplix's host key is independent of the client
  key. Review and record a new public pin only after verification. Do not use
  `StrictHostKeyChecking=no`, delete a pin to silence the error, or treat
  `ssh-keyscan` alone as identity verification.

Use the owner's local Ubuntu credentials for console recovery if required;
there is no repository-stored Ubuntu password. Private key files, root password
settings and Wi-Fi credentials do not belong in handoff receipts.
