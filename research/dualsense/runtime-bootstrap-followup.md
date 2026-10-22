# Remote Play runtime: bootstrap initialization recovered

The 2026-10-22 follow-up identifies the initializer for runtime global
`0x200318e88`: it receives the fourth entry argument, the readable preloader's
callback table. This connects the previously obscured bootstrap to a known
host interface. The application unpacker and Windows HID callers remain
unresolved.

The later [bootstrap continuation](runtime-bootstrap-continuation.md) follows
this initializer into table validation, import copying and Windows module
parsing. The barrier results below describe the preserved original experiment.

This pass combines static analysis with **bounded CPU emulation**. Sony's
Windows program was not launched, no Windows APIs were executed, and the
controller was not opened. The emulation runs x86-64 instructions inside
Unicorn against copied data and synthetic memory. This is a new analysis mode,
distinct from the earlier static-only passes.

## The recovered initializer

Starting at the first constant wrapper, the offline trace reaches:

```text
0x2010621e1  4c 89 28  mov qword ptr [rax], r13
```

At this instruction, `RAX = 0x200318e88` and `R13` equals the original entry
`R9`. Six runs varied the synthetic fourth-argument pointer, other registers,
and stack padding. Every run reached this same attempted store on instruction
step **22,854**:

| Entry R9 | Attempted value at `0x200318e88` |
| --- | --- |
| `0x60000100` | `0x60000100` |
| `0x60001100` | `0x60001100` |
| `0x60002100` | `0x60002100` |
| `0x60003100` | `0x60003100` |
| `0x60004100` | `0x60004100` |
| `0x60005100` | `0x60005100` |

The store itself was **blocked** by the copied runtime's write protection.
The reproducer verifies this expected observation and fails explicitly if it
does not recur. These are concrete differential observations, not a proof over
every possible register, memory or Windows configuration.

The traced arithmetic bodies, in order, are:

```text
0x203972b41 -> 0x203176330 -> 0x20230b931
           -> 0x20082e9bf  (thread-pointer load)
           -> 0x20329b333 -> 0x202f56b3d -> 0x201060d8f
```

The final body contains the initializer. Exact source copies of all six large
bodies are retained with source offsets, sizes and hashes. Register restoration,
small dispatch stubs and checked local call-to-RET trampolines connect them.
This trace does not establish that all these bodies are independent decryptors.

## Why this pointer has a known meaning

The already-readable preloader initializes a table at preferred VA
`0x18000a048`, then calls the runtime entry with:

```c
runtime_entry(0, 0x25665cd7, 0, &table);
```

Windows x64 passes that fourth argument in `R9`. The new trace connects the
table to the runtime global. Several fields can therefore be named from the
preloader's assignments rather than guessed from protected runtime code:

| Offset from table | Preloader assignment |
| --- | --- |
| `0x00` | `0xa9738c26f6daad73` |
| `0x08` | `0x019fcad04bee73a1` |
| `0x10` | Preloader image base |
| `0x18` | Mapped runtime base |
| `0x28` | Runtime section handle, `local_550` |
| `0x30` | Mapped runtime entry address |
| `0x58`, `0x60` | Zero |
| `0x68` | Preloader helper at `0x1800035d0` |
| `0x70` | Preloader helper at `0x1800035b0` |
| `0x78` | Preloader helper at `0x1800020a0` |

Other fields remain represented by their source variables or addresses in the
machine-readable findings. The two initial constants are host interface data;
they are not identified controller firmware keys.

The earlier wrapper at `0x20018ad20` copies `0x90` bytes from table offset
`0x88` to `0x200318090`. That is an array of **18 named import pointers**:

| Table offset | Imported pointer |
| --- | --- |
| `0x88` | `MessageBoxW` |
| `0x90` | `RegGetValueW` |
| `0x98` | `RtlQueryEnvironmentVariable_U` |
| `0xa0` | `RtlExpandEnvironmentStrings` |
| `0xa8` | `RtlDosPathNameToNtPathName_U_WithStatus` |
| `0xb0` | `RtlCreateProcessParametersEx` |
| `0xb8` | `RtlCreateEnvironment` |
| `0xc0` | `ZwQuerySystemInformation` |
| `0xc8` | `ZwCreateUserProcess` |
| `0xd0` | `ZwSetInformationFile` |
| `0xd8` | `ZwCreateFile` |
| `0xe0` | `ZwCreateThreadEx` |
| `0xe8` | `ZwQueryInformationFile` |
| `0xf0` | `ZwQueryInformationProcess` |
| `0xf8` | `ZwClose` |
| `0x100` | `ZwQueryDirectoryFile` |
| `0x108` | `ZwWaitForSingleObject` |
| `0x110` | `ZwDeleteFile` |

This identifies the source and purpose of that copy. It does not yet resolve
every encoded API pointer or syscall field elsewhere in the runtime.

## How the offline model is bounded

[`emulate_runtime.py`](emulate_runtime.py) pins the input hashes and admits
specific bootstrap ranges, plus at most twelve additional bounded function
ranges identified by the runtime's exception directory. Every instruction is
checked against an arithmetic/control-flow allowlist before emulation.
Arbitrary calls, system calls, CPU-query and device instructions are blocked.
The exception for calls is a checked local trampoline whose entire target is
one `RET`, with its return address verified.

