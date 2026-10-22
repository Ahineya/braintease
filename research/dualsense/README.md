# DualSense firmware research

The offline corpus contains 17 original Sony firmware containers, covering four
updater families. The connected controller's reported build metadata matches
`FWUPDATE0004 / 0x0630`. Readable headers expose a stable three-region layout,
but the bodies have statistical properties consistent with encryption. This
pass recovers container metadata, not executable ARM instructions or an unsigned
firmware installation method.

The research directory is independent of the compiler and operating-system
builds. The firmware/archive/updater analysis tools operate on files only.
The explicitly invoked `usb_probe.py` workbench accesses the connected
controller using a restricted set of information queries and optional ordinary
output controls. No tool here invokes the updater or executes firmware.

The [hardware and official updater follow-up](updater-and-hardware.md) identifies
the likely BDM-040 revision, inspects Sony's native update library, and assesses
REA as an analysis frontend. It includes a reproducible static installer
decoder and records the remaining barriers to custom code execution.

The subsequent [REA and Ghidra trace](rea-trace.md) recovers 1,719 hidden managed
metadata targets and follows the firmware file through the memory buffer into
native HID transfer. It also identifies the larger packets as the Bluetooth
update transport.

The [host crypto and recovery investigation](crypto-and-recovery.md) decodes the
updater's protected resources into 2,560 strings and a numeric cache. These are
host application assets, not recovered controller instructions. It also records
device-side version/authentication barriers and the missing recovery evidence
before our first custom payload or game.

The [BDM-040 RAM/debug and resource audit](ram-debug-and-resource-audit.md)
examines the decoded contents, completes all numeric arrays and tests a finite
set of resource-key reuse hypotheses. It distinguishes the unverified DualSense
DFU lead from demonstrated DS4 behavior, and records what can be pursued without
electronics tools. No recoverable custom RAM execution path is established yet.

The subsequent [live USB experiments](usb-experiments.md) exercise known factory
queries, bounded framing and brief ordinary output controls. They establish
consume-on-read factory replies on this controller/host combination, preserve
unchanged calibration readbacks and identify two additional descriptor reports
for further investigation. `usb-findings.json` records the measurements and
transcript hashes; loader and update operations remain excluded.

The [factory report trace](factory-report-trace.md) follows Sony's telemetry
reader, confirms its six-chunk response on the controller and decodes the named
numeric fields locally. GET-only samples of `0x0b` yield stable opaque data;
`0x0c` rejects the tested GET. `0xa0` remains blocked. The trace also narrows
the earlier consume-on-read observation to the simple queries actually tested.

The [controller DLL survey](dll-survey.md) distinguishes Edge settings from
debug facilities. The subsequent [PS Remote Play for Mac audit](remoteplay-audit.md)
maps the other app's HID callers and ties Sony's Venom and Spider DSP version
fields to matching words in the saved firmware regions. It uses static analysis
only and establishes no RAM execution or firmware decryption route.

The [Windows Remote Play audit](remoteplay-windows-audit.md) unpacks Sony's 9.5
and 9.0 packages and traces the readable host preloader. Their protected native
code prevents a report-level transport comparison in this static pass; the
audit records that boundary and the next host-analysis steps without treating
host memory loading as a controller capability.

The [runtime bootstrap follow-up](runtime-bootstrap-followup.md) combines static
analysis with bounded offline CPU emulation. It identifies the initializer for
the runtime's preloader-table pointer, maps an 18-entry Windows API array and
decodes a native string family. It does not launch the Windows application or
access the controller. The saved reproducer includes synthetic containment
controls and explicit limits on the conclusions.

The [bootstrap continuation](runtime-bootstrap-continuation.md) follows table
validation and the import copy into a Windows export walk. It explains the
preloader's handled-fault module discovery and tests a pinned Windows DLL as
non-executable data. The observed `Zw` export catalog is host initialization;
controller execution and the main application unpacker remain unresolved.

## Files and reproduction

From the repository root:

```sh
# Network access: obtain missing images from Sony, or the pinned archive.
python3 research/dualsense/fetch.py

# Everything below is offline.
python3 research/dualsense/fetch.py --verify-offline
python3 research/dualsense/test_analysis.py -v
python3 research/dualsense/analyze.py
```

- `firmware/` contains the unmodified images, grouped by device, family and
  version. Files are made read-only and excluded from Git.
- `catalog.json` is the upstream catalog at the revision pinned in
  `sources.json`; every image has an expected size and SHA256. The catalog itself
  is also hashed. `sony-info.json` records Sony's version metadata on 2026-10-21.
- `collection.json` records actual download URLs and fallback failures from this
  collection. `fetch.py` prefers Sony; a network failure permits the pinned
  archive fallback, while an incorrect size or hash fails immediately. Existing
  files must verify and are never overwritten.
- `controller.json` preserves the previously observed 64-byte firmware feature
  report and model identity, without serial numbers or Bluetooth addresses. It
  is an information report, not a dump of the controller's flash.
