# DualSense: host crypto and the path to a first game

Investigated on 2026-10-21, using files and public sources only. No controller
reports, mode changes, calibration writes or firmware updates were attempted.

The immediate obstacle is an accepted custom program and a recovery route.
We have not reached either. RAM capacity and USB throughput become practical
game constraints after we can execute and observe a small payload.

The subsequent [BDM-040 RAM/debug audit](ram-debug-and-resource-audit.md)
completes the numeric table, inspects crypto call sites and tests explicit
resource-key reuse hypotheses. It also assesses the USB loader leads under
the user's no-electronics-tools constraint.

## What the updater's crypto helper actually does

The separate managed helper `0x06000aae` in the pinned PlayStation Accessories
2.2.1.2 assembly unpacks protected **host application resources**. We now have
decoded output, rather than just references to AES APIs:

| Resource | Protection observed | Decoded result |
| --- | --- | --- |
| Main assembly's `leyD`, 44,851 bytes | AES-256-CBC with PKCS#7 padding, then a custom DEFLATE variant | 43,560-byte envelope containing resource names and a 43,520-byte .NET assembly |
| Embedded `tSHbR`, 40,931 bytes | Same AES mode and compression variant | 187,002-byte string table; 2,560 entries |
| Embedded `cDKfK`, 1,027 bytes | Compression variant only | 1,912-byte numeric table; 349 int32, zero int64, seven float32 and 59 float64 constants |

The embedded assembly has two private resources named `tSHbR` and `cDKfK`.
Its assembly name is `yUMPVixsIcdt`; its metadata has one TypeDef and no
MethodDef table. It is a resource carrier, not a controller program.

Its metadata omits `#Blob`: REA's strict admission reports malformed metadata,
while `dnfile` reads the assembly/resource rows with a warning. The table counts
and resource names above use that partial parse and exact output hashes; they
do not claim successful CLR loading. No extracted code was loaded to test it.

The string table includes application UI strings, telemetry schemas, updater
paths and all four firmware filenames. The numeric array resolves constants
used in the loader: indices 151 and 152 are 120 and 121, respectively, matching
the container version bytes at `0x78` and `0x79`; index 44 is the shift count 8.
This supports the earlier file-loading trace. It does not expose a controller
authentication key.

The wrapper stores an XOR-obscured metadata header and a 16-byte IV. In these
three resources the decoded header starts with `licF8865AC8` and embeds a
32-byte host resource key. Algorithm selector 2 constructs .NET AES. Flags 3
mean decrypt and decompress; flags 1 mean decompress only. Merely finding a key
beside these resources is therefore not evidence of a firmware key.

The compression stream differs from RFC 1951: it has a one-bit final flag and
a **three-bit** block type, with 6=fixed Huffman, 2=dynamic Huffman, and 3=stored.
Stored length is XORed with `0xbb9e`, with no standard complemented length word.
That explains why ordinary raw/zlib/gzip decoders rejected the decrypted data.
The literal/length, distance and dynamic-tree machinery inspected here otherwise
follows the familiar DEFLATE layout. The resource decoder implements these
observed forms with bounded reads and a 2 MiB output limit.

The host assembly SHA256 is still:

```text
a5ccc41e04e960c867d479d3434e23d92044a731627f71646edb7b2b8dd4e409
```

Decoded output identities:

| Output | SHA256 |
| --- | --- |
| Resource-only assembly | `6c949c43476df6844e8aa251ecac14e254de0e874d398adf889716892c63ddec` |
| String table | `a4ba231afa857121c452a4cc9830bf1f887cb09691bd9933b64fa58ff1bda94f` |
| Numeric table | `d85979383fc233974b0e5d2368ce15bb0d04148ef586037a2358aa4260191527` |

The helper's direct callers provide independent context:

| Method token | Static use of unpacked bytes |
| --- | --- |
| `0x06000a4a` | Reads a manifest resource into a position-keyed dictionary of strings |
| `0x06000a50` | Unpacks a supplied byte array before a typed-array reader |
| `0x06000a58` | Reads a manifest resource into the numeric cache |
| `0x06000a61` | Reads resource names and an embedded assembly byte array, then calls Assembly.Load |

Our decoder reproduces the data transformations without calling Assembly.Load,
starting a CLR, or executing any extracted assembly. Full original-byte-checked
CIL listings and local binary outputs are retained in ignored
`analysis/followup/crypto-audit/`. The build-specific methods that establish the
format are pinned by their CIL hashes in `crypto-findings.json`.

The controller's 928 KiB update containers remain opaque. We have not established
their cipher or found plaintext firmware. This result closes the identified
host-helper lead; it is not a proof that every byte of every updater version
has been examined or that keys could never exist elsewhere.

## Does verification code live in the firmware?

It could. The update commands and returned status show the controller making
an acceptance decision. They do not locate the implementation. Possible owners
include the currently running main firmware, a secondary bootloader, immutable
boot ROM, or a combination using a cryptographic peripheral and protected keys.

Two checks must be distinguished:

1. **Update admission:** may this header/body be received, erased/written and
   committed? We have observed the host protocol and its device status handling.
2. **Boot authentication:** after reset, may the processor execute this image?
   We have not reconstructed the controller's boot chain or established whether
   an additional check runs here.

Even if a verifier is inside the replaceable main firmware, changing it in a
new image does not solve the initial admission problem: the existing trusted
code must accept that image first. Patching a Windows return value also cannot
change a rejection made by the controller. A genuine debug/RAM execution route
could change the situation, but none is demonstrated for our controller yet.

