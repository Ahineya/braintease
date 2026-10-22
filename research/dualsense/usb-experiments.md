# DualSense USB query experiments

On 2026-10-21 the connected `054c:0ce6` controller completed a bounded USB
experiment pass using published information queries and ordinary output
controls. Firmware, IMU calibration and the twelve stick-calibration values
remained unchanged in the observed readbacks. Every NVS status reply reported
locked persistence. No arbitrary memory access or custom code execution was
established.

The useful protocol finding is that **the four tested factory information
replies are consumed by a read, including a partial read** on this macOS/controller combination. The controller
also advertises two feature reports missing from our older reference capture.

The later [factory report trace](factory-report-trace.md) samples those reports
and establishes a different, six-chunk protocol for telemetry. Consume-on-read
does not mean that every factory command has only one reply.

## Commands exercised

`usb_probe.c` uses macOS IOKit directly. `usb_policy.h` restricts every report
before transmission; there is no CLI for arbitrary report IDs or payloads.
The query definitions come from the pinned
[calibration tool source](https://github.com/dualshock-tools/dualshock-tools.github.io/blob/eb9af384c3372dac966bf26660ff90a20b9cf5d5/js/controllers/ds5-controller.js).

| Operation | Request | Observed result |
| --- | --- | --- |
| Firmware information | GET feature `20` | 64 bytes including report ID; unchanged from `controller.json` |
| IMU calibration | GET feature `05` | 41 bytes including report ID; unchanged across runs |
| NVS status | SET feature `80`, payload `03 03`; GET `81` | `81 03 03 02 01`, followed by zeros; persistence locked |
| Stick calibration data | SET `80`, payload `0c 02`; GET `81` | Twelve stable little-endian 16-bit values |
| Touchpad identity | SET `80`, payload `05 02`; GET `81` | Eight bytes followed by zeros |
| Touchpad version information | SET `80`, payload `05 04`; GET `81` | Eight bytes followed by zeros |
| Ordinary output | Output `02` | Dim RGB setting and low rumble requests, each followed by a stop packet; all accepted by IOKit |

These query operations use SET_REPORT to select a result even though their
purpose is reading information. No calibration changes, NVS unlock/lock,
factory audio tests, reset, pairing, update or loader commands were sent.
NVS locked status is not a general guarantee against every possible flash write.
The physical LED/rumble effects were not independently observed.

## Consume on read behavior

The first framing experiment issued the NVS query, read four bytes from `81`,
then tried to read eight more. It received:

```text
First GET, capacity 4:  81 03 03 02
Second GET, capacity 8: 81 00 00 00 00 00 00 00
```

The workbench stopped on that unexpected second response. A subsequent health
and query pass succeeded with unchanged calibration and locked NVS status.
The second response was an idle result, not a continuation containing the NVS
status byte.

Reissuing each query before reading confirmed this behavior in **55 framing
cases**. Each first response matched the corresponding baseline prefix; each
following full read contained report ID `81` and 63 zero bytes. Query request
sizes were 3, 4, 8, 16 and 64 bytes, including report ID, with zero padding.
Reply buffer sizes were 4, 8, 16, 28, 63 and 64. All four queries were exercised,
with NVS status repeated after the larger responses. Known full replies had
zero tails beyond their expected data; no previous calibration or touchpad
payload remained in the smaller status response.

Firmware information on `20` behaved differently: repeated reads with buffer
sizes 1, 2, 4, 8, 16, 32, 63 and 64 returned matching prefixes without consuming
the information report.

Future factory-query code should read the entire response once and retain it
locally. Reading a header and then fetching the remainder loses the result in
these experiments. The sizes above are **IOKit API buffer/request sizes**;
without a bus capture they do not independently establish exact USB `wLength`
or isolate firmware behavior from the host HID stack.

## Descriptor difference

The connected device's report descriptor is **289 bytes**, SHA256:

```text
4f48767516627510521512af13c20a064877465122b7bcc7cac1f3b608d37994
```

Compared with the pinned 273-byte
[nondebug capture](https://github.com/nondebug/dualsense/blob/02338358b6b4be2d64dbf661ae2a47842156cfcf/report-descriptor-usb.txt),
it adds feature reports **`0b` and `0c`**, each declaring 41 payload bytes plus
report ID. Their semantics were not established and neither was accessed.
The difference does not establish which hardware or firmware change introduced
them. Tracing their use in first-party host software is the next specific lead.

## Recorded checks and reproduction

`usb-findings.json` records hashes of the transcripts and final tool sources.
Raw transcripts remain under ignored `analysis/`. Across seven device runs:

- 265 feature GET calls and 90 SET/output calls completed without IOKit errors.
- 3,199 input reports arrived, with no invalid callback reports recorded.
- All firmware health checks and completed calibration comparisons passed.
- The first framing run intentionally ended with a failed expectation when it
  discovered the consume-on-read behavior; its transcript is preserved.

The final wrapper records source hashes, build arguments, stderr, return status
and transcript hash for each new run. It refuses concurrent workbench runs and
stops its child at a 30-second deadline. This bounds host execution; killing a
process is not a hardware reset or a guarantee about an interrupted effect.

From the repository root, on macOS with exactly one USB DualSense connected:

```sh
# Offline: includes exhaustive testing of the command allowlist.
python3 -m unittest discover -s research/dualsense -p 'test_*.py' -v

# Live device access: default is descriptor/info/calibration reads only.
python3 research/dualsense/usb_probe.py

# Live published query and framing experiments.
python3 research/dualsense/usb_probe.py --queries --lengths --query-shapes

# Live brief LED/rumble output and stop requests.
python3 research/dualsense/usb_probe.py --controls
```

All **31 offline tests** passed, and the wrapper's final read-only device run
succeeded. The local build selects the installed Xcode 15.5 SDK when present
because the default SDK's text stubs are incompatible with the installed linker.
