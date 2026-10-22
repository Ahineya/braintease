# DualSense factory report trace

The 2026-10-21 follow-up established a working factory telemetry reader on the
connected controller and sampled reports `0x0b` and `0x0c` without sending SETs
to either. Report `0x0b` returned stable data; `0x0c` rejected the GET. Neither
report has an identified meaning yet. Firmware and calibration readbacks stayed
unchanged, and the subsequent NVS status query still reported locked persistence.
Report `0xa0`, loader entry and firmware update operations remained excluded.

## Reports 0b and 0c

| Report | Live observation | Interpretation |
| --- | --- | --- |
| `0x0b` | Three successful 42-byte IOKit GET results, identical contents | Stable opaque data; not identified as code, a memory window or a cryptographic key |
| `0x0c` | One GET returned IOKit error `0xe0005000`; firmware-info reads and input callbacks continued normally | This GET was rejected; descriptor presence alone does not establish usable GET semantics |

The buffers were sized at the descriptor's 41 payload bytes plus report ID.
There were no oversized requests or SETs to these reports. The captured `0x0b`
payload does not contain the full MCU/PCBA identifiers or any of their nonempty
four-byte windows, as returned by the published `80 01 09` and `80 01 11`
queries. This rules out that simple correspondence, not encoded identifiers or
other meanings. Raw identifiers remain in ignored local transcripts.

### What Sony's host software establishes

We examined the previously pinned PlayStation Accessories 2.2.1.2 managed
assembly and native `libfwupdater.dll`. Native SHA256 remains:

```text
3bc6de1116a7ea29d0997f632613edfe7c3100b62efbf5bca6f32f9fb4bc4569
```

The original-byte-checked inventory finds ten direct calls to the
`HidD_GetFeature` thunk, three to `HidD_SetFeature`, and one to a separate
factory-selector sender. Reviewed paths include generic feature wrappers,
device initialization, firmware/status reads and telemetry. No use of `0x0b`
or `0x0c` was identified in those paths. We therefore cannot assign their
firmware handlers from this DLL. The inventory covers explicit direct calls,
not every possible dynamically resolved path or other Sony application.

REA/Ghidra decompilations and their original subject hashes are preserved under
`analysis/followup/report-trace/`. `report_protocol.py` independently checks
the selected call instructions against the original PE bytes before recording
their RVAs. This is static analysis; no Sony executable was run.

## Factory telemetry

Export `GetTelemetryDataDualSense` at RVA `0x3640` calls the shared reader at
`0x3400` after checking the controller type and update version greater than
`0x0452`. The Edge export at `0x36f0` checks its own type/version threshold and
uses the same reader. Only the connected standard controller was tested.

The native reader constructs a zero-padded 64-byte feature request beginning
`80 70 01`. It then reads six 64-byte replies from `81`, assembling 56 data
bytes from each reply's offsets 4 through 59. There is no supplied address,
memory offset, transfer destination or execution command.

Our controller returned:

```text
81 70 01 03  [56 data bytes] [4 trailing bytes]   five replies
81 70 01 02  [56 data bytes] [4 trailing bytes]   final reply
```

This produces **336 bytes** of assembled data. Native code skips the first
assembled byte when copying into the output structure. On this capture that
byte is zero. The application's packed standard-controller structure supplies
a 17-character serial field and **55 numeric fields**, occupying 190 bytes
after the skipped byte. Labels include:

- Total records, active time and charging time.
- Haptic and adaptive-trigger activity.
- USB/Bluetooth connections and charging events.
- Authentication challenge, success and failure counters.
- Stick movement, button presses and touchpad activity.

The record contains a printable identity and plausible values at those field
positions. Counter units and reset policies are not established. The remaining
145 bytes are preserved as an uninterpreted tail rather than assigned invented
fields. `report_protocol.py` writes a decoded local result with the serial
omitted; raw usage counts and identity bytes stay under ignored `analysis/`.

This extends the earlier mailbox finding: **the four previously tested simple
queries return one consumable result, while telemetry supplies six sequential
chunks**. Repeating GET does not universally mean idle. Response handling must
follow each command's protocol; short reads of multi-part telemetry were not
tested.

## Configuration paths kept out of live probing

A second native sender at RVA `0x10490` constructs report `80` with two
caller-supplied selectors and optional data. Its identified direct caller at
`0xfde1`, inside `0xfd30`, supplies selectors `18 01` and two configuration
bytes derived from initialization state. Its exact device-side meaning was
not established. It is not treated as a read query and was not sent.

This is a reason to follow callers and payload construction rather than scan
every factory selector. A generic packet builder is not itself an arbitrary
memory-write interface.

## Why a0 remains excluded

The descriptor declares one payload byte for report `0xa0`. The earlier
[community report table](https://controllers.fandom.com/wiki/Sony_DualSense/Report_Summaries)
labels it DFU enable while requesting further documentation. Neither that
label nor the descriptor establishes accepted values, side effects or exit
behavior for this controller. It remains a potential mode-transition command,
so **both GET and SET are blocked** by the workbench policy.

An apparent `0xa0` constant in Sony's DLL is unrelated: at RVA `0x6c05` it is
the length argument to the zero-fill routine called at `0x6c2a`. That occurrence
clears 160 host bytes; it does not send report `0xa0`. This byte-checked example
illustrates why matching an immediate value alone is insufficient.

## Tools and validation

The workbench now additionally permits the two published identity queries,
the exact telemetry request and GET-only `0x0b`/`0x0c` requests at 42 bytes.
It still exposes no arbitrary packet interface. Policy tests exhaust the
two-byte selector space and explicitly reject `0xa0`, update reports and NVS
unlock. The new parser tests cover malformed chunks, continuation status,
cross-chunk integers, identity redaction and changed evidence bytes.

All **35 offline tests passed**. Three live runs completed with 802 valid input
reports and no invalid callback reports. The rejected `0x0c` GET is recorded
explicitly; a completed experiment does not mean that GET succeeded.
`report-findings.json` records transcripts, source hashes and decompilation
hashes. The current tool sources are also snapshotted under ignored
`analysis/followup/report-trace/tool-snapshot/`.

```sh
# Offline checks and original-byte audit.
python3 -m unittest discover -s research/dualsense -p 'test_*.py' -v
python3 research/dualsense/report_protocol.py

# Live phases; each saves its own transcript and source hashes.
python3 research/dualsense/usb_probe.py --extra-reports
python3 research/dualsense/usb_probe.py --telemetry
python3 research/dualsense/usb_probe.py --identity --queries
```

To decode an existing telemetry transcript without opening USB:

```sh
python3 research/dualsense/report_protocol.py --telemetry-log /absolute/path/to/transcript.jsonl
```

The next useful evidence would identify the `0x0b` data structure or additional
factory information queries from authentic host code. Arbitrary memory access,
decrypted controller code and a custom payload remain separate milestones.

The subsequent [DLL survey](dll-survey.md) maps the export surface and selected
Edge profile/reset paths, verifies the profile CRC table and ranks additional
first-party transport analysis and cross-controller report comparisons.
