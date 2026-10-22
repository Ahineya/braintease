# PS Remote Play for Mac: controller transport audit

Sony's Mac client provides clearer controller names and firmware structures,
but this pass found no RAM upload or execute command. Its reviewed HID paths
cover identity, calibration, Bluetooth control and ordinary controller output.
The useful new result is a connection between named firmware-version fields
and two regions in our saved controller firmware.

This 2026-10-21 pass was static only. The installer was expanded without
installation; neither Sony's app nor its libraries were executed. No controller
was opened and no USB reports were sent. The existing USB allowlist is unchanged.

## Input and provenance

The download came from the Download link on Sony's
[PS Remote Play for Mac page](https://remoteplay.dl.playstation.net/remoteplay/lang/en/1100002.html):

```text
https://remoteplay.dl.playstation.net/remoteplay/module/mac/RemotePlayInstaller.pkg
12,571,186 bytes
SHA256 a82a7f645ff009a0c70d4b989f34249a51d800ca7aa8b4bf81772ad7d59a6a82
```

The extracted app identifies itself as `com.playstation.RemotePlay`, version
**9.5.0**, minimum macOS 12.0. Its main executable contains x86-64 and arm64
slices. This pass follows the arm64 slice, SHA256
`02882fb5005ef08e1256a59ce28debd484d54f1413e7f97d36eb2f219019240a`.

With normal macOS certificate access, `pkgutil --check-signature` reports a
trusted Sony Developer ID Installer signature, Apple notarization and a trusted
timestamp of 2026-08-04 09:25:49 UTC. The extracted app passes
`codesign --verify --deep --strict`. Both use Sony team `8UT4NVUACP`.
Initial checks inside the restricted sandbox misleadingly reported invalid
signatures; repeating the checks with normal certificate access resolved that
failure. Both sets of transcripts are retained.

The original package, extracted files, disassembly and analysis outputs remain
under ignored `analysis/remoteplay/`. [remoteplay-findings.json](remoteplay-findings.json)
records URLs, input and evidence hashes, report callers and observations. Vendor
binaries remain research inputs under their original ownership and licensing.

## Where the controller code lives

The controller transport is in the **main RemotePlay executable**, rather than
a separate pad library. The bundled dylibs are `libscenp` and three Unified
Telemetry libraries; their examined symbol inventories contain no direct HID
report imports.

The main executable retains Objective-C metadata for classes including
`IOHIDDeviceObject`, `DSDevice`, `DSManager` and
`DSDeviceOutputReportObject`. Decoding relative method lists and chained
pointers recovered 960 method records; decoding 383 selector stubs let us
annotate direct callers. These are recovered records, not a count of every
Swift/C/C++ function or every possible dynamic dispatch target.

Product-ID tests establish these internal names:

| Test | Product ID |
| --- | --- |
| `isBond` | `0x0ce6`, our standard DualSense |
| `isHunt` | `0x0df2`, DualSense Edge |
| `isAston` | `0x0e45` or `0x0e46` |
| `isLeonardo` | `0x0e5f` |
| `isClone` | `0x0ba0`, handled by `ScePadDS4ViaClone` |

`Clone` is an internal transport name here; the predicate is a product-ID
comparison, not a counterfeit-controller detector. Other families' report
numbers must not be treated as commands for our `0x0ce6` specimen.

## Actual report paths

Four direct HID report call sites were identified in the arm64 text:

| Address | Operation | Enclosing method |
| --- | --- | --- |
| `0x1000e3b14` | SET Output report | `output:length:` |
| `0x1000e3ba8` | GET Feature report | `getFeature:length:` |
| `0x1000e3c90` | SET Feature report | `setFeature:length:` |
| `0x1000fe2e8` | GET Feature `0x12`, 16 bytes | `DSDevice getBtPairing:` |

The generic wrappers take the report ID from buffer byte zero. Recognized
selector stubs have 16 direct calls to `getFeature:length:`, two to
`setFeature:length:` and twelve to `output:length:`. This inventory is bounded
to direct calls and recognized stubs, rather than proof against other dynamic
Objective-C invocations.

For Bond/Hunt, the examined query callers use:

| Report | Requested bytes, including ID | Purpose |
| --- | ---: | --- |
| `0x09` | 20 | Read Bluetooth address into the host object |
| `0x20` | 63 | Firmware information |
| `0x05` | 40 | Calibration read, converted to the host motion library's format |

These lengths are the app's actual requests, not a replacement for the
controller's descriptor or our workbench's known-good sizes. In particular,
the firmware method allocates 64 bytes, requests 63 and copies the 63-byte
payload beginning after the report ID. The last byte of that copy therefore
comes from the zero-initialized tail of the buffer.

`setBDAddress` at `0x1000e3cd4` sounds like a controller setting, but its reviewed
implementation performs GETs and stores the resulting address in the **host
object**. REA/Ghidra's retained decompilation agrees with the annotated arm64
instructions. It is not evidence of a command to rewrite the controller's
Bluetooth identity.

Other families explain several additional IDs: the default DS4 firmware read
uses `0xa3`, calibration uses `0x02`, and adapter paths use `0xe1`, `0xe3`,
`0xe5` and `0x15`. `0xa3` must not be confused with the excluded `0xa0` report.

The setting paths also have ordinary purposes:

- `setBtControlCommand:` builds model/bus-specific feature reports `0x08`,
  `0x14` or `0xe2`. `DSDevice disconnectBT` calls it with command value 2.
- `setAudioOutPath:` uses the ordinary output payload for Bond/Hunt and a
  three-byte `e0 00 <path>` feature report on its other branch.
- `DSDevice test` sends a fixed 76-byte Output report starting with `0x05`
  in a 100-iteration loop, changing output flag bytes. It does not construct
  a memory-address request or an execute command.
- Normal Bond/Hunt output uses `0x02` over USB and `0x31` over Bluetooth,
  carrying LED, vibration, trigger and audio-control fields.

None of these settings or test paths was exercised on hardware.

## Firmware component names: a useful cross-check

The Objective-C type encoding retains the complete packed
`_bond_payload_get_firmware_info` field names. Its tail names
`sbl_firmware_version`, `venom_firmware_version`,
`spider_dsp_firmware_version` and `crc32`. The payload occupies 63 bytes;
offsets below include the report ID at byte zero.

Applying these labels to the **previously saved** `0x20` reply gives:

| Sony field | Report offset | Value | Matching saved container field |
| --- | ---: | --- | --- |
| `sbl_firmware_version` | 48 | `0x0001003c` | Not identified |
| `venom_firmware_version` | 52 | `0x0002000a` | `0x4f04c`, in the VenomB region |
| `spider_dsp_firmware_version` | 56 | `0x00000006` | `0xaf05c`, in the BettyBND region |

Both comparisons use our matching `FWUPDATE0004 / 0630` container, SHA256
`861ad95a99b16805ff20f5690372988662c4f69c6ad0b266e793d7fc2292315a`.
The values match exactly as little-endian 32-bit words.

The Venom name and value corroborate one another. The Spider DSP value matching
BettyBND supports associating that container region with Sony's DSP-version
field; that association is an inference from this specimen, not a recovered
hardware block diagram. Likewise, `sbl` suggests a bootloader version but does
not identify a writable loader interface. These fields establish neither CPU
instruction sets nor RAM sizes. The `crc32`-named field is zero in our saved
USB reply; no checksum verification is claimed.

## Consequences for the first payload

No caller for features `0x0b` or `0x0c`, the `0x80/0x81` factory transport, or
report `0xa0` was identified in these reviewed paths. The Mac client therefore
does not explain our opaque `0x0b` data or the failed `0x0c` GET. No controller
firmware decryption, arbitrary-address memory operation or RAM execution path
was recovered. This is a limit of the inspected host code, not proof that the
firmware contains no such handler or no exploitable parser.

For further source work, a Windows Remote Play transport comparison is a
reasonable distinct target: it may package pad code or retain different
symbols. For the existing hardware, a second specimen's known-size `0x0b`
capture remains a useful way to classify device-specific versus constant
bytes. Neither step requires trying the unidentified loader path. The new
component labels also give us better terms and offsets for future firmware
and hardware research.
