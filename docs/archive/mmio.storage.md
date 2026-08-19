> **Deprecated (historical).** This file is not a current specification.
> Current architecture lives in [`docs/arch/`](../arch/README.md).
> See [`docs/archive/README.md`](README.md) for why this document was archived.

---

Ripple VM MMIO Storage Specification

Device: Word-Addressed RAMdisk with Commit

Overview

The Ripple VM provides a persistent block storage device via four MMIO registers in the Bank 0 header.
The design exposes a flat 16-bit word–addressed disk, structured as:
•	65536 blocks (selected via 16-bit register)
•	65536 words per block (addressed via 16-bit register)
•	Each word = 16 bits

Total Addressable Capacity:

65536 blocks × 65536 words × 2 bytes = 8 GiB

This layout balances simplicity (only 4 registers) with power (large address space).

⸻

MMIO Header Mapping

Address	Name	R/W	Description
17	HDR_STORE_BLOCK	W	Select current block (0–65535)
18	HDR_STORE_ADDR	W	Select word address within block (0–65535)
19	HDR_STORE_DATA	R/W	Data register: read/write 16-bit word at (BLOCK, ADDR)
20	HDR_STORE_CTL	R/W	Control register (busy/dirty/commit bits, see below)

⸻

Register Details

HDR_STORE_BLOCK (W)
•	16-bit value selecting active block number (0–65535).
•	All reads/writes to HDR_STORE_DATA apply to this block.

HDR_STORE_ADDR (W)
•	16-bit value selecting word address inside block (0–65535).
•	After each access to HDR_STORE_DATA, ADDR auto-increments by 1 (wraps at 0xFFFF).
•	This allows sequential streaming without repeatedly setting ADDR.

HDR_STORE_DATA (R/W)
•	Read: returns 16-bit word at (BLOCK, ADDR).
•	Write: updates 16-bit word at (BLOCK, ADDR) and marks block as dirty.
•	Auto-increments ADDR after each operation.

HDR_STORE_CTL (R/W)

Bit	Name	Description
0	BUSY	Read-only. 1 if VM is processing a storage operation.
1	DIRTY	Read/write. Set = current block has uncommitted writes.
2	COMMIT	Write-only. Writing 1 triggers commit of current block.
3	COMMIT_ALL	Write-only. Writing 1 triggers commit of all dirty blocks.
15–4	Reserved	Reads as 0.


⸻

Operation Model

Read Word
1.	Write BLOCK to HDR_STORE_BLOCK.
2.	Write word address to HDR_STORE_ADDR.
3.	Read HDR_STORE_DATA.

Write Word
1.	Write BLOCK to HDR_STORE_BLOCK.
2.	Write word address to HDR_STORE_ADDR.
3.	Write data to HDR_STORE_DATA.
4.	VM sets DIRTY=1 for that block.

Commit Block
1.	Write 0b100 (bit 2) to HDR_STORE_CTL.
2.	VM flushes the dirty block (128 KB) to host backing store.
3.	VM clears DIRTY flag.

Commit All
1.	Write 0b1000 (bit 3) to HDR_STORE_CTL.
2.	VM flushes all dirty blocks to host backing store.
3.	VM clears all DIRTY flags.

⸻

Backing Store Implementation
•	Host File: Backed by a sparse file up to 8 GiB.
•	Block Offset Calculation:

host_offset = (BLOCK × 65536 + ADDR) × 2 bytes


	•	Commit Granularity: Commit operations flush dirty blocks (128 KB each) for persistence.
	•	Dirty Tracking: Host maintains a 65,536-bit bitmap (8 KB) to track dirty blocks.

⸻

Example Usage

Writing a String to Block 42

; Set block 42
LI   A0, 42
LI   T1, 17       ; HDR_STORE_BLOCK
STORE A0, R0, T1

; Start at address 0
LI   A0, 0
LI   T1, 18       ; HDR_STORE_ADDR
STORE A0, R0, T1

; Write 'H'
LI   A0, 'H'
LI   T1, 19       ; HDR_STORE_DATA
STORE A0, R0, T1

; Write 'i'
LI   A0, 'i'
STORE A0, R0, T1  ; Auto-increment address

; Commit
LI   A0, 0b100    ; bit 2 = commit current block
LI   T1, 20       ; HDR_STORE_CTL
STORE A0, R0, T1

Reading Back

; Set block 42, address 0
LI   A0, 42
STORE A0, R0, 17
LI   A0, 0
STORE A0, R0, 18

; Read first char
LI   T1, 22
LOAD A0, T0, T1   ; A0 = 'H'


⸻

Design Rationale
1.	Simplicity: Only 4 registers control the entire 8 GiB space.
2.	Streaming-Friendly: Auto-increment address supports efficient block I/O.
3.	Commit Semantics: Allows deterministic persistence points (block or all).
4.	Scalability: Large logical capacity; sparse file avoids huge host usage.
5.	Flexibility: Can layer FAT16/FAT32 or custom FS over the raw block device.

