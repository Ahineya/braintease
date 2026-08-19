# Machine model

Ripple is a 16-bit, banked RISC VM. The C toolchain (`rcc` → `rasm` → `rlink` → `rvm`) is the primary target. A Brainfuck backend still exists for programs that do not need MMIO.

## Cells and arithmetic

- Every register and data-memory location is a **16-bit cell** (0…65535).
- Arithmetic wraps modulo 2¹⁶.
- There is **no flags register**. Comparisons write 0 or 1 into a register (`SLT` / `SLTU`); branches compare two registers.
- C `char` occupies one cell (value in the low 8 bits). C `sizeof` is still in **bytes**: `sizeof(char) == 1`, `sizeof(int) == 2`. See [abi.md](abi.md).

## Two memories

Instructions and data are **separate**:

| Memory | Addressed by | Unit |
|--------|--------------|------|
| Instruction (IMEM) | `(PCB, PC)` | one instruction (4 cells / 8 bytes on disk) |
| Data (DMEM) | `(bank, addr)` via `LOAD`/`STORE` | one 16-bit cell |

`LOADC` / `STORC` copy one instruction (X0…X3) between IMEM and registers. They do not touch DMEM.

## Banks

A **bank size** is the number of instructions (IMEM) or the address modulus the tools agree on for a build. It is **not** fixed at 4096.

- `rct` and `runtime/Makefile` default to **64000**.
- `rcc` CLI default is 4096; `rasm`/`rlink` defaults differ — always pass the same `--bank-size` through the pipeline.
- Instruction index: `idx = PCB * bank_size + PC`. When `PC` reaches `bank_size`, the VM wraps `PC` to 0 and increments `PCB`.
- A program **may span multiple IMEM banks**. `rlink` packs objects (or individual functions, if an object is larger than one bank) so bank N starts at instruction index `N * bank_size`, NOP-padding unused tails. `rvm` loads the whole image; fetch is `PCB * bank_size + PC`. The remaining limit is that **one function** must still fit in a single bank (the linker errors if a function is larger than `--bank-size`).

Data banks used by the C runtime (see [memory.md](memory.md)):

| Bank | Role |
|------|------|
| 0 | MMIO header (words 0–31), TEXT40 VRAM (32–1031), then unused/legacy data |
| 1 | C globals / rodata (`GP = 1`) |
| 2 | C stack (`SB = 2`, `SP`/`FP` grow **upward** from 1) |
| 3–4 | Unused by crt0/malloc |
| 5–255 | Heap (`malloc`) |

## Execution

```
SETUP → RUNNING
while RUNNING:
    maybe dispatch IRQ          # see mmio.md
    fetch instruction at (PCB, PC)
    execute
    unless skip_pc_increment: PC += 1 (and maybe PCB += 1)
HALT  → stop
```

Control-flow instructions (`JAL`, `JALR`, branches) set `skip_pc_increment` after writing `PC`/`PCB`.

## Toolchain

```
C  →  rcc  →  .asm  →  rasm  →  .pobj  →  rlink (+ crt0 + libruntime)  →  .bin  →  rvm
```

Commands and bank-size rules: [`TOOLCHAIN.md`](../../TOOLCHAIN.md).
