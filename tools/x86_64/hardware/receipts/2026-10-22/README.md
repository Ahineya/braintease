# Retained HP evidence: 2026-10-22

These small JSON records were copied byte-for-byte from local ignored build
outputs during documentation reconciliation. [index.json](index.json) records
their original repository-relative paths, sizes and SHA256 checksums. No
hardware operation or live verification was performed to create those original
four reconciliation records. The later Graphite trial below records new work.

| Record | Establishes | Does not establish |
| --- | --- | --- |
| [graphics-installed.json](graphics-installed.json) | GUI candidate hash/path, entry `0003`, retained fallback, one-shot selection, reported ESP/GPT preservation and no p3 installation write | That staging itself boot-tested the image, or that recorded BootNext/current OS is still live |
| [touchpad-acceptance.json](touchpad-acceptance.json) | Boot/service/calculator readiness and user-confirmed movement/clicks for the same GUI candidate | Gestures, scrolling, USB mice, touchscreen or a full compositor |
| [terminal-acceptance.json](terminal-acceptance.json) | Earlier terminal candidate's unattended reboot loop, nine-package persistence and SSH PTY application checks | Acceptance of a fresh build, local full-screen rendering or uninterrupted SSH during GTK rotation |
| [ubuntu-autostart-accepted.json](ubuntu-autostart-accepted.json) | Ubuntu SSH autostart after a real reboot with a fresh boot ID | Present reachability or a key-only authentication policy |

## Later native Graphite trial

[graphite-index.json](graphite-index.json) checksums two additional records,
without changing the original index or its historical assertions:

- [graphite-kvm.json](graphite-kvm.json) is an exact copy of the passing disposable
  KVM result for fixture EFI `eccdbfe5…`. It predates the new idle-watch test.
- [graphite-native-trial.json](graphite-native-trial.json) combines the live
  staging/boot/transfer observations and the user's physical feedback for entry
  `0004`, EFI `a889571f…`. The desktop boots, but unstable hover/clicks, roughly
  15-second Calculator launch and lag prevent full interaction acceptance.
  It also identifies a host-tested source fix that has not been deployed.

The full hashes, staged app hashes, preservation scope and limits are in these
records. No main private root image was used. The HP was left running; the
temporary LAN transfer server was stopped.

Nested `build/` paths and relative log references retain their original meaning;
the detailed logs/binaries are not part of this compact archive. The install
manifest's original timestamp and `staged-not-boot-validated` state are preserved,
including when it was carried forward by the historical in-place GUI refresh.
The later acceptance receipt supplies the separate boot/user evidence.

Use the [handoff](../../README.md) for current instructions. Do not edit an old
receipt to reflect a new candidate; retain a new dated record and update the
handoff. These are recorded local observations, not signed attestations.