- `findings.json` preserves this analysis result, including input hashes, region
  statistics and comparisons. `analyze.py` regenerates the result under
  `analysis/results.json`, with per-image 4 KiB entropy maps in CSV files.

All 17 images passed size and SHA256 verification: **16,154,624 bytes total**.
Each container is **950,272 bytes**, or **928 KiB**. Four came from Sony's live
download endpoints and thirteen from the pinned archive after Sony returned
HTTP 404. Matching the archive hashes establishes corpus consistency; it does
not independently verify Sony's cryptographic authentication tags.

The firmware retains Sony's original ownership and licensing. The original
binary files are local research inputs, not relicensed repository sources.

## Controller identification

Observed on 2026-10-21 using a read-only `GET_REPORT` of feature report `0x20`:

| Field | Value |
| --- | --- |
| Rear label | CFI-ZCT1W B |
| FCC identifier | AK8CFIZCT1B |
| USB vendor and product | `054c:0ce6` |
| Connection | USB |
| Build date | `Jul  4 2025` |
| Build time | `10:10:32` |
| Hardware version | `0x00000616` |
| Internal firmware version | `0x0110002a` |
| Update version | `0x0630` |

The decoder follows the offsets in Linux's
[`dualsense_get_firmware_info`](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c):
hardware at byte 24, internal firmware at 28, and the 16-bit update version at 44.
The date occupies bytes 1 through 11 and the time bytes 12 through 19; byte 0 is
the report identifier. The USB descriptor's `bcdDevice=0x0100` is a separate field.

Exactly one catalog header matches the date, time, internal version and update
version together:

```text
firmware/dualsense/type-0004/0x0630/FWUPDATE0004.bin
SHA256 861ad95a99b16805ff20f5690372988662c4f69c6ad0b266e793d7fc2292315a
```

The `000B / 0x0630` image instead reports `10:38:40` on the same date. The
comparison identifies the matching published build family, rather than relying
on the shared update-version number. It does not establish byte-for-byte
identity with installed flash, the exact BDM board number, RAM size, or CPU type.

Separately, a public hardware-version decoder maps this report to BDM-040;
see the follow-up for the source and distinction from physical PCB inspection.

## Firmware inventory

| Family | Device | Versions in this corpus |
| --- | --- | --- |
| `0004` | Standard DualSense | `0297`, `0307`, `0356`, `0402`, `0458`, `0520`, `0630` |
| `000B` | Standard DualSense | `0520`, `0630` |
| `000E` | Standard DualSense | `0633`, `0641` |
| `0044` | DualSense Edge | `0113`, `0154`, `0180`, `0200`, `0213`, `0217` |

Version values are hexadecimal. This is the pinned archive's available set,
not a claim to contain every firmware Sony has ever released. Sony's captured
metadata lists `0630`, `0630`, `0641` and `0217` respectively as current versions.

## Container structure

All 17 files contain copyright headers at the same three page-aligned offsets.
The main region also contains an additional readable description at `0x80`.

| Region | File start | Allocated span | Labels across the corpus |
| --- | --- | --- | --- |
| Main | `0x000000` | `0x4f000` = 316 KiB | Wireless controller 1.01, 2.10, 2.20; Rhino Main Firmware |
| Second | `0x04f000` | `0x60000` = 384 KiB | `VenomB`, `GwenB`, `VenomH` |
| Third | `0x0af000` | `0x39000` = 228 KiB | `BettyBND`, `BettyHNT` |

These are observed container regions. A label does not by itself identify a
processor or prove that a region is an independently bootable image. File
offsets and allocated spans must not be treated as a hardware memory map.

For the matching `0004 / 0630` image, these main-header fields are readable:

| Offset | Observed interpretation or value |
| --- | --- |
| `0x40` | Build date, 16-byte field |
| `0x50` | Build time, 16-byte field |
| `0x60` | 16-bit word `2`; meaning unconfirmed |
| `0x62` | USB product identifier `0x0ce6` |
| `0x64` | 32-bit word `0x00100000`; meaning unconfirmed |
| `0x68` | `0x0004f000`, matching the first region's span |
| `0x6c` | 32-bit word `0x00001300`; meaning unconfirmed |
| `0x70` | Internal firmware version `0x0110002a` |
| `0x74` | Family value `4`, matching `FWUPDATE0004` |
| `0x78` | Update version `0x0630` |
| `0x80` | Readable description containing Wireless controller 1.01 |
| `0xe0` | Packaging timestamp candidate `20250704101343` |
| `0xf0` | Opaque 16 bytes; role unconfirmed |

The public updater sends the first 256 bytes as the initial update header. The
opaque field at `0xf0` could be cryptographic metadata, but this analysis does
not establish whether it is a CMAC, IV or something else.

The second and third regions have readable metadata through relative `0x5f`,
followed by high-entropy bytes and a trailing `FF` run. Their update-version
fields match the main header in every image. Candidate content-length fields
are at relative `0x48` for Venom/Gwen and `0x58` for Betty. In every case:

