> **Deprecated (historical).** This file is not a current specification.
> Current architecture lives in [`docs/arch/`](../arch/README.md).
> See [`docs/archive/README.md`](README.md) for why this document was archived.

---

## Practical RISC Instruction Set (32 instructions)

### The Instructions

| Opcode | Mnemonic        | Format | Description |
|--------|-----------------|--------|-------------|
| **0x00** | NOP             | R | No operation |
| **0x01** | ADD rd,rs,rt    | R | rd = rs + rt |
| **0x02** | SUB rd,rs,rt    | R | rd = rs - rt |
| **0x03** | AND rd,rs,rt    | R | rd = rs & rt |
| **0x04** | OR rd,rs,rt     | R | rd = rs \| rt |
| **0x05** | XOR rd,rs,rt    | R | rd = rs ^ rt |
| **0x06** | SLL rd,rs,rt    | R | rd = rs << (rt & 15) |
| **0x07** | SRL rd,rs,rt    | R | rd = rs >> (rt & 15) |
| **0x08** | SLT rd,rs,rt    | R | rd = (rs < rt) ? 1 : 0 |
| **0x09** | SLTU rd,rs,rt   | R | rd = unsigned compare |
| |                 | | |
| **0x10** | ADDI rd,rs,imm  | I | rd = rs + sign_ext(imm) |
| **0x11** | ANDI rd,rs,imm  | I | rd = rs & zero_ext(imm) |
| **0x12** | ORI rd,rs,imm   | I | rd = rs \| zero_ext(imm) |
| **0x13** | ORI rd,rs,imm   | I | rd = rs \| zero_ext(imm) |
| **0x14** | LI rd,imm       | I | rd = imm << 10 |
| **0x15** | SLLI rd,rs,imm  | I | rd = rs << imm |
| **0x16** | SRLI rd,rs,imm  | I | rd = rs >> imm |

| **0x17** | GETPC rd  | I | rd = PC |
| |                 | | |
| **0x20** | LOAD rd,rs,rt  | I | rd = mem[rsbank][rt] |
| **0x21** | STORE rd,rs,rt | I | mem[rs + imm] = rd |

| **0x30** | BEQ rs,rt,imm | I | if(rs==rt) PC += imm |
| **0x31** | BNE rs,rt,imm | I | if(rs!=rt) PC += imm |
| **0x32** | BLT rs,rt,imm | I | if(rs<rt) PC += imm |
| **0x33** | BGE rs,rt,imm | I | if(rs>=rt) PC += imm |
| | | | |
| **0x38** | J addr | J | PC = addr  |
| **0x39** | JAL addr | J | R7 = PC+1; PC = addr |
| **0x3A** | JR rs | R | PC = rs |
| **0x3B** | JALR rd,rs | R | rd = PC+1; PC = rs |
| | | | |
| **0x3E** | TRAP imm | J | System call |
| **0x3F** | HALT | J | Stop execution |

### Registers (Clean AF)

| Register | Purpose |
|----------|---------|
| **R0** | Always zero (hardwired) |
| **R1-R6** | General purpose |
| **R7** | Link register (return address) |
| **SP** | Stack pointer (separate register) |
| **PC** | Program counter |

### Why This Doesn't Suck

1. **Predictable timing** - Every instruction takes known cycles
2. **No modes** - Instruction does ONE thing
3. **Orthogonal** - Any register can be used anywhere
4. **Simple pipeline** - Fetch/Decode/Execute/Memory/Writeback
5. **R0 = 0** - Free zero constant, makes so much shit easier

### Example: Multiply by 10

```asm
    ; R2 = R1 * 10
    SLLI R2, R1, 3    ; R2 = R1 * 8
    SLLI R3, R1, 1    ; R3 = R1 * 2
    ADD  R2, R2, R3   ; R2 = R1*8 + R1*2
```

Clean, predictable, no weird addressing mode fuckery. This is what a sane person would implement in Brainfuck 😂

------------------

Hell yeah! Let me build you the complete reference table with all the juicy details.

## Complete RISC Instruction Reference

### Architecture Overview
- **NO FLAGS REGISTER** - This is pure RISC philosophy
- Comparisons set a register to 1 or 0 instead of flags
- Branches compare registers directly
- Simpler hardware, easier to pipeline, less state to track

### Detailed Instruction Table