The runtime copy is readable/executable but not writable. The main EXE copy is
readable, **not executable**, and not writable. Only the synthetic stack can
receive writes. Each run has a 250,000-instruction and five-second limit.
The original files are never mapped into a native Windows process or edited.

Three synthetic pointer cells bridge the first environment reads:

```text
GS:[0x30]       -> synthetic thread address
[thread+0x60]   -> synthetic process address
[process+0x10]  -> main EXE's preferred image base
```

The thread-to-process interpretation follows Microsoft's
[TEB declaration](https://learn.microsoft.com/en-us/windows/win32/api/winternl/ns-winternl-teb).
The internal main-image field follows the
[PHNT PEB layout](https://github.com/winsiderss/systeminformer/blob/master/phnt/include/ntpebteb.h).
Microsoft also documents the [PEB](https://learn.microsoft.com/en-us/windows/win32/api/winternl/ns-winternl-peb)
as an internal structure subject to change. These are explicit synthetic
layout assumptions, not captured Windows process state. Every other thread or
process field read stops the model instead of silently supplying zero.

No Windows API return values or callback implementations are invented. In the
final runs, the main EXE's bytes were not read before the initializer barrier;
only its image-base pointer was consumed. The callback-table pointer is varied
but its contents are not mapped or executed in this reproducer.

Eleven synthetic controls exercise the actual emulator policy: arithmetic,
blocked system calls, blocked arbitrary calls/returns, checked trampolines,
image write protection, unmapped reads, instruction limits, strict context
fields, and the main EXE's non-executable permission. All pass.

## Static work that helped get here

The previous symbolic stop at `0x203974ba5` is a division-width selection.
Its guard checks whether the numerator and divisor fit into 32 bits. With a
zero high dividend and the known positive divisor `0x2ed`, both paths produce
the same zero-extended quotient and remainder. The mathematical model checks
the two decoded paths and guard equality before merging their effects.

This advances the symbolic slice from 1,942 to 2,124 lifted instructions. The
next unresolved address is at `0x203974ec5`. The emulator independently shows
that read reaching `0x200925608`, with qword value `4`, in the tested states.
The symbolic checkpoint is retained; its unresolved address was not guessed.

Five more bounded REA/Ghidra decompilations were obtained:
`0x200219610`, `0x200229030`, `0x20018b1a0`, `0x200195230` and
`0x200243220`. These expose cache/path handling and shared-section machinery.
They have not been established as the main application unpacker.

## Native string decoding

Those smaller functions expose a separate native UTF-16 string transform:

```text
plain_word = cipher_word XOR low16(state)
state      = ROL64(state, 5) XOR cipher_word
```

Feedback uses the **ciphertext** word. This is unrelated to the earlier
managed resource decoder or a recovered firmware cipher.

[`decode_runtime_strings.py`](decode_runtime_strings.py) checks an exact
ten-instruction loop and a bounded setup providing its seed, zero counter and
RIP-relative source. Across 1,587 candidate alignments it recognizes seven
loops, yielding these strings (the terminating NUL is omitted here):

- `bhcache.36ab6a20677cfbf4.bin`
- `PACKER_SECTION` at two callers
- `\Registry\Machine`
- `catcache4.bin`
- `\SYSTEMROOT\system32\CatRoot\{F750E6C3-38EE-11D1-85E5-00C04FC295EE}`
- `%ALLUSERSPROFILE%\Packer`

A separately checked loop also decodes `*.cat`; it is outside the automated
recognizer's accepted setup family. The filenames do not prove that any cache
contains unpacked executable code. Pattern recognition also does not establish
that every matched caller executes. Five decoder controls pass, including
ciphertext-feedback round trips, odd-length rejection, a source-backed known
string, and rejection of altered rotation/branch instructions.

## Reproduction and remaining work

With the previously acquired, hash-pinned Windows inputs and local research
dependencies present:

```sh
python3 research/dualsense/test_runtime_strings.py
python3 research/dualsense/test_runtime_emulation.py
python3 research/dualsense/decode_runtime_strings.py
python3 research/dualsense/emulate_runtime.py
```

Dependencies are local `pefile`, Capstone 5.0.6 and Unicorn 2.1.4 under ignored
`analysis/toolchain/python/`. The earlier symbolic experiment additionally
uses Z3. Unicorn's host-cache probe trapped inside the restricted sandbox even
for a synthetic-only control, before target instructions ran; the control and
research runs passed with ordinary host metadata access.

The [findings record](runtime-bootstrap-findings.json) preserves input hashes,
exact body fragments, callback offsets, differential observations, controls and
evidence hashes. Analysis files and Sony binaries remain under ignored
`analysis/remoteplay-win/`; the original files retain their hashes.

The next target is continuation after the now-attributed initializer, using
the known preloader table layout. Any virtual data changes should be explicit
and recorded, with unknown callbacks/API results left unresolved. The encoded
API initialization, main code transformation and Windows HID report callers
still need recovery. Nothing here establishes a controller RAM execution path
or changes the live USB workbench.
