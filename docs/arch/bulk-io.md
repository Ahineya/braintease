# Bulk storage and memory transfers

Bulk I/O uses the existing `DEVICE_*` mailbox and ABI version 1. It removes guest
word-copy loops without changing the ISA. RVM and the RBF host implement disk and
Harvard code transfers. The native runtime supports RAM copy/fill; disk and guest
instruction-memory operations return `UNSUPPORTED` there. These are host devices,
so a standalone Brainfuck interpreter needs equivalent device support.

## Commands and capabilities

| Command | ID | Operation |
|---|---|---|
| STORAGE_READ | 0x0300 | attached disk → RAM or instruction memory |
| STORAGE_WRITE | 0x0301 | RAM → attached disk cache |
| MEMORY_COPY | 0x0302 | RAM → RAM or instruction memory |
| MEMORY_FILL | 0x0303 | fill RAM cells |

`CAPS` advertises RAM transfers with bit `0x0400`, storage with `0x0200` when a
disk backend is attached, and instruction uploads with `0x0800` when the CPU image
and host adapter support them. New RBF images publish reserved code capacity at
CPU boot; older images retain RAM transfers but reject instruction uploads.
Rebuild an RBF image to enable its bulk code path. No additional MMIO pins,
interrupts, or polling loops are required.

## Descriptor

All four commands take twelve 16-bit cells. Inputs and the descriptor are
snapshotted before mutation; the descriptor has no output fields.

| Cell | Meaning |
|---|---|
| 0 | version = 1 |
| 1 | length = 12 cells |
| 2–3 | STORAGE: disk octet offset, low/high; COPY: source RAM bank/offset; FILL: value/zero |
| 4–5 | destination bank/offset; STORAGE_WRITE uses these as its RAM source |
| 6–7 | element count, low/high |
| 8 | representation, below |
| 9–11 | reserved, must be zero |

| Representation | Value | READ / WRITE | COPY |
|---|---|---|---|
| WORDS | 0 | two little-endian disk octets per RAM cell | one RAM cell per element |
| OCTETS | 1 | one disk octet per RAM cell; WRITE takes low 8 bits | one RAM cell per element, masked to low 8 bits |
| CODE_WORDS | 2 | READ: eight disk octets per instruction; WRITE unsupported | four RAM cells per instruction |
| CODE_OCTETS | 3 | unsupported | eight RAM cells per instruction, low octets decoded |

Code representation is `[opcode, word1, word2, word3]` in cell form, or
`[opcode, ignored pad, word1_lo, word1_hi, word2_lo, word2_hi, word3_lo, word3_hi]`
in octet form. The opcode's low eight bits are retained, as by STORC. Uploaded
instructions are executed normally; unsupported opcodes still fault when run.
FILL accepts only WORDS.

Count means RAM cells for RAM operations and **instructions** for code operations.
Each submission accepts at most 32,768 RAM cells or 8,192 instructions. Zero count
is a no-op after validating the descriptor, endpoints and backend availability.
Bank offsets must be canonical (`offset < bank_size`). Transfers cross banks
using the configured bank size, including non-power-of-two sizes such as 64000.
Code offsets count instruction slots, not their four encoding words.

## Completion, bounds and storage coherence

Submission completes before the next guest instruction. The host snapshots COPY
sources, giving overlapping RAM copies memmove semantics. All ranges and reserved
fields are checked before mutation. RAM endpoints exclude the first 32 MMIO cells;
a RAM destination may not overlap its descriptor. Buffers refer to ordinary RAM,
not legacy RGB565 display mappings. Code uploads must fit the backend's reserved
capacity and extend initialized code with NOPs, following STORC growth semantics.
With the MMU enabled, RVM code uploads invalidate compiled code across all address
spaces. Without the MMU, RVM invalidates translated regions covering the upload
and unwinds active stale JIT callers. RBF changes only code cells and extent
metadata through its logical tape interface, preserving CPU registers and
temporary lanes.

Disk offsets are octets in the attached image, with a 4-GiB address limit. Reads
past the image's current EOF are zero-padded, matching the legacy storage device.
Operations use the **same cached blocks** as STORE_DATA, including dirty data,
and never change STORE_BLOCK or STORE_ADDR. Writes preload all required blocks
before changing cached contents. Bounds, unsupported-operation and host-I/O
failures leave RAM/code/disk contents unchanged (a failed operation may populate
clean cache entries). Host-I/O errors also latch STORE_CTL.ERROR. Successful
writes mark affected cache blocks dirty; they do not flush or fsync implicitly.
Existing COMMIT/COMMIT_ALL operations remain durability barriers.

Results use the normal DEVICE_STATUS codes: OK, VERSION, BAD_DESCRIPTOR, BOUNDS,
UNSUPPORTED, BAD_STATE, or IO. An unsupported representation/command combination
is BAD_DESCRIPTOR. Guest libraries may fall back on UNKNOWN_COMMAND or
UNSUPPORTED; they must report other failures rather than hiding them with a retry
through an older path. The caller must serialize mailbox submission across tasks
and interrupt handlers. Bulk operations advance neither logical time nor the
instruction counter beyond the normal submitting STORE.

## C examples

`runtime/include/vm_bulk.h` supplies helpers in `libruntime.par`. They return a
device status. Descriptors and endpoints contain physical bank/offset pairs.
A pointer cast to `unsigned long` resolves C GP/SB tags before splitting it;
it does **not** translate virtual addresses to physical addresses. These examples
assume supervisor access to physical buffers or disabled translation. With the
[MMU](mmu.md) enabled, the kernel must translate or copy user buffers first;
protected user code cannot submit device commands directly.