| Opcode   | Mnemonic | Format | Operation                         | Description                          | Example           | Cycles |
|----------|----------|--------|-----------------------------------|--------------------------------------|-------------------|--------|
| **0x00** | NOP      | R      | -                                 | No operation, advance PC             | `NOP`             | 1      |
| **0x01** | ADD      | R      | rd = rs + rt                      | Add two registers                    | `ADD R3,R1,R2`    | 1      |
| **0x02** | SUB      | R      | rd = rs - rt                      | Subtract rt from rs                  | `SUB R3,R1,R2`    | 1      |
| **0x03** | AND      | R      | rd = rs & rt                      | Bitwise AND                          | `AND R3,R1,R2`    | 1      |
| **0x04** | OR       | R      | rd = rs \| rt                     | Bitwise OR                           | `OR R3,R1,R2`     | 1      |
| **0x05** | XOR      | R      | rd = rs ^ rt                      | Bitwise XOR                          | `XOR R3,R1,R2`    | 1      |
| **0x06** | SLL      | R      | rd = rs << (rt & 15)              | Shift left logical                   | `SLL R3,R1,R2`    | 1      |
| **0x07** | SRL      | R      | rd = rs >> (rt & 15)              | Shift right logical (no sign extend) | `SRL R3,R1,R2`    | 1      |
| **0x08** | SLT      | R      | rd = (rs < rt) ? 1 : 0            | Set if less than (signed)            | `SLT R3,R1,R2`    | 1      |
| **0x09** | SLTU     | R      | rd = (rs < rt) ? 1 : 0            | Set if less than (unsigned)          | `SLTU R3,R1,R2`   | 1      |
|          |          |        |                                   |                                      |                   |        |
| **0x10** | ADDI     | I      | rd = rs + sign_ext(imm)           | Add immediate (-32 to +31)           | `ADDI R2,R1,10`   | 1      |
| **0x11** | ANDI     | I      | rd = rs & zero_ext(imm)           | AND with immediate (0-63)            | `ANDI R2,R1,0x3F` | 1      |
| **0x12** | ORI      | I      | rd = rs \| zero_ext(imm)          | OR with immediate (0-63)             | `ORI R2,R1,0x0F`  | 1      |
| **0x13** | LUI      | I      | rd = imm << 10                    | Load upper immediate                 | `LUI R1,0x3F`     | 1      |
| **0x14** | SLLI     | I      | rd = rs << (imm & 15)             | Shift left by immediate              | `SLLI R2,R1,4`    | 1      |
| **0x15** | SRLI     | I      | rd = rs >> (imm & 15)             | Shift right by immediate             | `SRLI R2,R1,4`    | 1      |
|          |          |        |                                   |                                      |                   |        |
| **0x20** | LW       | I      | rd = mem[rs + sign_ext(imm)]      | Load word (16-bit)                   | `LW R2,R1,8`      | 2      |
| **0x21** | SW       | I      | mem[rs + sign_ext(imm)] = rd      | Store word (16-bit)                  | `SW R2,R1,8`      | 2      |
| **0x22** | LB       | I      | rd = byte mem[rs + sign_ext(imm)] | Load byte (sign extend)              | `LB R2,R1,0`      | 2      |
| **0x23** | SB       | I      | byte mem[rs + sign_ext(imm)] = rd | Store byte (low 8 bits)              | `SB R2,R1,0`      | 2      |
|          |          |        |                                   |                                      |                   |        |
| **0x30** | BEQ      | I      | if(rs==rt) PC += sign_ext(imm)*2  | Branch if equal                      | `BEQ R1,R2,loop`  | 1-2    |
| **0x31** | BNE      | I      | if(rs!=rt) PC += sign_ext(imm)*2  | Branch if not equal                  | `BNE R1,R2,skip`  | 1-2    |
| **0x32** | BLT      | I      | if(rs<rt) PC += sign_ext(imm)*2   | Branch if less than                  | `BLT R1,R2,less`  | 1-2    |
| **0x33** | BGE      | I      | if(rs>=rt) PC += sign_ext(imm)*2  | Branch if greater/equal              | `BGE R1,R2,more`  | 1-2    |
|          |          |        |                                   |                                      |                   |        |
| **0x38** | J        | J      | PC = addr * 2                     | Jump to address                      | `J start`         | 2      |
| **0x39** | JAL      | J      | R7 = PC+2; PC = addr*2            | Jump and link (call)                 | `JAL printf`      | 2      |
| **0x3A** | JR       | R      | PC = rs                           | Jump to register                     | `JR R1`           | 2      |
| **0x3B** | JALR     | R      | rd = PC+2; PC = rs                | Jump and link register               | `JALR R7,R1`      | 2      |
|          |          |        |                                   |                                      |                   |        |
| **0x3E** | TRAP     | J      | System call                       | Trap to OS/BIOS                      | `TRAP 0x10`       | 3+     |
| **0x3F** | HALT     | J      | Stop execution                    | Halt processor                       | `HALT`            | 1      |