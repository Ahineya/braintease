# Optional memory management

RVM implements MMU ABI 1 in both its interpreter and JIT. Reset starts in
supervisor mode with translation disabled, preserving the existing Ripple ABI.
The Brainfuck CPU uses the existing unprotected profile; BF exports reject MMU
instructions and MMU CSRs. It does not emulate protection through host devices.

The MMU translates existing `(bank, offset)` addresses rather than changing C
pointers or the instruction format. There are separate instruction and data
maps, backed by physical RAM tables. Device descriptors and bulk transfers
continue to contain **physical** addresses. A kernel must translate or copy user
buffers before submitting them to a device.

## Modes and instructions

With translation enabled, every user instruction fetch, LOAD/STORE and LOADC
checks its corresponding map. Offsets must be less than the configured bank
size. User mappings require the USER flag and cannot target physical bank zero.
An absent entry, missing permission, malformed table or out-of-range physical
address raises a synchronous CPU fault. An access has no partial side effects.

Supervisor instruction fetches are physical. Supervisor data accesses are also
physical except for an optional inclusive virtual-bank window. Accesses within
that window use the data map and enforce read/write permissions, including
copy-on-write faults. USER is required only in user mode. Physical bank zero
cannot be the target of any translated mapping; direct supervisor accesses keep
all existing MMIO behavior. Kernel and IRQ stacks must be placed outside the
translated window.

| Instruction | Encoding | Effect |
| --- | --- | --- |
| `SYSCALL` | Opcode 0x25, no operands | User enters the supervisor trap vector; saved PC points after SYSCALL |
| `ENTERU R0, rb, ro` | Opcode 0x26, R format | Supervisor enters user mode at the address in rb:ro; MMU must be enabled, word1 must be zero |

User HALT, BRK, STORC, IRET, CSRW and ENTERU raise privilege faults. User CSRR
can read only MMU_VERSION, MMU_FEATURES and MODE. Unknown user instructions raise
illegal-instruction faults. Ordinary instructions retain their existing operand
semantics. All system instructions leave native execution before changing CPU
control state.

## Control registers

These are CPU state, accessed by CSRR/CSRW, **not** MMIO cells. The existing
interrupt CSRs 0–11 retain their numbering. All writes below are privileged.

| CSR | Name | Meaning |
| --- | --- | --- |
| 12 | MMU_VERSION | Read-only, 1 |
| 13 | MMU_CONTROL | Bit 0 enables translation; bit 1 enables the supervisor data window; other bits rejected |
| 14, 15 | DROOT_BANK, DROOT_OFF | Physical data-map directory pointer |
| 16, 17 | IROOT_BANK, IROOT_OFF | Physical instruction-map directory pointer |
| 18, 19 | WINDOW_LO, WINDOW_HI | Inclusive virtual-bank range for supervisor data translation; LO > HI gives an empty window |
| 20, 21 | TRAP_BANK, TRAP_OFF | Physical supervisor synchronous-trap vector |
| 22 | TRAP_CAUSE | Read-only synchronous cause, zero for a hardware/software IRQ |
| 23, 24 | FAULT_BANK, FAULT_OFF | Read-only virtual address of the fault; for syscall/privilege/illegal traps, the instruction address |
| 25 | SAVED_MODE | Interrupted mode: 0 supervisor, 1 user; writable only with a live CPU trap frame, user return requires MMU enabled |
| 26 | MODE | Read-only current mode, 0 supervisor or 1 user |
| 27 | MMU_FLUSH | Write bit 0 to flush address translations, bit 1 to also discard compiled instruction translations; reads zero; writing zero does nothing; unknown bits rejected |
| 28 | MMU_FEATURES | Read-only 3: bit 0 bank maps, bit 1 512-cell pages |

CONTROL, root and window writes flush address translations. Table stores alone
need not become visible until a flush. Changing instruction mappings requires a
code flush as well. STORC and bulk code writes invalidate all compiled address
spaces. Data-only changes, such as resolving COW, retain compiled code.

## Physical table layout

All entries are native 16-bit cells. Table pointers are concrete physical
`(bank, offset)` pairs; bank zero is forbidden and the initial offset must be
canonical. Table arrays may continue across physical bank boundaries. Unmapped
or malformed table reads fault instead of accessing MMIO.

1. Each root points to a 256-entry directory. Two cells per entry hold the
   physical pointer to a leaf. Index with virtual bank >> 8. A zero pointer
   denotes an absent leaf.
2. A leaf has 256 four-cell bank entries, indexed with virtual bank & 255:
   `[target_bank, target_offset, flags, reserved]`.
3. With PAGED clear, target_offset must be zero; target_bank maps the whole
   virtual bank to a physical bank.
4. With PAGED set, target points to 128 four-cell page entries. Index with
   virtual offset / 512. A page entry has the same layout, with PAGED clear and
   its physical target_offset aligned to 512 cells.

| Flag | Value | Meaning |
| --- | --- | --- |
| PRESENT | 1 | Entry exists |
| READ | 2 | LOAD or LOADC permitted |
| WRITE | 4 | STORE permitted |
| EXECUTE | 8 | Instruction fetch permitted |
| USER | 16 | User access permitted |
| PAGED | 32 | Bank entry points to a page table |
| COW | 64 | Software marker; hardware still faults when WRITE is clear |

Reserved cells and unknown flag bits must be zero. Parent and page permissions
are intersected. Physical page accesses cannot spill into the next bank, even
when bank size is not divisible by 512. All 16-bit virtual bank numbers work;
the current JIT bounds its dense entry tables to virtual code banks 0–255 and
interprets user code above that range.

## Traps, scheduling and COW

| Cause | Meaning |
| --- | --- |
| 1 | SYSCALL |
| 2 | Data/instruction-memory read fault |
| 3 | Data write fault |
| 4 | Instruction-fetch fault |
| 5 | Privileged instruction |
| 6 | Illegal instruction |

Synchronous traps use the existing dedicated IRQ stack and CPU continuation
frame. Entry preserves general registers, saves PC/PCB, interrupt enable,
SB/SP/FP and the previous mode, sets IRQ_BUSY, switches to supervisor mode, and
jumps to TRAP_BANK:TRAP_OFF. A synchronous trap is already acknowledged and does
not create a pending device interrupt. The kernel must configure a nonzero IRQ
stack top first. A fault while IRQ_BUSY is set is a fatal double fault.

IRET restores the saved mode in addition to the existing continuation state.
Returning to user mode with translation disabled is an execution error.
Faults save the failing instruction's PC for retry; SYSCALL saves the next PC.
Each faulting instruction attempt, including a fetch fault, counts as one
executed instruction. The JIT publishes register state before invoking the same
trap path as the interpreter.

Hardware/software IRQ entry also saves the mode and switches to supervisor.
Roots are **not** part of the automatic frame: a scheduler selects the next
process's roots explicitly before restoring its continuation. SAVED_MODE lets a
kernel leave a trap through a supervisor trampoline, clear IRQ_BUSY, then run
blocking syscall or scheduling code. A software IRQ can subsequently restore a
complete user register frame with IRET.

For COW, the kernel shares physical pages, increments its own reference counts,
clears WRITE and sets COW. A write fault either copies a shared page or restores
WRITE when only one reference remains, flushes data translations, and retries.
There is no hardware allocation, eviction or disk paging policy.
