# PS Remote Play for Windows: package and transport audit

The Windows client is a harder protocol source than the Mac client. This pass
obtained and unpacked Sony's current 9.5 package, compared the older 9.0 package,
and traced the readable host preloader. The actual Windows controller report
callers remain unresolved behind the protected application code. We cannot
yet say whether those callers use features `0x0b`, `0x0c`, `0xa0` or the factory
transport.

This 2026-10-21 pass was static only. No installer, Sony executable or DLL was
run or loaded. No USB device was opened and no controller commands were sent.
The existing workbench and its allowlist were not changed.

## Inputs and extraction

Sony's [Windows Remote Play download page](https://remoteplay.dl.playstation.net/remoteplay/lang/en/1100001.html)
links to `RemotePlayInstaller.exe`. That file is a small InstallShield
bootstrapper. Statically decoding its 63-member `ISSetupStream` v4 archive
recovers a `Setup.ini` naming the actual MSI and its Sony download URL.

| Input | Bytes | SHA256 |
| --- | ---: | --- |
| Current bootstrapper | 3,292,384 | `f10055d503c47a4e96fd2f578fbcba3e651c19ecbe7f30ed87c15bec798f5684` |
| `RemotePlayInstaller_9.5.0.08040_x64.msi` | 86,272,512 | `a67d36c6fb14e67ec78858b831153fe372a0af5f7c41649ef710e3e8fc30bc25` |

