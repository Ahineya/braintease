# BDM-040: RAM execution and the decoded-resource audit

Investigated on 2026-10-21. The user has a spare controller but no electronics
tools available for the foreseeable future. This pass therefore examined files
and public sources only: no device was opened through USB, no feature report
was sent, and no boot/reset/update command was attempted.

**There is still no demonstrated, recoverable USB-only RAM execution route for
our inferred BDM-040.** We found a plausible loader-entry lead, but neither its
BDM-040 entry/exit behavior nor an unsigned RAM-load protocol is established.
The decoded updater resources have now been audited beyond their filenames.

## What is actually inside the protected resources?

They are already decrypted/unpacked. Local readable outputs are
`analysis/followup/crypto-audit/strings.json` and `numeric-tables.json`.
The [preceding investigation](crypto-and-recovery.md) records the resource
envelope, cipher, compression variant and original/output hashes.

The numeric reader now consumes **all 1,912 bytes**, with an error on leftover
or truncated data. Original method `0x06000a59` supplies its array sizes,
integer XOR masks and reverse storage order:

| Array | Count | Examples |
| --- | --- | --- |
| Signed 32-bit integers | 349 | `0`, `1`, `120`, `121`, `32768`, `950272`, flags and HRESULT-shaped error values |
| Signed 64-bit integers | 0 | Empty |
| 32-bit floats | 7 | `96`, approximately `2.4`, `0`, `30`, `100`, `2.5`, `25` |
| 64-bit doubles | 59 | `5000`, `0.5`, `96`, π, `180`, `255`, UI dimensions and offsets |

For example, integer lookup indices 151/152 give the firmware version byte
offsets `120/121`; index 179 gives the native transfer block size `32768`.
This is an application-wide constant cache, not an otherwise unexplained
1,912-byte secret blob. We have not traced every numeric use or ruled out every
possible derived-key computation.

The 2,560 strings include XAML property/asset names, updater filenames and
paths, settings, logging and chunks of telemetry schema text. A complete-string
hex scan found no 16-, 24- or 32-byte encoded key. A strict Base64/length scan
found three syntactic candidates, all recognizable UI/settings identifiers:

- `huntLeftPointNumStickSensitivity`
- `huntLeftStickSensitivityPresetId`
- `OpacityCheckRearRightTouchButton`

Their Base64 decodings are not identified crypto keys. This scan does not cover
arbitrary substrings, every encoding, text passwords, or derived keys.

### Crypto call sites, rather than keywords alone

We inspected the recovered managed crypto member references and their direct
and decoded delegate call edges. All **24 recovered crypto call-edge records**
originate in eight methods within `0x06000aa2`–`0x06000aaf`: the resource
unpacker, its algorithm selector and its custom SymmetricAlgorithm support.
The custom transform at `0x06000aa7` XORs bytes using a key indexed through the
IV; it is reached by selector 6, whereas the three recovered wrappers use
selector 2. No new cipher path from the firmware file loader was identified.

The generic typed-array reader at `0x06000a50` has no caller in the recovered
direct call edges or four resolved delegate-factory families. That means no
identified use, not proof of runtime unreachability through every mechanism.
The original assembly's metadata coverage remains partial, as documented in
[the REA trace](rea-trace.md).

The native update DLL has no named crypto imports or delay-import directory.
That fact alone cannot exclude statically linked or dynamically resolved crypto;
the stronger existing observation is the traced file-to-HID copy path.

### Finite tests of key reuse

To test the user's hypothesis instead of dismissing it, `probe_resource_keys.py`
uses the three observed 32-byte host resource keys, plus the three syntactic
24-byte Base64 candidates above, on the matching `0004/0630` firmware file.
No password guessing, key-space search or controller access is involved.

For each candidate it checks eight explicit start hypotheses: main-relative
`0x100/0x110`, and `0x60/0x70/0xc0` in each secondary region. Each sample is
64 KiB. Modes are ECB, CBC and CTR; CBC/CTR try a zero IV, the preceding file
block and the corresponding resource IV. Some parameter combinations repeat
the same IV; **336 counts parameterized trials, not distinct decryptions**.

Observed results:

- No conventional Cortex-M vector-table candidates.
- No checksum-validated zlib/gzip stream and no complete host-resource-codec stream.
- Output entropy ranged from approximately `7.99646` to `7.99790` bits/byte.
- Twelve AES-CMAC hypotheses did not match the opaque `0xf0..0xff` header field:
  each candidate over either the preceding 240 bytes or the 256-byte header
  with that final field zeroed.

These tests found no validated plaintext or authentication match. They **do not
prove the keys are unusable under every transformation**: firmware cipher,
key derivation, authenticated ranges, IV policy and exact body offsets are
unknown. The opaque header field is not established as a CMAC at that position.
Our conventional vector filter is not a general processor detector. Random
bytes producing disassemblable instructions would not count as success.

Known AES and CMAC vectors and the original resource ciphertext pass the probe
controls. Detailed output hashes and every trial parameter remain in ignored
`analysis/followup/crypto-audit/key-reuse-probes.json`; compact measurements and
source hashes are in `ram-debug-findings.json`.

## USB recovery and RAM-loading leads