```c
#include <vm_bulk.h>

unsigned short buffer[1024];
unsigned long address = (unsigned long)buffer;
unsigned status = vm_bulk_read(65534UL, (unsigned)(address >> 16),
    (unsigned)address, 1024UL, VM_BULK_WORDS);
/* Reads 2,048 disk octets across a legacy disk-block boundary. */

/* Upload 16 serialized instructions from RAM into code bank 32. */
status = vm_bulk_copy((unsigned)(address >> 16), (unsigned)address,
    32, 0, 16UL, VM_BULK_CODE_OCTETS);
/* On success, normal branches/LOADC see the new instructions immediately. */
```

## Fuzix integration

The BIOS loads each contiguous FAT-cluster span directly into kernel code or RAM,
and clears the remaining data bank with FILL. The filesystem's block driver uses
word transfers after mapping ROOTFS.IMG extents in the guest. In both profiles,
RXE execution uploads instruction batches from its physical filesystem stream
buffer. The guest still checks executable headers and relocation records and
manages processes.

The default RVM `MMU=1` profile switches process maps instead of copying user
banks. Fork shares code and COW data/stack pages; a write to a shared page copies
512 physical cells through the bulk RAM helper. Physical page initialization
uses bulk FILL. Fork also clones the parent's physical kernel stack through the
bank-copy helper.

The legacy `MMU=0` profile snapshots/restores fixed user data/stack banks during
fork and scheduling, and clones the parent's kernel stack. Each 64,000-cell bank
uses two submissions (32,768 and 31,232 cells), preserving all 16 bits per cell.
This profile also uses bulk operations to install/clear executable data and
applies physical code-base relocations with LOADC/STORC. MMU-profile code uses
virtual addresses starting at code bank zero and needs no physical-base edits.

Physical kernel-buffer and filesystem cache-buffer copies/clears of at least
256 cells use bulk operations. Small buffers retain direct copies. In the MMU
profile, copies involving the supervisor's translated user window use CPU
accesses, with user-memory validation and COW handling. This includes executable
data installation and `/dev/zero` destinations. Virtual addresses are never
submitted as physical device endpoints.

These kernel operations disable interrupts across each mailbox submission and
restore the previous interrupt mask. Assembly trap-frame moves and signal-frame
validation retain their existing paths. Unsupported memory operations use the
original loops; other errors become EIO for user-memory helpers or stop the
kernel when a physical page, task stack, or cache-buffer transfer cannot be
trusted. Toggling bulk I/O changes copy cost without changing either profile's
memory layout, the compiler ABI, or disk format.

Build Fuzix with `BULK_IO=0` to force scalar transfers for comparison; the build
records this setting and rebuilds affected objects. This flag does not disable
the MMU or restore bank snapshots. Fast/fallback builds must use the same `MMU`
setting and matching userspace. Unsupported bulk operations retain scalar
fallbacks, but the MMU profile still requires an MMU-capable RVM. RBF uses `MMU=0`.
Doom has not been switched to these helpers in this change.

## Validation checkpoint (16 October 2026)

The following results record the legacy `MMU=0` bulk-I/O integration before the
MMU/COW profile was added. Instruction counts and timings are not benchmarks of
the current default profile. Its additional protection/COW checks are described
in the [Fuzix validation guide](../../ports/fuzix/README.md#validation).

- 212 RVM/RBF unit and integration tests, 71 assembler/CPU unit tests, and eight
  native-runtime tests passed.
- `python3 scripts/test-bulk-io.py` passed with the real C helpers in RVM and RBF,
  both with and without JIT. Coverage includes global/stack pointer resolution,
  disk-block crossings, cache coherence and execution of uploaded code.
- Fuzix `tools/test.py` and `tools/test_unix.py` passed under both RVM engines,
  including timer preemption, fork/exec, indirect file I/O, pipes, persistence,
  executable-slot reuse, clean shutdown and offline recovery.
- The original loading-only workload used approximately 225 million guest
  instructions with bulk I/O versus 1.18 billion with `BULK_IO=0` (about 81%
  fewer), using identical userspace. The extended legacy `tools/test_bulk.py`
  workload ran repeated nested-fork stress tests, bank isolation, large user-stack/global
  clears, preemption, signals and native-word file-buffer round trips in both
  engines, with bulk enabled and forced off. Wall-clock timings are informational.
  With the extended workload, both engines recorded 1,024,902,712 instructions
  with bulk enabled versus 2,696,085,528 with `BULK_IO=0` (about 62% fewer).
- A separate JIT boot/`hello coffee`/`hello`/shutdown run dropped from about 7.4 s
  to 3.8 s and from 983 million to 159 million guest instructions.
- A bounded RBF/Fuzix smoke run completed BIOS bulk loading and reached the
  executing kernel's banner before its 90-second limit. A complete RBF shell
  session was not validated; focused CPU/C tests cover its transfer operations.
  After the process/cache-copy integration, a further 180-second run also
  completed BIOS loading and reached the kernel banner; a full RBF fork/shell
  session remains unvalidated.

Legacy RXE physical code-base relocation edits still use individual LOADC/STORC
operations. Bulk transfer removes physical payload-copy loops; it does not
eliminate filesystem traversal, executable validation, translated user-buffer
copies, ordinary guest computation or RBF's CPU emulation cost.