Both downloaded hashes independently match the
[commit-pinned WinGet installer manifest](https://github.com/microsoft/winget-pkgs/blob/3b5ad74e8555b885a02c683af811a442e7f90e7d/manifests/p/PlayStation/PSRemotePlay/9.5.0.08040/PlayStation.PSRemotePlay.installer.yaml).
The embedded certificate metadata names Sony Interactive Entertainment Inc.;
this pass did not perform full Windows Authenticode trust validation. Hash
agreement and certificate metadata must not be described as that validation.

The existing InstallShield `members` and `decode` helpers were reused with the
new bootstrapper hash checked separately. The older updater's pinned `extract()`
guard was not relaxed. `unar` then expanded the MSI and its embedded
`Data1.cab`, yielding 46 cabinet files. The 26 `Binary.*` MSI streams were also
inventoried; no separate pad transport library was identified there. Installer
custom actions were never executed.

Original inputs are read-only and retained under ignored
`analysis/remoteplay-win/`, alongside derived analysis files. The
[machine-readable findings](remoteplay-windows-findings.json) contain URLs,
hashes, binary inventories, section measurements and evidence hashes. Sony's
binaries retain their original ownership and licensing.

## The native code barrier

The main native files are:

| File | Bytes | Observed role or limitation |
| --- | ---: | --- |
| `remoteplay.exe` | 37,795,744 | Protected main application; normal imports expose only `preloader.dll` ordinal 1 |
| `preloader.dll` | 49,568 | Small, readable host runtime loader |
| `runtime.dll` | 62,004,632 | Mapped host runtime with `packer*` sections |
| `scenp.dll` | 4,910,488 | Separate native library; no direct HID report imports identified |
| `remoteplayui.dll` | 5,811,104 | Managed UI with obfuscated metadata and readable UI strings |

The current main executable has `.grfn10`, `.grfn20`, `.grfn11` and `.grfn22`
sections. Its nominal `.text` section has entropy **7.999974 bits/byte**;
`.rdata` is **7.991325**, and `.pdata` is **7.999606**. These measurements cover
each section's raw file bytes. Together with the loader structure and hidden
imports, they support treating this as protected on-disk content rather than
ordinary recovered code. They do not identify an encryption algorithm or a
particular protection vendor.

The cabinet's ordinary import inventories expose no `HidD_GetFeature` or
`HidD_SetFeature` import. Delay imports were checked separately. This is a
visibility limitation: it does not mean Remote Play lacks USB controller
support, nor does it establish where every dynamically resolved API lives.
Disassembling the high-entropy sections and treating accepted x86 instructions
as real functions would produce unreliable protocol claims.

## What the readable preloader establishes

REA/Ghidra recovered the function at preferred VA `0x180002390`, RVA `0x2390`,
from `preloader.dll`, SHA256
`78f0f88ed59e6d8d26f3f149703d781cdf2b60edc81db59eca310f3d1c3301c7`.
Its complete decompilation is retained with the original input digest.

The function constructs an adjacent `runtime.dll` path, opens/maps the file
using native Windows APIs, prepares a table containing host callbacks and
addresses, then transfers control to the mapped runtime's entry point. That
entry is RVA `0x260410` in the current runtime, whose preferred base is
`0x200000000`. The ordinal-2 link function at RVA `0x37f0` immediately returns.

The loader explicitly resolves `wine_get_version` through
`LdrGetProcedureAddress` and has a manual mapping path associated with that
environment. This makes Wine worth assessing for future host analysis; it is
not a demonstrated compatibility result. No Wine installation or test was
performed.

The memory allocation, mapping and protection calls here operate on the
**Windows host process**. They are not a DualSense RAM upload mechanism.
Likewise, the host runtime and its loader parameters are not an identified
controller firmware image or firmware decryption key. This pass did not unpack
the protected application or recover the runtime's transformation algorithm.

The managed UI retains `TYPE_BOND` and `TYPE_HUNT` controller-update enum names,
and messages recommending that controllers be connected to a PS5 for updates.
Those strings do not establish the packed native code's complete capabilities.
The metadata parser also reported an invalid compressed integer, so no complete
UI deobfuscation is claimed.

## Older-build comparison

The older
[9.0 WinGet manifest](https://github.com/microsoft/winget-pkgs/blob/a5b12a46f70620448dc4e156fb37dc64765edb34/manifests/p/PlayStation/PSRemotePlay/9.0.0.02120/PlayStation.PSRemotePlay.installer.yaml)
points to the mutable `RemotePlayInstaller.exe` URL. Its recorded expected hash
differs from today's download. Downloading that URL would therefore not obtain
the historical bootstrapper.

A bounded check of six versioned filename hypotheses on Sony's server found a
valid `RemotePlayInstaller_9.0.0.02120_x64.msi`: 83,003,904 bytes, SHA256
`b41e63a3655fdb3ae545dbf5b051624d76d25cdd086aa334ce52781a3cfb9926`.
This URL was inferred from the naming pattern and confirmed by the server;
it was not extracted from an old bootstrapper or pinned by that manifest.

Its cabinet contains the same broad application/runtime arrangement. The 9.0
main also has `.grfn*` sections and a `.text` entropy of **7.999978 bits/byte**;
its `runtime.dll` also has `packer*` sections. No readable standalone pad DLL
was identified in that cabinet. Thus this particular older build does not
provide the desired shortcut; it says nothing about every earlier release.

The tested 7.5 `_Win32.msi` endpoint returned HTTP 200 with a **single ASCII
space**, not an MSI. That response was retained as failed acquisition evidence
and was not extracted. The other four filename hypotheses returned HTTP 404.
An attempt to retrieve the pinned 7.5 manifest also received HTTP 429; no
repeated retry or broad endpoint scan was performed.

## REA follow-up: more static work is possible

A subsequent REA pass revises the initial recommendation to move directly to a
live Windows environment. The runtime contains readable bootstrap code, and
some of its obfuscation can be resolved statically. Packing is an analysis
problem, not by itself a reason to require execution of the application.

The runtime entry at `0x200260410` jumps to `0x2008d453d`. That path includes
overlapping instructions and indirect branches whose targets are assembled
through address arithmetic. Two examined wrappers have constant destinations:

| Wrapper VA | Statically resolved destination VA |
| --- | --- |
| `0x2008c15df` | `0x2007c1e29` |
| `0x2007e765c` | `0x200813ed0` |

Each starts with `eb ff`, a jump into its own second byte, which begins the
instruction `ff f0` (`push rax`). Two following `lea` instructions construct
the destination for `jmp rax`. Following actual branch addresses and evaluating
those constants avoids the misleading boundaries of a linear disassembly.

For the first wrapper, a separately hashed **19-byte analysis fragment** was
created. Replacing its first byte with `nop` makes execution fall through to
the same second-byte instruction, removing that overlap for analysis. This
preserves the ordinary register/flag effects of this entry path; timing and
debug behavior are not claimed equivalent. The original DLL was not changed.

REA/Ghidra successfully decompiled this normalized fragment and recovered
destination `0x2007c1e29`, agreeing with direct constant arithmetic. The target
body is outside the fragment: the decompiler's call/return notation is not a
recovery of the whole original function. A separate whole-runtime attempt hit
REA's 330-second import/auto-analysis deadline and produced no full-runtime
result. Neither outcome unpacks the application or identifies its USB report
handlers.

[runtime-static-findings.json](runtime-static-findings.json) records the original
DLL hash, exact source bytes and offsets, derived fragment identity, one-byte
normalization, REA output and timeout evidence. All work remained offline,
without vendor-code execution or controller access.

## Smaller helpers and dispatch follow-up

The next static pass used bounded fragments instead of importing the entire
62 MB runtime. REA's startup allowance was raised from 330 to **900 seconds**
for these invocations using `REA_GHIDRA_STARTUP_TIMEOUT_MS=900000`. This is a
per-invocation setting; no toolchain default or controller policy was changed.
Eight fragments produced Ghidra pseudocode, including a 61,312-byte arithmetic
helper. Raising the allowance does not mean these fragments took 15 minutes,
and the whole-DLL analysis was not repeated.

Every new fragment is an exact contiguous copy of its source bytes, mapped at
its original preferred virtual address. The original runtime hash was checked
again. Adjacent fragment manifests retain source offsets, lengths and hashes.
These new fragments contain no instruction patches. External code and data
outside each fragment are absent, so unresolved references and decompiler
warnings remain meaningful limitations. Raw-binary decompilation also lacks
the PE's Windows calling-convention metadata: argument numbers in its output
must be checked against the actual registers before naming parameters.

### The overlapping branches have a repeatable explanation

For example, the path starting at `0x2006949c3` is:

```text
0x2006949c3  mov rax, 0x2f561aeb
0x2006949ca  test r10, rax
0x2006949cd  jae 0x2006949c6
0x2006949c6  jmp 0x2006949e2
```

`TEST` clears carry, so this `JAE` is always taken. Its destination is inside
the preceding `MOV`'s immediate, where bytes `eb 1a` form the real next jump.
The apparent fall-through contains misleading SIMD instructions and invalid
decodes. The same reasoning works for `JNO` immediately after `TEST`, because
overflow is also cleared. No unknown condition was assumed true.

Three return-dispatch paths now reach register/flags restoration and a direct
continuation:

| Initial helper | Local dispatch body | Continuation after restoration |
| --- | --- | --- |
| `0x2006d9b0e` | `0x2006949c3` | `0x2008d5ade` |
| `0x2008958ec` | `0x2007d6532` | `0x2008d45d7` |
| `0x2006da44a` | `0x2008eea49` | `0x2008d45ec` |

These are stack-based control-flow adapters, rather than evidence of three
separate decryptors. This does **not** establish that their complete enclosing
operations are no-ops: the saved frame can be changed by the arithmetic helper.

The two earlier constant wrappers reach larger bodies through similar
overlapping predicates:

| Constant wrapper | State-saving body | Arithmetic body | Copied extent |
| --- | --- | --- | ---: |
| `0x2008c15df` | `0x2007c1e29` | `0x203972b41` | 61,312 bytes |
| `0x2007e765c` | `0x200813ed0` | `0x2034eb007` | 62,171 bytes |

Another examined wrapper, `0x20072d71a`, computes the same destination
`0x2007c1e29` as the first wrapper. This is useful sharing to account for when
tracing callers; it is not another independent unpacking algorithm.

One earlier stop was a decoder disagreement. At `0x200813f40`, bytes `63 d1`
are rejected by Capstone 5.0.6; iced-x86 1.21.0 decodes them as the two-byte
`movsxd edx,ecx`. Following that decoding reaches the second arithmetic body.
It should not be reported as proof of a required exception or encrypted code.

### What the large arithmetic body currently tells us

Following direct branches from `0x203972b41` recovers 14,844 instructions,
ending in an unresolved `jmp rcx` at `0x203981abf`. The corresponding walk for
`0x2034eb007` recovers 15,215 instructions and ends in `jmp rcx` at
`0x2034fa2e0`. Those counts describe the recovered direct control-flow graph,
including both sides of conditional branches. Indirect destinations remain
outside that coverage. Neither walk encounters a direct `CALL`.

REA successfully decompiled the first body. Its 2,768-line pseudocode still
contains extensive mixed Boolean/arithmetic expressions and an unrecovered
final jump. Decompilation is therefore progress in visibility, not recovery of
the original simple algorithm.

A separate experimental symbolic slice simplified several linear arithmetic
identities. In particular, the calculation ending at `0x203972c9d` reduces to
`0xc6c458b78e9631a3`, independently of its three saved-word inputs. The
simplifier enumerates the Boolean truth table of linear word expressions;
this is different from sampling a few arbitrary 64-bit register values.
Independent SMT controls accepted all 248 synthetic equivalence checks at
4- and 8-bit widths. Earlier generic 64-bit solver attempts returned
`unknown` on timeout and were not accepted as proofs.

For the first bootstrap entry, the slice models the preferred image base,
`RCX = 0x200000000`, and the saved relocation delta at frame offset `0x110`
as zero. It resolves a read at `0x203974569` to runtime address
`0x200924940`, whose first qword is `0x2ed`. It stops after 1,942 lifted
instructions at the input-dependent branch `0x203974ba5`, before a division
path. It performs no calls or system operations and does not pick an unknown
branch direction. This experimental model is not a complete x86 emulator or
a validation of every possible runtime context. The remaining arithmetic
contains the FNV prime `0x100000001b3`, but its presence alone does not identify
the whole function as a particular hash or decryptor.

### Smaller ordinary functions and runtime-context hooks

The runtime's exception directory supplies 25,842 function-range records.
These provide useful boundaries for small fragments without relying on a
whole-image disassembly. They are ranges, not a count of fully recovered
source functions or a guarantee that protected bytes have not changed.

The ordinary routine at `0x2002bf2b0` checks CPU vendor/features using `CPUID`
and `XGETBV` and updates feature-selection globals. At `0x2001c0390`, the
assembly and decompilation show an overlap-safe memory-copy implementation,
with SIMD paths, reverse copying for overlap and size-indexed dispatch tables.
Its tables are outside the raw fragment and are separately retained from the
PE. These are host initialization primitives; neither is identified as the
application unpacker.

The wrapper at `0x20018ad20` calls that copy routine with:

```text
destination = 0x200318090
source      = *(uint64_t *)0x200318e88 + 0x88
length      = 0x90
```

Five smaller functions referencing the context pointer at `0x200318e88` were
also extracted using their exception-directory ranges and decompiled:
`0x20017c140`, `0x200188260`, `0x200220980`, `0x200236130` and
`0x200250990`. They expose context-shaped record changes, callback redirection,
OS-version gates, a page-address bitmap, and encoded indirect call/syscall
fields. Their exact Windows API identities have not been established.
In particular, the `+0xf8`, `+0x80` and `+0x88` record accesses suggest saved
instruction/register context handling; that is an interpretation of the
layout and operations, not a recovered type declaration.

The pointer arithmetic is readable, for example:

```text
decoded = ROL64(encoded ^ xor_constant, 7) + add_constant
```

However, checked **on-disk initial values** are sentinels:

| Encoded global | Decoded initial value |
| --- | --- |
| `0x200318680` | `0` |
| `0x200318d30` | `0` |
| Low 32 bits at `0x200318b50` | `0xffffffff` |

Those formulas expose the representation, but do not yet provide valid API
addresses or syscall numbers. The initialization writes are the useful next
target. The page bitmap and context redirection are leads into protection
machinery; they do not prove page-by-page unpacking or locate USB handling.

This pass leaves the application unpacker, API-resolution initialization and
Windows HID callers unresolved. No new `0x0b`, `0x0c`, factory or loader
operation is justified by these findings. The controller remained untouched.
The expanded [machine-readable record](runtime-static-findings.json) identifies
the derived fragments, experimental limits and additional evidence hashes.

## Bounded emulation follow-up

The subsequent [bootstrap investigation](runtime-bootstrap-followup.md) reaches
the initializer for `0x200318e88` through bounded offline CPU emulation. It
connects that pointer to the readable preloader's fourth entry argument and
names the 18 Windows API pointers in the earlier bootstrap copy. It also
recovers a native string-decoding family. This extends the earlier static-only
work; the main application unpacker and HID callers remain unresolved.

## Next useful stages

Continue targeted static analysis first. Resolve the arithmetic helper's
division/branch joins and final computed continuation, then follow the writes
that initialize `0x200318e88` and the encoded pointer/syscall fields. Use the
exception-directory ranges to obtain bounded fragments and verify memory-write
destinations before labeling anything a decryptor. Once reconstructed code and
API addresses are attributable, trace the HID callers and report buffers.
Keep unresolved branches explicit. REA's function, reference and value-flow
tools do not automatically decrypt an arbitrary packed application.

If a live host stage becomes useful, the proposed sequence remains:

1. Use an isolated Windows x64 environment with **no USB passthrough**. Inspect
   the loaded main application and runtime, and identify reconstructed code,
   imports and dynamic API resolution. This would be a future vendor-code
   execution stage; it has not happened in this audit.
2. Expect that code may be reconstructed on demand. One early process dump may
   not contain every controller function. Preserve load addresses, module
   hashes and dump timing so later disassembly is attributable.
3. Once the relevant host code is readable, trace report APIs through their
   buffers, lengths, report IDs and model/version gates, as in the Mac audit.
   Only then compare `0x0b`, `0x0c` and factory operations.

Wine is an alternative to assess, based on the explicit loader handling, but
its compatibility remains unknown. The Mac client and updater remain the
readable first-party protocol sources already in hand. Nothing in this Windows
pass justifies a new live query, a loader experiment or a claim that custom
controller execution is impossible.