| Route | Evidence | What is still missing |
| --- | --- | --- |
| Normal update reports `0xf4/0xf5` | Public updater and traced Sony library transfer headers/body and poll verification | No exposed arbitrary RAM destination or execute operation; controller authentication remains |
| Feature report `0xa0` | Public DualSense descriptor declares a one-byte payload; a community report table labels it DFU enable | Verified meaning/value, BDM-040 applicability, entry/exit, loader descriptors and accepted image policy |
| Factory/test reports `0x80/0x81` | Published calibration tools query known info and alter calibration/settings | No demonstrated arbitrary-address RAM read/write or execute primitive |
| Primary/secondary loader modes | Community wiki names the modes | Its entry procedures remain explicitly undocumented; no proven unsigned RAM loading |
| SWD/JTAG or a ROM serial monitor | General mechanisms that could permit RAM loading on suitable hardware | CXD90071GG-specific pinout, voltage, enabled/authenticated debug state and required hardware |

The pinned public updater's
[state machine](https://github.com/nchie/dualsense-updater-rs/blob/c1ef2cacffa36d5c20867178af06afa3e92daa2f/src/update.rs)
implements sequential transfer, status polling, verification and finalization.
It supplies no separate RAM-upload/jump facility. An internal staging buffer
would not imply the controller will execute its contents.

The independently published
[DualSense USB descriptor](https://github.com/nondebug/dualsense/blob/02338358b6b4be2d64dbf661ae2a47842156cfcf/report-descriptor-usb.txt)
really does contain feature report `0xa0`, with eight-bit report size and count
one: one payload byte, plus the report ID. The captured specimen is labelled
CFI-ZCT1W, not verified BDM-040. Its 273-byte descriptor is not a fresh capture
from our pad. A vendor-defined report declaration describes size, not command
semantics or whether an operation changes persistent state.

The community label comes from the
[report summary](https://controllers.fandom.com/wiki/Sony_DualSense/Report_Summaries),
which itself asks for documentation. It is a lead to investigate in authentic
factory/loader code, not enough information to send a test value safely.
Likewise, the
[DFU modes note](https://www.psdevwiki.com/ps5/index.php?title=DualSense_DFU_Modes)
names primary/secondary modes but leaves their triggers unknown.

### Why the DualShock 4 recipe is not our recovery procedure

Al's original
[DS4 DFU experiment](https://blog.the.al/2023/01/03/ds4-reverse-engineering-part-3.html)
reports a JDM-055 entering USB `054c:0856` and remaining in DFU through reconnects
and physical resets. The author eventually returned it using a loader command;
this was not a permanently bricked controller. The experiment neither installs
custom code nor establishes a DualSense/BDM-040 recovery sequence.

The same researcher's
[DualSense calibration work](https://blog.the.al/2024/04/02/calibrating-dualsense.html)
shows that report IDs and subcommands changed between generations. NVS unlock
controls calibration persistence; it is not an executable-image authentication
unlock. The known in-memory-module query in the pinned calibration source
returns twelve 16-bit calibration values, not a caller-selected RAM range.

### What the board photographs establish

We re-inspected RDC's published BDM-040 top/bottom scans. The main package
marking supports the existing **CXD90071GG** identification. The
[PCB scan thread](https://acidmods.com/forum/index.php?topic=44714.0)
does not supply a verified SWD/JTAG/UART pinout; visible test pads and button
connections are not sufficient to assign one. Older revision connector
pinouts on that page are explicitly revision-specific.

No CXD90071GG programming/debug manual or demonstrated BDM-040 RAM payload was
established in the reviewed public material. CPU ISA, RAM map and debug state
remain unverified. Generic Cortex-M addresses, older Renesas part descriptions,
and debug interfaces on external Pico/ESP32 dongles cannot fill those gaps.

## Direction under the current no-tools constraint

There are two software-only possibilities still worth pursuing: a genuine
loader/factory protocol that explicitly loads and executes RAM, or a runtime
entry point in the existing USB firmware. Neither is demonstrated here.
Opening a debugger on the Mac cannot create a target debug interface that the
controller does not already expose.

The highest-value next evidence is authentic factory/recovery tooling or a
plaintext loader/firmware dump for the matching family. An older official PC
updater may be easier to inspect; both legacy Sony installer URL spellings
checked in this pass returned HTTP 404. No substitute downloader or mirrored
installer was executed. A spare's read-only identity snapshot could also tell
us whether it belongs to this same family before planning any later experiment.

Before entering any candidate loader over USB, establish both the exit behavior
and whether entry changes persistent state. Before sending a payload, establish
its destination, authentication requirements and executable memory map. RAM-only
storage does not by itself guarantee an entry procedure or running code cannot
write flash. This is why this pass does not ship a speculative DFU button or
payload uploader for the user's controller.

If a suitable RAM route is found, the first payload should return a fixed USB
marker and a counter, then reset cleanly. A button-driven tiny game follows
that proof. USB bandwidth and FUZIX sizing remain later questions.

## Reproduction

With the previously extracted, hash-pinned updater and local metadata evidence:

```sh
python3 research/dualsense/unpack_resources.py
python3 research/dualsense/probe_resource_keys.py
python3 -m unittest discover -s research/dualsense -p 'test_*.py' -v
```

The tools read local files only. Full decoded Sony resources stay ignored.
Thirty tests now cover the existing corpus/archive/trace checks, complete
numeric parsing, malformed input, candidate encoding and known crypto vectors.
