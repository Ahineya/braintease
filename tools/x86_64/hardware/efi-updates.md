# Publishing the next HP EFI

This is the repeat-update runbook, using the maintained
[`internal_boot.py`](../internal_boot.py). Follow the [current handoff](README.md)
and [pinned SSH recipes](ssh.md) first. This procedure has not been executed as
part of the original documentation reconciliation. The later
[native Graphite trial](receipts/2026-10-22/graphite-native-trial.json) exercised
this fresh-namespace apply/inspect procedure and one-shot boot as entry `0004`.
Its desktop interaction remains unaccepted, and the user requested leaving HP
running before a return-to-Ubuntu check. The staged manifest remains unchanged.

## Publication policy

**Use a fresh namespace for each changed candidate.** For example,
`Riplix-trial-<candidate SHA256 prefix>`. The tool stages complete directories
atomically without replacing an existing namespace. Applying the same hashes
again only verifies/fsyncs them; different hashes in an existing namespace are
deliberately refused. A prefix collision requires another fresh name, never
deleting the existing directory to make the command pass.

The recorded GUI update used ignored
`build/x86_64-graphics-wave/refresh-efi*.py` scripts to replace the existing
candidate and manifest. Those are historical, machine-specific scripts, not
the maintained update interface. Do not copy their in-place replacement into
the staging tool or assume `apply --directory Riplix-gui` updates it.

Keep accepted `EFI/Riplix-gui`, `EFI/Riplix-tui`, `EFI/Riplix`, Ubuntu and USB
recovery intact. This updates the embedded kernel/EFI, **not** packages, p3,
root format, Wi-Fi configuration or the guest SSH host identity. A retained EFI
is not an automatic rollback of the root or installed applications.

## 1. Prepare and identify the candidate

Record the source revision plus any dirty patch, build command, required private
inputs and acceptance results. Compute the candidate SHA256 independently and
bind it to those results. A new build does not inherit the accepted GUI image's
physical acceptance. Keep a copy of the previously accepted EFI as known-good.

The retained GUI receipt identifies accepted image
`4444d9166e54be07dfa2e0ff54512f52375f42018e3857917db5aca17ccd171c`.
If using that image as the next trial's fallback, obtain its exact binary from
the owner's retained build or verified `EFI/Riplix-gui/BOOTX64.EFI` and check
its hash. The GUI directory's **BOOTGOOD.EFI is older**, hash
`32d71c89df9ed6ed8e0a039faa19b931d1a1ef529770c8a64a70ad6fc5790691`.
Do not silently substitute it. Candidate and known-good must be distinct.

Run relevant host/QEMU tests and the staging tests before hardware publication:

```sh
python3 tools/x86_64/tests/test_internal_boot.py
```

## 2. Revalidate Ubuntu and the target

Connect to Ubuntu with its pinned port-2222 recipe. From that root session,
inspect the current machine, mounts, partition map, boot entries and Secure Boot:

```sh
cat /sys/class/dmi/id/product_name
lsblk -o NAME,MODEL,SERIAL,SIZE,FSTYPE,PARTUUID,MOUNTPOINTS
findmnt /
findmnt /boot/efi
efibootmgr -v
mokutil --sb-state
df -h /boot/efi
```

Require these independently verified identities, rather than assuming a path
names the intended device:

- HP Envy x360 16-ac0xxx, WD PC SN560 SDDPNQE-1T00-2006,
  serial `24231U807090`, physical disk `/dev/nvme0n1`.
- Ubuntu root on p2; ESP p1, vfat, mounted at `/boot/efi`, PARTUUID
  `5b46a40c-54be-48b3-8a20-59d35a5d5165`.
- Riplix p3 is unmounted and has the dedicated identity/geometry in the storage
  record. It is not opened for writes by EFI publication.
- Secure Boot disabled for the unsigned candidate, Ubuntu's entry/path and
  retained recovery entries unchanged. Ubuntu first in the live BootOrder,
  with no unexplained pending BootNext.

Save the live `efibootmgr -v` output, a path/size/SHA256 inventory of every
existing ESP file, and both GPT metadata copies using the existing identity
and bounds from the [storage record](hp-nvme-persistence.txt). Store these
read-only snapshots outside the ESP. They are required for before/after
comparison; do not use formatting, partition repair or an old snapshot as
substitutes. Check room for both images and the manifest, and available firmware
entry space. Stop if identities or boot policy differ from the expected setup.

