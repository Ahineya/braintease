# Instruction set

Each instruction is **8 bytes** on disk:

```
byte0     byte1     bytes 2-3     bytes 4-5     bytes 6-7
opcode    pad       word1 LE      word2 LE      word3 LE
```

`pad` is unused by the VM (the assembler stores a copy of the opcode there). Operands are three 16-bit words. Formats:

| Format | word1, word2, word3 | Used by |
|--------|---------------------|---------|
| R | rd, rs, rt (register numbers) | ALU, `JALR`, `LOADC`/`STORC` |
| I | rd/rs, rs/rt, imm | ALU-imm, `LOAD`/`STORE`, branches, `JAL` |
| I1 | rd, imm, 0 | `LI` |

Source of truth: `src/ripple-asm/src/types.rs` (`enum Opcode`) and `rvm/src/vm/execution.rs`.

## Opcodes

### ALU (R)

| Hex | Mnemonic | Effect |
|-----|----------|--------|
| 00 | HALT | Stop. Operands ignored. |
| 42 | NOP | No operation. |
| 01 | ADD rd, rs, rt | rd ← rs + rt |
| 02 | SUB rd, rs, rt | rd ← rs − rt |
| 03 | AND rd, rs, rt | rd ← rs & rt |
| 04 | OR rd, rs, rt | rd ← rs \| rt |
| 05 | XOR rd, rs, rt | rd ← rs ^ rt |
| 06 | SLL rd, rs, rt | rd ← rs << (rt & 15) |
| 07 | SRL rd, rs, rt | rd ← rs >> (rt & 15) (logical) |
| 08 | SLT rd, rs, rt | rd ← (rs < rt) ? 1 : 0 (signed) |
| 09 | SLTU rd, rs, rt | unsigned compare |

### ALU immediate (I / I1)

| Hex | Mnemonic | Effect |
|-----|----------|--------|
| 0A | ADDI rd, rs, imm | rd ← rs + imm |
| 0B | ANDI rd, rs, imm | rd ← rs & imm |
| 0C | ORI rd, rs, imm | rd ← rs \| imm |
| 0D | XORI rd, rs, imm | rd ← rs ^ imm |
| 0E | LI rd, imm | rd ← imm (no shift) |
| 0F | SLLI rd, rs, imm | rd ← rs << (imm & 15) |
| 10 | SRLI rd, rs, imm | rd ← rs >> (imm & 15) |
| 1A | MUL rd, rs, rt | rd ← rs * rt |
| 1B | DIV rd, rs, rt | signed divide |
| 1C | MOD rd, rs, rt | signed remainder |
| 1D | MULI rd, rs, imm | |
| 1E | DIVI rd, rs, imm | |
| 1F | MODI rd, rs, imm | |

### Memory

| Hex | Mnemonic | Effect |
|-----|----------|--------|
| 11 | LOAD rd, bank, addr | rd ← DMEM[R\[bank\]]\[R\[addr\]\] (MMIO if bank 0 and addr < 1032) |
| 12 | STORE rs, bank, addr | DMEM[R\[bank\]]\[R\[addr\]\] ← rs |
| 20 | LOADC R0, bank, addr | X0..X3 ← IMEM[R\[bank\]]\[R\[addr\]\] (4 cells; bank/addr are registers) |
| 21 | STORC R0, bank, addr | IMEM[R\[bank\]]\[R\[addr\]\] ← X0..X3 |

**LOAD/STORE pitfall:** the VM always treats the bank and addr *fields* as **register indices**. An assembler immediate in those positions is that register number (`STORE R5, 0, 10` stores using `R10` as the address register, not DMEM\[0\]\[10\]). Put the address in a register (`LI T0, 10` / `STORE R5, R0, T0`), or use `R0` when the address really is 0 (TTY_OUT).

### Control flow

| Hex | Mnemonic | Effect |
|-----|----------|--------|
| 13 | JAL rd, bank, addr | If rd ≠ R0: rd ← PC+1 and RAB ← PCB. Then PCB ← bank, PC ← addr; skip increment. Bank and addr are **immediates** (or a label in word3). `JAL R0` is a non-linking jump and must not clobber RA/RAB (same rule as `JALR R0`). |
| 14 | JALR rd, bank_reg, addr_reg | Snapshot targets first. If rd ≠ R0: rd ← PC+1, RAB ← PCB. Then PCB ← R\[bank_reg\], PC ← R\[addr_reg\]. |
| 15 | BEQ rs, rt, imm | if rs==rt, PC ← PC+imm (signed), skip increment |
| 16 | BNE | |
| 17 | BLT | signed |
| 18 | BGE | signed |
| 19 | BRK | Debugger breakpoint (halt+dump unless `-d`) |

`CALL target` expands in the assembler to `JAL RA, R0, target`. **`rlink` then rewrites** `JAL` word2/word3 to `(bank, offset)` of the resolved label, so a linked `CALL` can enter any code bank. `RET` expands to `JALR R0, RAB, RA` (restores PCB from RAB). Same-bank hand-written code that never goes through the linker can use `JALR R0, R0, RA` if PCB is already correct.

Cross-bank C calls save RAB in the prologue because a nested `CALL` overwrites RAB.

### Special

- **HALT**: opcode `0x00` (operands ignored).
- **NOP**: opcode `0x42`.
- **IRQ dispatch** is not an instruction. See [mmio.md](mmio.md).

## Virtual instructions (assembler)

| Pseudo | Expansion |
|--------|-----------|
| MOVE rd, rs | ADD rd, rs, R0 |
| INC rd | ADDI rd, rd, 1 |
| DEC rd | ADDI rd, rd, -1 |
| CALL label | JAL RA, R0, label |
| RET | JALR R0, RAB, RA |