The host protocol names header-CMAC, version, capability and body-CMAC failures.
CMAC is keyed authentication, distinct from a packet CRC and from encryption.
Readable plaintext would help reverse engineering; it would not automatically
let us authenticate a modified image. We do not know the controller's CMAC key
placement, exact authenticated ranges, or complete secure-boot policy.

## Flashing and recovery findings

The public updater author's February 27, 2026
[reply about downgrade attempts](https://github.com/nchie/dualsense-updater-rs/issues/1)
reports rejection of a header with an unacceptable version or CMAC before body
transfer. This supports device-side version enforcement; it is not just a GUI
restriction. It has not been retested on our BDM-040/0630 controller.

The author's February 15 reply and
[project README](https://github.com/nchie/dualsense-updater-rs/tree/c1ef2cacffa36d5c20867178af06afa3e92daa2f)
report an official update committing without explicit VerifyUpdateImage or
FinalizeUpdate commands. Those omitted commands do **not** define a dry run.
The author's expectation that rejected images leave the old firmware intact
is not a tested guarantee for interrupted writes or our hardware revision.

The pinned
[protocol definitions](https://github.com/nchie/dualsense-updater-rs/blob/c1ef2cacffa36d5c20867178af06afa3e92daa2f/src/protocol.rs)
also name a flash-erase error in StartUpdate and a flash-write error in the
body-transfer phase. These names do not establish exactly when physical flash
changes, but they are enough to reject an assumption that sending a valid header
or staging the body must be harmless. The public tool exposes update phases;
it does not implement a demonstrated independent rescue loader or rollback.

[Sony's instructions](https://controller.dl.playstation.net/controller/lang/en/2100004.html)
warn against disconnecting during an update. They describe the rear reset
button as troubleshooting and say it does not change pairing/profile settings.
They do not document a ROM loader, flash restoration or interrupted-update
recovery procedure. A reset button is not evidence of recoverable flash.

The community
[DFU modes note](https://www.psdevwiki.com/ps5/index.php?title=DualSense_DFU_Modes)
mentions primary/secondary loader modes, but its published entry procedures
are explicitly unknown. Treat those mode names as leads, not working recovery
instructions or proof of unsigned-image support. The live site was unavailable
during a later check; the indexed text still carried the unknown-entry warning.

No documented, demonstrated CXD90071GG-specific debug pinout, unsigned RAM loader
or end-to-end restoration procedure was established by the sources reviewed.
This is a research gap, not a claim that such a route cannot exist. The updater
library's 54 named exports contain normal controller/configuration/update APIs;
their names offer no explicit RAM-execute or rescue API. Hidden commands remain
possible and are not safe to discover by sending arbitrary reports to this pad.

## Execution milestones

The first useful hardware milestone is **reversible custom execution**, preferably
in RAM if a suitable entry point exists. An official updater's ability to send
Sony-authenticated images is not that milestone.

| Stage | Evidence needed before moving on | Current position |
| --- | --- | --- |
| Identify the target | Family/build match and revision evidence | Published `0004/0630` build matched; BDM-040 inferred from hardware report |
| Establish recovery | Known loader/debug entry, accepted recovery image, preserved device-specific data, restoration demonstrated on matching spare hardware | Not established |
| Execute custom bytes | A reproducible accepted loader/debug/runtime route and correct CPU/memory map | Not established |
| Observe a tiny payload | Bounded program produces a recognizable USB response, counter or LED sequence | Not attempted |
| Run the first game | Small state machine owns game logic on the controller; host only displays/input-transports results | Pending execution and I/O |
| Consider FUZIX | Measured usable RAM, executable storage, interrupts and USB driver ownership | Premature to size |

A tiny game need not boot a kernel. A few game-state variables, button input
and a USB status reply could be enough for a reaction or guessing game. A host
rendering its state would still satisfy controller-side execution; running all
game logic on the Mac and changing the lightbar would not establish that.

USB device mode itself is compatible with this goal: the Mac can initiate
requests and read replies. The unresolved issue is which USB services remain
usable after custom code takes control. A RAM payload inside running firmware
might reuse existing services if their ABI is recovered; a replacement firmware
would need to initialize USB itself or call a verified resident loader service.
Neither is established merely by the current pad enumerating as a USB device.

The next practical research target is the loader/debug boundary on matching
hardware: trace a legitimate update on a separate, updatable specimen; find a
verified reset/boot/debug path and preserve any available unique calibration
and identity data. Passive USB capture can clarify the normal protocol, but
cannot prove recovery or reveal encrypted executable code by itself. Public
bootloader documentation or a genuine plaintext/ROM dump would be substantially
more useful than repeatedly comparing opaque update bodies.

## Reproduce the offline resource result

Use the already extracted, hash-pinned Windows application and existing local
REA evidence from [the preceding trace](rea-trace.md). `dncil` and `pefile` are
in the isolated research Python directory; OpenSSL performs only AES on file
bytes supplied by this decoder.

```sh
python3 research/dualsense/unpack_resources.py
python3 research/dualsense/test_resources.py -v
python3 -m unittest discover -s research/dualsense -p 'test_*.py' -v
python3 research/dualsense/fetch.py --verify-offline
```

Seven new checks cover the recorded assembly/string/table identities, both
observed Huffman forms, independently encoded fixed/stored blocks, truncated
input, malformed trees/strings and output bounds. The full suite contains
24 checks at this stage; the follow-up expands it to 30. Binary research inputs and decoded Sony resources remain ignored;
only analysis code, hashes and findings belong in version control.
