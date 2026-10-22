# DualSense hardware and updater investigation

Observed on 2026-10-21. The official Windows updater is a useful reverse
engineering target: its managed application and native update library are
readable host code. The controller firmware embedded inside it remains the
same opaque container already collected. REA can help trace the host side;
obtaining controller plaintext and making modified code execute are still
separate unresolved steps.

The [subsequent REA and Ghidra trace](rea-trace.md) implements this setup,
recovers the hidden managed update calls and identifies the alternate packets
as the Bluetooth transport. The observations below record the initial audit.

This pass used files and public sources only. It did not communicate with the
controller or run Sony's installer, application, DLL or firmware.

## Hardware revision

The previously saved feature report contains hardware version `0x00000616`.
The public calibration tool's `hwToBoardModel` maps the byte at bits 8–15 to a
board revision: `0x06` means **BDM-040**. This is a source-supported community
identification; the user's physical PCB has not been opened and inspected.

[Pinned decoder source](https://github.com/dualshock-tools/dualshock-tools.github.io/blob/eb9af384c3372dac966bf26660ff90a20b9cf5d5/js/controllers/ds5-controller.js).

RDC's published BDM-040 motherboard scan has a legible **SIE CXD90071GG**
package on Side A. The downloaded image is retained with its hash in
`updater-findings.json`. This identifies the marking on that photographed
revision, not the instruction set, available RAM, flash arrangement or debug
security of the user's individual chip. Earlier DualSense MCU descriptions
must not be treated as a verified CXD90071GG datasheet.

[RDC's PCB scans and revision labels](https://acidmods.com/forum/index.php?topic=44714.0),
[BDM-040 Side A](https://www.acidmods.com/RDC/DS5/BDM-040/BDM-040_TOP.jpg),
[Side B](https://www.acidmods.com/RDC/DS5/BDM-040/BDM-040_BOTTOM.jpg).

Sony's public FCC internal-photo filing for `AK8CFIZCT1B` is document 6414748.
The PDF download was refused with HTTP 403 by the hosts tried in this pass;
the MCU observation above therefore uses RDC's directly inspected scan.
The public metadata for permanently confidential block diagrams and schematics
does not provide the actual technical drawings.

## Calibration access does not establish code execution

The public DS5 calibration source uses feature reports `0x80` and `0x81` as a
command/response pair. Known operations retrieve identification and calibration
data, run stick calibration and control whether calibration settings persist.
The source's `flash()` method unlocks and locks NVS to save those settings.
Its in-memory module data is a fixed calibration structure, not an exposed
arbitrary-address RAM reader. These operations do not demonstrate an unlocked
bootloader or a way to install executable code.

The saved feature report additionally has words named SBL, Venom and Spider
firmware versions by this source. These names are clues for future correlation,
not a verified identification of the three update-container regions.

[Pinned DS5 implementation](https://github.com/dualshock-tools/dualshock-tools.github.io/blob/eb9af384c3372dac966bf26660ff90a20b9cf5d5/js/controllers/ds5-controller.js),
[independent public tester implementation](https://github.com/daidr/dualsense-tester/blob/62dce1703a468a20af901db5e31e476ce5ec24b3/src/utils/dualsense/ds.util.ts).

## Official updater availability

Sony's current **PlayStation Accessories** application supports Windows 10
64-bit and Windows 11. Its official download page offers a Windows installer;
no official macOS edition is listed. Controller support on macOS does not
imply that Sony supplies a Mac firmware updater. Community update tools are
separate projects.

[Sony application and system requirements](https://controller.dl.playstation.net/controller/lang/en/2100004.html).

The downloaded official installer is 76,977,440 bytes, SHA256:

```text
37d2d7337e83558486fb4a811368ec76f2d5cedb329207040e9b5a55a9029e43
```

Its outer launcher is a 32-bit Windows PE containing an InstallShield
`ISSetupStream` v4 archive with 66 members. Static decoding recovers
`PlayStation® Accessories.msi`; its embedded `Data1.cab` contains the application,
native libraries and three firmware containers. All extraction was performed
as archive parsing, without Windows emulation or vendor code execution.

The updater is kept in ignored `analysis/followup/`. Its extracted MSI hashes to
`15d8f3f380ad689f3df76c45baffa8c527949368e24dedfaa6d1469285ee2b4b`.
The InstallShield encoding is ordinary installer packaging and is unrelated to
the transformation protecting the controller firmware bodies.

## Native update library

`libfwupdater.dll` is a 226,768-byte x86-64 native Windows library, SHA256:

```text
3bc6de1116a7ea29d0997f632613edfe7c3100b62efbf5bca6f32f9fb4bc4569
```

Its PE export table supplies 54 names, including `StartUpdate`,
`WriteUpdateImage`, `VerifyUpdateImage` and `FinalizeUpdate`. A debug-directory
record references `D:\PSDEV\fwupdater\Application\2.0\x64\Release\libFWupdater.pdb`;
this is a build-machine path, not an included symbol database. The package does
not contain a `.pdb` file.

The following observations come from static x86-64 disassembly. Addresses below
are RVAs for this exact DLL, with preferred image base `0x180000000`.

| Function or site | Observed operation |
| --- | --- |
| `StartUpdate`, RVA `0x1b90` | Copies a 256-byte caller-supplied header into update feature packets, then polls status |
| `WriteUpdateImage`, RVA `0x1ea0` | Copies a 32 KiB caller-supplied block into successive packets, polling status between packets |
| `WriteUpdateImage`, RVA `0x1fe2` | Calls a byte-copy routine at RVA `0x233b0` with source `caller buffer + offset` |
| `VerifyUpdateImage`, RVA `0x21b0` | Sends update operation 2, polls the controller, and maps returned status values to host errors |
| `FinalizeUpdate`, RVA `0x23b0` | Sends update operation 3 |
| Send wrapper, RVA `0x4e40` | Copies the packet into a transport buffer, then calls the lower feature-report sender |
| Lower sender, RVA `0x9f10` | Reaches `HidD_SetFeature` through the thunk at RVA `0x12780` |
| Lower receiver, RVA `0x9d00` | Reaches `HidD_GetFeature` through the thunk at RVA `0x1277a` |

The update functions select a 64-byte packet with up to 57 payload bytes or an
alternate 547-byte packet with up to 539 payload bytes. The ordinary path uses
report `0xf4` for commands and `0xf5` for status. The alternate path selects
`0xf6` and `0xf7`; its exact device/transport applicability needs caller and
device-type tracing before use.

These paths packetize the supplied bytes rather than decrypting firmware.
In particular, patching the host `VerifyUpdateImage` result would only suppress
the host's interpretation of a controller response. It would not make the
controller accept an invalid image. This trace does not reconstruct all code in
the managed caller, the controller's verifier, or its boot chain, and does not
establish whether a decryption key exists elsewhere in the application.

The installer embeds `FWUPDATE0004`, `FWUPDATE000B` and `FWUPDATE0044`. All three
are byte-for-byte identical to the respective current-version images in our
verified corpus. The `0004` image is the previously matched `0630` build. There
is no newly recovered plaintext firmware copy in these three embedded files.

## REA assessment

Assessed source revision:
[`2947a775070863e15e802194fbb14bfd1ec18b9f`](https://github.com/morluto/rea/tree/2947a775070863e15e802194fbb14bfd1ec18b9f).
The source package declares version 6.7.0; this is a source assessment, not a
claim that the same version and features are currently installed or published
to npm.

| Target | Useful REA capability | Boundary |
| --- | --- | --- |
| `libfwupdater.dll` | Ghidra/Hopper/IDA functions, pseudocode, calls, cross references and strings | Requires a separate analysis engine; no Windows target execution is necessary |
| `playstationaccessories.dll` | Built-in .NET metadata, CIL, declared native boundaries and call relationships | Static CIL inspection is not reconstructed C#; optional ILSpy can help with that |
| Other updater versions | Native function matching and managed member comparisons | Match the exact build/hash and preserve ambiguous mappings |
| Firmware containers | Binwalk/Unblob region inspection and extraction | Documented firmware lane is Linux x64; format recognition does not decrypt unknown bodies |
| A future plaintext MCU image | Ghidra raw analysis with a correct CPU and memory map | Stock REA raw mode explicitly excludes Cortex-M and Thumb-only targets |

The managed application is a 1,390,544-byte PE/CLI assembly, distinct from the
native DLL. Its SHA256 is retained in `updater-findings.json`. REA's managed
tools are particularly relevant for finding where it selects the firmware
family, reads files, checks header fields and invokes the native update API.

The native Ghidra provider supports macOS arm64 hosts. A compatible full JDK
and a Ghidra installation with a native arm64 decompiler are required. The
source package requires Node `^22.19.0 || ^24.11.0 || >=26.0.0`; the current shell
has Node 22.16.0, so that executable is below its declared minimum. No REA
installation, client registration or host configuration changes were made.

REA is an agent-facing interface to analysis tools, not a replacement for
cryptanalysis or hardware access. Ghidra itself can support Cortex-M analysis;
the limitation above is REA's documented stock raw-import profile. It can be
addressed later with a suitable manual Ghidra project or an explicit adapter
extension after plaintext and the actual processor are established.

[REA native provider](https://github.com/morluto/rea/blob/2947a775070863e15e802194fbb14bfd1ec18b9f/docs/ghidra-provider.md),
[managed analysis](https://github.com/morluto/rea/blob/2947a775070863e15e802194fbb14bfd1ec18b9f/docs/managed-code-analysis.md),
[firmware analysis](https://github.com/morluto/rea/blob/2947a775070863e15e802194fbb14bfd1ec18b9f/docs/firmware-analysis.md),
[raw import limits](https://github.com/morluto/rea/blob/2947a775070863e15e802194fbb14bfd1ec18b9f/docs/raw-binary.md),
[package requirements](https://github.com/morluto/rea/blob/2947a775070863e15e802194fbb14bfd1ec18b9f/package.json).

## Next investigation

1. Use REA plus Ghidra for the native DLL and its built-in managed tools for
   the application. Trace the actual file-to-update call chain and family
   selection. Establish whether any host stage transforms the container before
   the native calls.
2. Compare an older official updater, if available from a verifiable source.
   Changes in validation, transport or diagnostics may clarify the protocol.
3. Seek CXD90071GG-specific public documentation, a genuine plaintext dump or a
   demonstrated debug/code-execution route. The photographed pads are not a
   verified SWD/JTAG pinout.
4. Once execution is reproducible, use an observable minimal payload before a
   Brainfuck interpreter and USB console. Measure usable memory before sizing
   FUZIX or a Ripple interpreter.

## Reproduction

Download the installer from Sony's linked endpoint and verify its SHA256 above.
The decoder refuses other bytes and an existing output destination:

```sh
python3 research/dualsense/extract_updater.py \
  research/dualsense/analysis/followup/PlayStationAccessoriesInstaller.exe \
  research/dualsense/analysis/followup/updater

unar -q -o research/dualsense/analysis/followup/msi \
  'research/dualsense/analysis/followup/updater/PlayStation® Accessories.msi'
unar -q -o research/dualsense/analysis/followup/payload \
  'research/dualsense/analysis/followup/msi/PlayStation® Accessories/Data1.cab'

python3 research/dualsense/test_updater.py -v
objdump -p research/dualsense/analysis/followup/payload/Data1/libfwupdater.dll
objdump -d research/dualsense/analysis/followup/payload/Data1/libfwupdater.dll
```

The archive decoder is based on
[ISx's documented layout and transformation](https://github.com/Coldblackice/InstallShield-installer-extractor-ISx/tree/098e866fa5341db4424d3831d40943c01b88aefe),
with the original MIT notice preserved in the Python file. Extracted vendor
files remain local ignored research inputs and are not relicensed sources.
