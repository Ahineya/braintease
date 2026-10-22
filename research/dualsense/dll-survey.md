# Sony controller DLL survey and next research targets

The native DLL contains useful protocol descriptions, but its export list is
not a list of hidden debug commands. Of 54 exports, 24 explicitly name the
DualSense Edge. Selected profile, reset and tuning paths describe configuration
features; no arbitrary-address memory or execute operation was identified.
This 2026-10-21 pass used static analysis only and sent no USB commands.

## Export surface

These categories follow export names. They inventory the API surface rather
than establish every internal function's behavior.

| Category | Exports | Examples |
| --- | ---: | --- |
| Edge named functions | 24 | Profiles, saved settings, tuning, reset, Edge telemetry |
| Input access | 8 | Digital buttons, analog buttons, sticks, raw/adjusted variants |
| Connection and lifecycle | 8 | Open, close, initialization, handles, connection count |
| Identity and status | 7 | Firmware, software series, battery, address, container ID |
| Firmware update | 4 | Start, body transfer, verify, finalize |
| Light bar | 2 | Set and reset color |
| Standard telemetry | 1 | The previously exercised six-chunk reader |

For example, `GetAnalogSticksRaw` takes a controller handle and packs four
axis bytes into a scalar. Its name does not indicate raw controller-memory
access. Host buffers and allocation callbacks similarly do not establish a
device RAM-loading facility.

## Additional paths identified

`GetDualSenseEdgeProfile`, RVA `0x25d0`, selects a base GET report ID from
`70`, `73`, `76` or `79`, then reads three consecutive report IDs. It assembles
three 58-byte portions into a 174-byte record and validates sequence numbers,
field ranges and a checksum. These report IDs are distinct from the `70`
**selector inside report `80`** used for standard-controller telemetry.

`SetDualSenseEdgeProfile`, RVA `0x28a0`, sends three portions through report
`60`, `61` or `62`, depending on the writable slot, and checks a corresponding
status report `63`, `64` or `65`. The host routine at `0x3950` computes the
profile checksum over 170 bytes; the caller complements its result. All 256
entries of its table at `0x2e2b0` match reflected CRC-32/IEEE, polynomial
`0xedb88320`. This describes settings-record integrity, not firmware image
authentication or permission to execute the record.

`ResetDualSenseEdgeController`, RVA `0x2f10`, starts with a report beginning
`68 ff` and then performs further setting operations. It is an Edge-specific
API, not a demonstrated universal recovery sequence. The examined tuning
export delegates to another mode-setting routine. Neither reset nor tuning
was tried on the standard controller.

The earlier `80 18 01` factory configuration path remains unidentified. Its
enable flag at `0x48df6` and value at `0x48df4` have only a clear operation and
read operations among their identified direct references. No enabling path was
found. That makes it a dormant-looking branch in this build, not a proven
available debug feature; indirect writes and other builds remain possible.

Five additional REA/Ghidra decompilations are retained in ignored
`analysis/followup/dll-map/`. `dll-survey.json` records their hashes, the export
inventory and the checksum-table observation. The original DLL remains
unchanged and hash-pinned to the preceding investigation.

## Priorities for the custom code goal

1. **Compare another first-party controller client.** Sony documents USB
   DualSense and haptic support in
   [PS Remote Play for Mac](https://remoteplay.dl.playstation.net/remoteplay/lang/en/ps5_mac.html).
   Its controller transport is therefore a concrete new static-analysis target.
   It may contain different report definitions or clearer structures than the
   updater; its binary has not been downloaded or inspected in this pass.
   Support for USB controllers does not establish a debug interface.
2. **Compare report `0x0b` across controller specimens.** A second known-size
   GET capture, tied to firmware and hardware identity, could distinguish
   constant format bytes from revision fields and device-specific data. That
   is more informative than assigning meanings to one opaque sample.
3. **Expand the factory query catalog from identified callers.** Capture exact
   selectors, payloads, reply framing and model/version gates. Add live queries
   only when their purpose is established; keep settings and reset paths out
   of the information-query allowlist.

The missing capability for the first payload remains a device-side debug or
memory-access primitive, or a weakness in a reachable firmware parser. The host
DLL reveals packet formats and validation expectations; it cannot establish
the controller's internal bounds checks while its executable code remains
unavailable. Edge profile uploads are a lower priority for the current
standard-controller specimen, and `0xa0`/loader operations stay excluded.