```text
region start + candidate length + 0xc0 = start of trailing FF padding
```

The fixed `0xc0` overhead is an observation, not a decoded authentication format
or proof that the first `0xc0` bytes are all headers.

## Body statistics and comparisons

Analysis excludes the first `0x100` bytes of the main region and the first `0x60`
of each secondary region. For entropy/compression statistics it additionally
removes a uniform zero or `FF` suffix of at least 64 bytes. This prevents unused
space from being mistaken for readable or compressible firmware.

For the matching image:

| Region | Bytes after exclusions | Shannon entropy in bits per byte | zlib output divided by input |
| --- | --- | --- | --- |
| Main | 323,328 | 7.999427 | 1.000328 |
| VenomB | 389,728 | 7.999420 | 1.000323 |
| BettyBND | 217,440 | 7.999237 | 1.000350 |

Across all 51 regions, entropy after these exclusions ranges from **7.998991 to
7.999604**. No region has repeated nonuniform, aligned 16-byte blocks. The
repeated blocks in secondary spans are their `FF` padding.

Every pair of images was compared for shared nonuniform 16-byte values within
the corresponding region: **136 pairs per region, 408 comparisons total**.
There were **zero shared values**, including comparisons at different block
indices. This scan covers blocks aligned to each selected body start; it does
not search every possible byte alignment or compare different region indices.

For a concrete same-family upgrade, `0004 / 0520` versus `0004 / 0630`, only
1,290 of the 323,328 main-body bytes match at the same offset: **0.39898%**.
Independent uniform bytes would match about **0.390625%** of the time. The
longest equal main-body run is two bytes. Large equal ranges elsewhere are
readable headers or `FF` padding, not shared executable functions.

Together, the high entropy, lack of compressibility and absence of shared
blocks are consistent with encryption or another strong transformation of the
bodies. They do not establish a cipher, key, mode, IV policy or secure-boot
implementation. In particular, they do not prove AES-CBC rather than CTR or
another mode. Compression can also produce high entropy, so the statistics
alone are not a decryption proof.

No ELF, ZIP, XZ, Zstandard or framed LZ4 signatures were found. Two gzip magic
matches occurred, but neither decoded as a valid gzip stream. Bounded,
checksum-validated zlib/gzip probes decoded no streams. These checks do not
cover raw compression, custom wrappers, encrypted compression or every codec.

The conventional Cortex-M4 vector-table filter found no candidates in any
image. It checks word-aligned tables with plausible RAM stack pointers, Thumb
handler addresses and zero reserved slots. Synthetic valid tables are detected
by the tests. The negative result does not exclude Cortex-M code behind
encryption, relocation or a nonstandard image layout.

## Consequences for custom software

The images support identifying the controller's matching build family and
studying packaging changes over time. They do not currently support ordinary
function-level ARM diffing. A disassembler accepting arbitrary bytes would not
be evidence of recovered instructions.

Decryption and acceptance of modified firmware remain separate problems. The
pinned updater's
[`protocol.rs`](https://github.com/nchie/dualsense-updater-rs/blob/c1ef2cacffa36d5c20867178af06afa3e92daa2f/src/protocol.rs)
names header and firmware-body CMAC failures, plus version and capability
checks. Those host-side status names suggest keyed update authentication; they
do not describe the complete controller-side verification or boot chain.
Recovering readable code would not automatically let us generate accepted
modified images.

The next useful investigation is the matching hardware family's SoC, boot/debug
interfaces and update/boot implementation. Further offline work can examine
the official host updater's format handling and public hardware documentation.
Randomly modifying these images is not an execution experiment with a known
recovery path. The public updater author also observed updates committing
without explicit verify/finalize commands, so omitting those commands is not a
dry-run mechanism.

Once there is a reproducible custom execution route, the first target should be
a small payload with observable output, followed by a Brainfuck interpreter
and USB console. FUZIX resource and driver work should follow measured usable
RAM and executable storage. Our
[RBF profile](../../ports/fuzix/rbf/README.md) provides a useful minimal feature
set, but its Ripple instructions and runtime-supplied peripherals still require
either an ARM-hosted Ripple interpreter or a native ARM platform port.

## Source references

- [Pinned firmware archive](https://github.com/Paliverse/DualSense-List-of-Firmwares/tree/d520913335e797be8497c805c15a039e6a8ddc3c).
- [Sony version metadata](https://fwupdater.dl.playstation.net/fwupdater/info.json).
- [Pinned public updater](https://github.com/nchie/dualsense-updater-rs/tree/c1ef2cacffa36d5c20867178af06afa3e92daa2f).
- [Linux controller report decoder](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c).
- [DualSense USB and HID research](https://github.com/nondebug/dualsense).

All numeric body statistics and container layout observations above come from
the locally verified corpus and can be regenerated with `analyze.py`.
