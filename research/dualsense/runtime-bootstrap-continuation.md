# Remote Play bootstrap continuation and Windows module discovery

This 2026-10-22 pass continues beyond the initializer recovered in the
[previous bootstrap investigation](runtime-bootstrap-followup.md). It follows
the runtime into table validation, import copying and a Windows export walk.
The traced export walk examines all 2,429 named exports of the pinned Windows
DLL and selects its 471 `Zw` names. This is evidence for host syscall setup,
rather than recovered controller firmware decryption.

All execution remains **offline CPU emulation**. The Windows application was
not launched, Windows APIs were not called, and the connected controller was
not accessed. Original Sony files remain unchanged.

## Table validation and import copying

The continuation validates and allows the original store at `0x2010621e1`
to write the preloader-table pointer into emulator-owned data at
`0x200318e88`. Unlike the historical barrier experiment, this model permits
recorded writes to the copied PE's writable, non-executable data sections.
Code remains immutable.

Six runs vary the initial table pointer, other general registers and stack
padding. With the import fields left unknown, every run stops at the first
read of table offset `0x88`, on step **42,583**. The preceding trace reads
the two source-backed magic values at offsets `0x00` and `0x08`.

Separate differential controls replace each magic value with zero. Both select
an alternate path ending at the unadmitted target `0x2008dd9dc`; the model
stops there. This demonstrates an effect on control flow without claiming to
have traced the eventual error handler.

The byte at table offset `0x85` is copied to runtime global `0x200318078` at
`0x200d8c71c`. Testing zero and one changes that byte accordingly. Its preloader
source comes from the machine-type query and is carried through `cVar12`; the
normal experiment chooses the native x64, non-Wine scenario with the crash
environment variable unset. These are declared test conditions, not captured
Windows state.

An additional six runs supply distinct, named **address tokens** for the
18 import pointers. These addresses have no executable mapping and no API
implementation. The original SSE2 copy instructions reproduce all 18 qwords
in order at `0x200318090` through `0x200318118`. Every run then stops at the
unknown table offset `0x20`, on step **59,122**, at `0x20287a502`.

This demonstrates the import-copy path dynamically, in addition to the earlier
static identification. It does not turn the tokens into working Windows APIs.

## The missing module-base field

The readable preloader provides a stronger attribution for offset `0x20`:
`local_498` receives a module-base result and is assigned to that field.
One native Windows path uses helper `0x180003530`:

1. It sets `RCX = 4` and calls an import thunk for `ntdll.dll!MD5Init`.
2. Its exception scope covers that call and names filter `0x1800034c0`.
3. The filter reads `[exception_pointers+8]`, then `[context+0xf8]`, and saves
   the result into the helper's local slot. It returns one to select handling.
4. The handler continuation reads that slot and passes it to
   `RtlPcToFileHeader`, whose output becomes the module-base result.

The field interpretation follows Microsoft's
[EXCEPTION_POINTERS](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_pointers)
and [x64 CONTEXT](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-context)
layouts: the captured value is the faulting instruction pointer. The scope
record is exactly `(1, 0x3546, 0x3551, 0x34c0, 0x3586)`, with
`__C_specific_handler` as its handler thunk.

In the pinned Windows data fixture, `MD5Init` is not forwarded. Its first
instruction is `and dword ptr [rcx], 0`; with `RCX = 4` and the ordinary
unmapped low page, this would fault inside `ntdll`. The resulting static chain
explains how the preloader obtains that DLL's base using a handled fault.
The exception itself was not executed in a Windows process or simulated by
inventing an API result. Nor is this call evidence of MD5 being used to decrypt
controller firmware.

## Windows data fixture and export walk

To continue the parser, the model supplies a real x64 `ntdll.dll` as
read-only, **non-executable data** at synthetic base `0x62000000`:

```text
Version  10.0.19041.207
Size     2,020,080 bytes
SHA256   96edaf03c5c54e5e0f8892a4944020dc1d4ca8edeb8de3fa1dabf02c61812dda
```

The file came from Microsoft's public symbol server. Its hash matches the
captured Winbindex metadata. This establishes input identity; the Authenticode
certificate chain was not validated. This is an explicit Windows 10 fixture,
not an observation of a Sony process's actual Windows version or address space.
The image is mapped by section RVA without applying relocations; the observed
PE header and export RVAs are usable in this model, while any later dependency
on relocated absolute pointers would need separate treatment.

The trace reads `MZ`, follows `e_lfanew`, and walks the export name, ordinal
and function arrays. The saved reads show:

- Every one of the 2,429 named exports is examined by the first-character
  comparison at `0x2028ca969`.
- The second-character comparison at `0x2034a65de` reads exactly the 471
  names starting with `Zw`.
- The stack probe receives `RAX = 0x1d70`, or **471 × 16 bytes**, at
  step 14,647,933.
- A second export walk follows, including local string processing.

The saved synthetic stack contains the full catalog at `0x700fc210`:
**471 sixteen-byte records**, each a function address followed by a 64-bit
name hash, sorted by address and then hash. An independent transcription of
runtime hash function `0x2002c5440` matches **all 471 stored hashes** against
their actual export names. Its reviewed 4..128-byte input branches cover
these names; the observed names are 7..53 bytes long. Other input lengths are
rejected by the analysis tool. Branches above the observed lengths were not
validated by this catalog comparison.

There is a further check on its purpose: **470 catalog indices match the
service numbers in the DLL's ordinary syscall stubs**. The remaining entry,
`ZwQuerySystemTime`, starts with a jump rather than the checked stub prefix,
so that particular service-number check is left unresolved.

The extended run reaches 48,306,552 instructions before exhausting the
128-helper admission budget at target `0x202fc3533`. It also observes a
four-byte update to runtime global `0x200318cb4`; that field's encoding is
not yet attributed. The main EXE's bytes were not read in this run.

This establishes a named catalog for Windows syscall setup. Its later
dispatch mechanism and encoded globals remain separate targets. It does not
recover the main application unpacker.

## Emulator policy and controls

[`emulate_runtime_continue.py`](emulate_runtime_continue.py) preserves the
original reproducer and adds an explicit continuation model. It admits the
reviewed bootstrap ranges and at most 128 additional bounded PE function
ranges or observed five-byte jumps to such ranges. Calls must target admitted
local code or a bounded PE function start; returns must match observed local
calls or individually reviewed return stubs. Indirect API calls are blocked.

Every instruction is checked before emulation. Immutable decoded instructions
are cached so repeated arithmetic does not repeatedly invoke the decoder or
range search. One exact decoder exception at `0x200813f40`, bytes `63 d1`,
is checked with iced-x86: `movsxd edx, ecx`. A synthetic CPU control verifies
its 32-bit destination semantics. Other decoder disagreements stop the model.

Two reviewed CPU-feature query sites use a fixed minimal SSE2 fixture.
No host CPUID is executed. The synthetic thread model now also exposes the
mapped stack limit at `GS:[0x10]` for the reviewed stack-page probe. Unknown
thread/process/table fields still stop; Windows return values are never
fabricated. The main EXE and Windows DLL stay non-executable and read-only.

Runs have instruction, time, helper-admission and memory-log budgets. Longer
runs can use bounded time slices while retaining the same emulator state.
[`test_runtime_continuation.py`](test_runtime_continuation.py) passes **22
synthetic containment controls**, including blocked system calls and register
access, API-token calls, immutable code, non-executable data, unknown context
fields, validated stores, instruction limits and continuation across slices.

## Reproduction and next target

With the earlier local research inputs and dependencies present:

```sh
# Network only if the pinned Windows data fixture is missing.
python3 research/dualsense/fetch_windows_data.py

python3 research/dualsense/test_runtime_continuation.py
python3 research/dualsense/check_runtime_bootstrap.py
python3 research/dualsense/emulate_runtime_continue.py --stage imports
python3 research/dualsense/emulate_runtime_continue.py --stage module-pointer
python3 research/dualsense/emulate_runtime_continue.py --stage module-data \
  --seeds 0 --instructions 80000000 --chunks 8 --timeout-ms 20000
python3 research/dualsense/analyze_runtime_catalog.py
python3 research/dualsense/analyze_runtime_continuation.py
```

The Windows fixture pin is in
[`windows-data-source.json`](windows-data-source.json). The extended decoder
check additionally uses the previously installed iced-x86 1.21.0. Unicorn still
requires host metadata access unavailable in this workspace's restricted
sandbox; the synthetic controls exercise the same runtime requirement.

The [findings record](runtime-continuation-findings.json) preserves the final
stop, input and evidence hashes, differential outcomes, export-read checks and
module-discovery proof. The 17 historical bootstrap evidence hashes still
verify. Sony/Microsoft binaries and full traces stay in ignored `analysis/`.

The next target is the encoded API/syscall consumers of the recovered catalog,
followed by the main application transformation and HID handlers. Nothing in
this pass establishes unsigned controller firmware acceptance, RAM payload
execution or a controller recovery method.
