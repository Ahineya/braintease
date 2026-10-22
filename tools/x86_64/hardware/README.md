# HP development: start here

Updated 2026-10-22 from retained receipts and the later native Graphite hardware
trial. **These are dated observations, not a live check of the HP's current OS,
firmware variables, files or network.**

Read these in order:

1. This baseline and the [retained evidence](receipts/2026-10-22/README.md).
2. [SSH connections and missing local files](ssh.md).
3. [Ubuntu development loop](ubuntu-dev-loop.txt) and
   [publishing the next EFI](efi-updates.md).
4. The [master plan](../../../docs/x64-master-plan.md) for remaining work;
   [implementation README](../README.md) for build/runtime details.

## Latest recorded baseline

| Item | Recorded result |
| --- | --- |
| Machine | HP Envy x360 16-ac0xxx; board 8C66 |
| Accepted GUI candidate | `4444d9166e54be07dfa2e0ff54512f52375f42018e3857917db5aca17ccd171c` |
| Later desktop trial | Entry **`0004`**, `EFI/Riplix-trial-a889571f688c/BOOTX64.EFI`, SHA256 `a889571f688c595d098da5c3d50618f98d7ba4f5a8d5d0e75822632b6df88a6d`; boots, interaction issues remain |
| Candidate location | Entry **`0003`**, label `Riplix graphics test`, `EFI/Riplix-gui/BOOTX64.EFI` |
| GUI fallback file | `EFI/Riplix-gui/BOOTGOOD.EFI`, terminal candidate `32d71c89df9ed6ed8e0a039faa19b931d1a1ef529770c8a64a70ad6fc5790691` |
| Other entries | `0001` Ubuntu; `0000` earlier Riplix development; `0002` terminal test at `EFI/Riplix-tui` |
| Boot policy | Ubuntu first temporarily, with one-shot Riplix trials; recorded order `0001,0000,2001,2005,2002,2004` |
| Storage | Dedicated p3: 1 GiB C16 V3 filesystem within a 30 GiB reservation. EFI updates use p1, not p3. |
| Remote access | Riplix SSH/SFTP on 22; Ubuntu supervised root SSH on 2222; separate USB rescue SSH on 22 |
| Terminal applications | Nine packages installed and retained across reboot; exact executable checks and SSH PTY Levee/Commander/Rift checks passed |
| Graphics/input | Native software GX and LVGL calculator; user accepted physical touchpad movement and clicks |

The [installation receipt](receipts/2026-10-22/graphics-installed.json) records
one-shot selection, not a successful boot. The separate
[touchpad acceptance](receipts/2026-10-22/touchpad-acceptance.json) binds boot,
network/service health, calculator readiness and user input acceptance to the
same candidate hash. A staged manifest deliberately remains
`staged-not-boot-validated`; do not rewrite it to imply acceptance.

The later [native Graphite trial](receipts/2026-10-22/graphite-native-trial.json)
booted the multi-window desktop at 1920×1200, with all nine staged executables
hash-verified under `/root/graphite-test`. Launch with
`sh /root/graphite-test/startgraphite.sh`. The user saw the desktop and launched
Calculator through the Applications menu using Enter, but dock hover/clicks
were unstable, launch took about 15 seconds, and interaction felt laggy. This
is bring-up evidence, **not full physical interaction acceptance**. HP was left
running at the user's request; no final return-to-Ubuntu trial was performed.
The next ordinary reboot still defaults to Ubuntu.

An identical panel-configuration revision loop was subsequently fixed in source
and passes the host native desktop regression. That fix, the hidden-modal focus
correction and the new `desktop-probe --idle` diagnostic are **not in the HP
candidate or staged apps**. The new VM idle assertion is not yet run. Start
the next session by validating this fix in a disposable VM, then give a fresh
candidate its own hardware trial; do not claim the existing EFI contains it.

Entry numbers are specific to this installation. Re-read `efibootmgr -v`, the
ESP mount/PARTUUID and payload hashes in Ubuntu before using them. `BootNext`
in an old receipt does not mean it is still set. Do not infer the current OS
from an old “HP left running…” sentence or from a responding TCP port alone.

## What remains open

- Held SSH continuity through an actual GTK rotation and sustained radio
  reliability; Wi-Fi profiles/address configuration remain integration work.
- Local full-screen terminal/cursor acceptance beyond the recorded key and
  remote PTY checks; gestures, scrolling, USB mice and touchscreen.
- Multi-window desktop interaction acceptance and performance: stop idle panel
  refresh churn, verify stable pointer clicks, then measure cold app startup and
  full-resolution composition. Native audio and actual ACPI power-off remain.
- Production process mappings, ring 3, timer-driven userspace preemption,
  LP64 applications and SMP. Current native C16 apps run at ring 0 on one CPU
  with scheduling checkpoints.
- Automatic failed-boot recovery and a Riplix-owned ESP writer. The documented
  Ubuntu staging loop exists; a hung candidate still needs a reset.

## Evidence and clean checkouts

The [receipt index](receipts/2026-10-22/README.md) retains small exact copies of
the acceptance/install records, their original paths and SHA256 checksums.
Their nested references to detailed `build/` logs are optional local evidence,
not files promised by a clean checkout. Receipts are observations, not signed
attestations or proof that today's source rebuild is the accepted binary.

Graphics/input implementation sources are tracked now. A clean checkout still
does not contain accepted EFI binaries, compiled application caches, private
SSH keys, Wi-Fi credentials, root images or all the old build logs. Restore
private inputs from the owner; use the [SSH recovery instructions](ssh.md) for
keys and pins and the [clean-checkout LVGL setup](../graphics/README.md#clean-checkout-lvgl-setup)
for the graphics adaptation/cache. Record hashes and source revision/dirty diff for a fresh build;
give it its own tests and physical acceptance.

## Which document wins?

This page and its linked operational runbooks describe the reconciled handoff.
The master plan describes direction and gates. The implementation README
describes code/build contracts. `hp-integration-plan.txt`, dated sections of
`internal-boot.txt`, rescue/USB notes and old `build/` receipts preserve history;
they do not override newer evidence or a fresh machine inspection.

For later milestones, update this baseline, the master plan and the relevant
runbook together. Retain a compact receipt in the repository with exact image
hash, target, tested scope and limitations. Keep passwords/private keys and
large binaries out of those receipts; preserve older accepted entries until
their replacement and recovery role have been reviewed.