## 3. Transfer and stage into a new namespace

Use pinned Ubuntu SSH/SFTP to create a private root-owned work directory under
`/tmp` (for example with `mktemp -d /tmp/riplix-efi.XXXXXXXX`) and transfer:
the candidate, the independently verified known-good EFI and the tracked
`tools/x86_64/internal_boot.py`. Record/check all transferred file hashes on
both ends. Ubuntu clears `/tmp` at boot; retain the inputs and receipts on the
Mac as well. Do not transfer private SSH keys or whole root images.

In the Ubuntu root session, substitute the verified values below. `NEW_NAME`
must be a fresh allowed name and `WORK` the actual private transfer directory;
these commands do not create a firmware entry or reboot:

```sh
python3 "$WORK/internal_boot.py" apply \
  --esp-dir /boot/efi \
  --esp-id PARTUUID=5b46a40c-54be-48b3-8a20-59d35a5d5165 \
  --directory "$NEW_NAME" \
  --candidate "$WORK/candidate.efi" --candidate-sha256 "$CANDIDATE_SHA256" \
  --known-good "$WORK/accepted.efi" --known-good-sha256 "$ACCEPTED_SHA256"
```

Save successful JSON output and its `manifest_sha256`. Then inspect with that
exact hash, not a newly invented expected value:

```sh
python3 "$WORK/internal_boot.py" inspect \
  --esp-dir /boot/efi \
  --esp-id PARTUUID=5b46a40c-54be-48b3-8a20-59d35a5d5165 \
  --directory "$NEW_NAME" \
  --candidate-sha256 "$CANDIDATE_SHA256" --known-good-sha256 "$ACCEPTED_SHA256" \
  --manifest-sha256 "$MANIFEST_SHA256"
```

Require successful verification. Recheck the ESP inventory and GPT snapshots:
only the three files in the new namespace may have been added. Preserve every
pre-existing ESP byte/path and both GPT copies; p3 is not an installation target.
The [stager contract](internal-boot.txt) explains interruption/error handling.
Never remove pending staging directories or partial evidence blindly.

## 4. Create and select a one-shot trial

After successful readback and preservation checks, create an entry **without
changing BootOrder**. Use a label that identifies the trial:

```sh
efibootmgr --create-only --disk /dev/nvme0n1 --part 1 \
  --label "$NEW_NAME" --loader "\\EFI\\$NEW_NAME\\BOOTX64.EFI"
efibootmgr -v
```

Record the newly allocated `Boot####` number. Verify its partition GUID and
exact loader path, retained entries and unchanged BootOrder. Do not assume
`0003`: that is the recorded accepted GUI entry, not the next available number.
Set `NEW_ENTRY` to the verified four-digit number, then:

```sh
efibootmgr --bootnext "$NEW_ENTRY"
efibootmgr -v
```

Check that BootNext selects precisely the new entry and Ubuntu remains first.
Save receipts locally before deliberately running `systemctl reboot`. Creation,
selection and reboot are separate steps; a failed check must not trigger reboot.

## 5. Accept, return and record

Reconnect with the pinned Riplix recipe and verify actual OS/build identity,
Wi-Fi/services, storage, SSH/SFTP and the changed feature. Arrange physical
display/input checks when relevant. A successful SSH login alone does not
accept graphics, touchpad behavior or sustained GTK continuity.

Run an orderly Riplix `reboot`; reconnect with the pinned Ubuntu recipe. Verify
a fresh Ubuntu boot ID, consumed BootNext, Ubuntu-first order and all retained
ESP/GPT hashes. The earlier accepted terminal image passed this round trip;
the next candidate needs its own evidence.

BootNext is one-shot selection, **not a health watchdog**. A hung kernel may
need a physical reset; retained BOOTGOOD can be selected through firmware's
file browser. Preserve Ubuntu and USB access. Do not promise unattended
recovery or promote/delete entries as part of this procedure.

Retain compact tracked install/acceptance receipts with image and manifest
hashes, namespace/entry, source identity, preservation checks, date and tested
limitations. Update the [baseline](README.md) and master plan. Keep private
inputs and bulky binaries/logs out of tracked receipts. Review ESP/NVRAM space
before accumulating more trials; cleanup and changing the persistent default
require their own explicit plan and verified recovery choices.
