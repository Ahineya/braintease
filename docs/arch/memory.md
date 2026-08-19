# Memory, banks, and fat pointers

## Data addressing

`LOAD rd, bankReg, addrReg` / `STORE rs, bankReg, addrReg` use two registers: a **bank id** and an **address within that bank**. There is no flat 32-bit load.

C pointers are therefore two words (**fat pointers**): `(addr, bank)`.

When a fat pointer is stored in memory, the bank word is either:

- a **tag**: Global = −1, Stack = −2, NULL = −3 (`rcc-backend/src/regmgmt/bank.rs`), or
- a non-negative **heap/dynamic bank number**.

On load, negative tags become `GP` or `SB`; non-negative values are used as the bank register after `LI`.

## Provenance

The compiler tracks a bank tag on every pointer-valued temporary (`BankTag` in `rcc-frontend/src/types/mod.rs`):

| Tag | Meaning | Bank register |
|-----|---------|----------------|
| Global | `.data` / `.rodata` / string | `GP` (crt0: 1) |
| Stack | alloca / frame | `SB` (crt0: 2) |
| Heap(n) | `malloc` result | immediate bank `n` |
| Unknown | parameter or loaded pointer | runtime tag / second word |
| Mixed | PHI/select of different regions | must not silently pick a bank |
| Null | NULL | must not dereference |

Pointer arithmetic goes through **GEP** so the backend can handle bank wrap (`addr + offset` may increment the bank when `addr` exceeds `BANK_SIZE`). Do not treat `ptr + i` as a 16-bit integer add.

Mixing stack and global pointers in a PHI (or loading a pointer from memory without a tag) is how you get a wrong-bank LOAD. The compiler should error rather than guess. Historical write-up: [`archive/about-pointers-and-provenance.md`](../archive/about-pointers-and-provenance.md).

## Bank map (C runtime)

Set in `runtime/crt0.asm` and `runtime/malloc.c`:

| Bank | Use |
|------|-----|
| 0 | MMIO + TEXT40 VRAM (see [mmio.md](mmio.md)). Not used as the C global bank. |
| 1 | Globals (`GP`) |
| 2 | Stack (`SB`), grows up from address 1 |
| 3–4 | Unused |
| 5–255 | Heap bump allocator. Allocations may span banks. |

`BANK_SIZE` (words) is injected when compiling the runtime (`-D BANK_SIZE=…`) and must match `rcc` / `rasm` / `rlink` / `rvm`.

## Instruction memory

`(PCB, PC)` indexes IMEM, not DMEM. The linked image may span several IMEM banks: `rlink` packs functions so `idx = PCB * bank_size + PC`, and patches `JAL` targets with the real bank and offset. Assembler `CALL label` is `JAL RA, R0, label` *before* linking; after `rlink` the bank immediate is no longer necessarily 0. `LOADC`/`STORC` move one instruction (X0–X3) for overlays / self-modifying sequences.
