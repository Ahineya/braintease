# Ripple VM Assembly Language & Architecture

A design doc for the Ripple Virtual Machine architecture and its assembly language.

> Canonical spec: [`docs/arch/`](../docs/arch/README.md). Parts of this tutorial still mix older ABI sketches with the 32-register machine; prefer the `docs/arch` pages if they disagree.

## Table of Contents
1. [Architecture Overview](#architecture-overview)
2. [Register Architecture](#register-architecture)
3. [Instruction Set](#instruction-set)
4. [Memory Model](#memory-model)
5. [Assembly Syntax](#assembly-syntax)
6. [Calling Convention](#calling-convention)
7. [Pseudo-Instructions](#pseudo-instructions)
8. [Programming Examples](#programming-examples)
9. [System Integration](#system-integration)

## Architecture Overview

The Ripple VM is a 16-bit RISC architecture designed for efficient implementation in Brainfuck. It features:

The ISA is called "Perfectly Engineered Non-standard Instruction Set".

- **16-bit word size**: Values range from 0 to 65,535
- **32 general-purpose registers**: Including special-purpose registers
- **4-word instructions**: Each instruction is 64 bits (4 × 16-bit words)
- **Bank-based memory**: 65536 instructions per bank
- **Load/Store architecture**: LOAD/STORE access data memory; LOADC/STORC copy instruction cells
- **No flags register**: Comparisons set registers to 0 or 1

### Key Design Principles
2. **No modes** - Each instruction does exactly one thing
3. **Orthogonal design** - Any register can be used in any instruction
4. **Simple pipeline** - Fetch/Decode/Execute/Memory/Writeback

## Register Architecture

The Ripple VM features 32 registers with both numeric and symbolic names:

### Register Map

| Numeric | Symbolic | Purpose              | Notes                           |
|---------|----------|----------------------|---------------------------------|
| **R0**  | R0       | Hardware Zero        | Always reads 0, writes ignored  |
| **R1**  | PC       | Program Counter      | Current instruction address     |
| **R2**  | PCB      | Program Counter Bank | Current bank number             |
| **R3**  | RA       | Return Address       | Stores return address for calls |
| **R4**  | RAB      | Return Address Bank  | Return address bank             |
| **R5**  | RV0      | Return Value 0       | Function return value (low)     |
| **R6**  | RV1      | Return Value 1       | Function return value (high)    |
| **R7**  | A0       | Argument 0           | First function argument         |
| **R8**  | A1       | Argument 1           | Second function argument        |
| **R9**  | A2       | Argument 2           | Third function argument         |
| **R10** | A3       | Argument 3           | Fourth function argument        |
| **R11** | X0       | Extended 0           | Reserved for future use         |
| **R12** | X1       | Extended 1           | Reserved for future use         |
| **R13** | X2       | Extended 2           | Reserved for future use         |
| **R14** | X3       | Extended 3           | Reserved for future use         |
| **R15** | T0       | Temporary 0          | Caller-saved                    |
| **R16** | T1       | Temporary 1          | Caller-saved                    |
| **R17** | T2       | Temporary 2          | Caller-saved                    |
| **R18** | T3       | Temporary 3          | Caller-saved                    |
| **R19** | T4       | Temporary 4          | Caller-saved                    |
| **R20** | T5       | Temporary 5          | Caller-saved                    |
| **R21** | T6       | Temporary 6          | Caller-saved                    |
| **R22** | T7       | Temporary 7          | Caller-saved                    |
| **R23** | S0       | Saved 0              | Callee-saved                    |
| **R24** | S1       | Saved 1              | Callee-saved                    |
| **R25** | S2       | Saved 2              | Callee-saved                    |
| **R26** | S3       | Saved 3              | Callee-saved                    |
| **R27** | SC       | Scratch              | For register spilling           |
| **R28** | SB       | Stack Bank           | Bank for stack                  |
| **R29** | SP       | Stack Pointer        | Current stack position          |
| **R30** | FP       | Frame Pointer        | Current frame base              |
| **R31** | GP       | Global Pointer       | Bank for global data            |

### Register Classes

- **Hardware** (R0-R4): Special CPU registers
- **Return Values** (R5-R6): Function return values
- **Arguments** (R7-R10): Function arguments (4 max in registers)
- **Reserved** (R11-R14): Reserved for future extensions
- **Temporaries** (R15-R22): Caller-saved, freely usable
- **Saved** (R23-R26): Callee-saved, preserved across calls
- **Special** (R27-R31): Stack, globals, and scratch

## Instruction Set

### Instruction Formats

```
Format R:  [opcode] [rd] [rs] [rt]     - Register operations
Format I:  [opcode] [rd] [rs] [imm]    - Immediate operations
Format I1: [opcode] [rd] [imm] [0]     - Load immediate
Format I2: [opcode] [rd] [imm1] [imm2] - Jump and link
```

### ALU Operations (R-format)

| Opcode | Mnemonic | Operation                         | Example           |
|--------|----------|-----------------------------------|-------------------|
| 0x00   | NOP      | No operation                      | `NOP`             |
| 0x01   | ADD      | rd = rs + rt                      | `ADD T0, A0, A1`  |
| 0x02   | SUB      | rd = rs - rt                      | `SUB T1, T0, T2`  |
| 0x03   | AND      | rd = rs & rt                      | `AND S0, S1, S2`  |
| 0x04   | OR       | rd = rs \| rt                     | `OR T3, T4, T5`   |
| 0x05   | XOR      | rd = rs ^ rt                      | `XOR RV0, A0, A1` |
| 0x06   | SLL      | rd = rs << (rt & 15)              | `SLL T0, A0, T1`  |
| 0x07   | SRL      | rd = rs >> (rt & 15)              | `SRL T2, T0, T1`  |
| 0x08   | SLT      | rd = (rs < rt) ? 1 : 0 (signed)   | `SLT T0, A0, A1`  |
| 0x09   | SLTU     | rd = (rs < rt) ? 1 : 0 (unsigned) | `SLTU T1, A2, A3` |
| 0x1A   | MUL      | rd = rs * rt                      | `MUL RV0, A0, A1` |
| 0x1B   | DIV      | rd = rs / rt                      | `DIV T0, S0, S1`  |
| 0x1C   | MOD      | rd = rs % rt                      | `MOD T1, A0, T2`  |

### ALU Immediate Operations (I-format)

| Opcode | Mnemonic | Operation      | Example             |
|--------|----------|----------------|---------------------|
| 0x0A   | ADDI     | rd = rs + imm  | `ADDI SP, SP, -10`  |
| 0x0B   | ANDI     | rd = rs & imm  | `ANDI T0, A0, 0xFF` |
| 0x0C   | ORI      | rd = rs \| imm | `ORI T1, T2, 0x80`  |
| 0x0D   | XORI     | rd = rs ^ imm  | `XORI T3, T3, 1`    |
| 0x0E   | LI       | rd = imm       | `LI T0, 42`         |
| 0x0F   | SLLI     | rd = rs << imm | `SLLI T1, A0, 4`    |
| 0x10   | SRLI     | rd = rs >> imm | `SRLI T2, T1, 8`    |
| 0x1D   | MULI     | rd = rs * imm  | `MULI RV0, A0, 10`  |
| 0x1E   | DIVI     | rd = rs / imm  | `DIVI T0, T1, 100`  |
| 0x1F   | MODI     | rd = rs % imm  | `MODI T2, A0, 10`   |

### Memory Operations

| Opcode | Mnemonic | Operation | Example |
|--------|----------|-----------|---------|
| 0x11   | LOAD     | rd = MEM[bank][addr] | `LOAD T0, GP, T1` |
| 0x12   | STORE    | MEM[bank][addr] = rd | `STORE A0, SP, T0` |
| 0x20   | LOADC    | X0..X3 = IMEM[bank][addr] (4 instruction cells) | `LOADC R0, T0, T1` |
| 0x21   | STORC    | IMEM[bank][addr] = X0..X3 (4 instruction cells) | `STORC R0, T0, T1` |

`LOAD`/`STORE` use data memory. `LOADC`/`STORC` use instruction memory at the same (bank, offset) as PCB/PC. Bank and address are registers only; the first operand is unused (conventionally R0). X0 receives the opcode cell; X1–X3 receive the three operand cells.

### Control Flow

| Opcode | Mnemonic | Operation            | Example               |
|--------|----------|----------------------|-----------------------|
| 0x13   | JAL      | RA = PC+1, PC = addr | `JAL function`        |
| 0x14   | JALR     | rd = PC+1, PC = rs   | `JALR RA, T0`         |
| 0x15   | BEQ      | if(rs==rt) PC += imm | `BEQ A0, R0, done`    |
| 0x16   | BNE      | if(rs!=rt) PC += imm | `BNE T0, T1, loop`    |
| 0x17   | BLT      | if(rs<rt) PC += imm  | `BLT A0, A1, less`    |
| 0x18   | BGE      | if(rs>=rt) PC += imm | `BGE T0, T1, greater` |
| 0x19   | BRK      | Debugger breakpoint  | `BRK`                 |

## Memory Model

### Address Calculation
```
absoluteCell = PCB × BANK_SIZE + PC × 4 + localWord
```

### Memory Layout
- **Bank Size**: BANK_SIZE instructions
- **Total Address Space**: 65,536 banks × BANK_SIZE instructions
- **Word Size**: 16 bits
- **Instruction Size**: 4 words (64 bits)

### Memory-Mapped I/O

| Address | Name     | Description                    |
|---------|----------|--------------------------------|
| 0x0000  | OUT      | Write byte to host stdout      |
| 0x0001  | OUT_FLAG | Host ready flag (1 when ready) |

## Assembly Syntax

### Basic Syntax Rules
- **Case-insensitive**: `ADD`, `add`, and `Add` are equivalent
- **Comments**: Use `;` or `//` for line comments
- **Labels**: End with colon, e.g., `loop:`
- **Immediates**: Decimal by default, `0x` prefix for hex, `0b` for binary

### Example Assembly Code

```assembly
; Function to multiply by 10
multiply_by_10:
    ; S0 = A0 * 10
    MULI S0, A0, 10   ; Multiply first argument by 10
    JALR R0, RA       ; Return

; Main program
main:
    LI   A0, 7        ; Load 7 into first argument
    JAL  multiply_by_10
    ; Result is now in RV0
```

### Directives

| Directive | Description                   | Example           |
|-----------|-------------------------------|-------------------|
| `.code`   | Start code section            | `.code`           |
| `.data`   | Start data section            | `.data`           |
| `.word`   | Define 16-bit word            | `.word 0x1234`    |
| `.byte`   | Define 8-bit byte             | `.byte 65`        |
| `.space`  | Reserve space                 | `.space 100`      |
| `.ascii`  | Define ASCII string           | `.ascii "Hello"`  |
| `.asciiz` | Define null-terminated string | `.asciiz "World"` |

## Calling Convention

### Register Usage
- **Arguments**: A0-A3 (first 4 arguments)
- **Return Values**: RV0-RV1 (can return 32-bit values)
- **Caller-saved**: T0-T7 (must save before call if needed)
- **Callee-saved**: S0-S3 (function must preserve)
- **Stack**: Additional arguments at SP+offset

### Function Prologue/Epilogue

```assembly
function:
    ; Prologue - save callee-saved registers
    ADDI  SP, SP, -3      ; Allocate stack space
    STORE S0, SP, 0       ; Save S0
    STORE S1, SP, 1       ; Save S1
    STORE RA, SP, 2       ; Save return address
    
    ; Function body
    ; ... use S0, S1 freely ...
    
    ; Epilogue - restore registers
    LOAD  S0, SP, 0       ; Restore S0
    LOAD  S1, SP, 1       ; Restore S1
    LOAD  RA, SP, 2       ; Restore return address
    ADDI  SP, SP, 3       ; Deallocate stack
    JALR  R0, RA          ; Return
```

## Pseudo-Instructions

The assembler provides convenient pseudo-instructions that expand to real instructions:

| Pseudo        | Expansion                          | Description          |
|---------------|------------------------------------|----------------------|
| `HALT`        | `NOP` with all zeros               | Stop execution       |
| `MOVE rd, rs` | `ADD rd, rs, R0`                   | Copy register        |
| `PUSH rs`     | `STORE rs, SP, 0; ADDI SP, SP, -1` | Push to stack        |
| `POP rd`      | `ADDI SP, SP, 1; LOAD rd, SP, 0`   | Pop from stack       |
| `CALL label`  | `JAL label`                        | Function call        |
| `RET`         | `JALR R0, RA`                      | Return from function |
| `INC rd`      | `ADDI rd, rd, 1`                   | Increment            |
| `DEC rd`      | `ADDI rd, rd, -1`                  | Decrement            |
| `NEG rd`      | `SUB rd, R0, rd`                   | Negate               |
| `NOT rd`      | `XORI rd, rd, -1`                  | Bitwise NOT          |

## Programming Examples

TBD

## System Integration

### Linking with C Runtime

The Ripple VM can be targeted by the Ripple C Compiler (rcc), which generates assembly code following these conventions:

```assembly
; C function example
; int add(int a, int b) { return a + b; }
_add:
    ADD   RV0, A0, A1     ; Result in RV0
    JALR  R0, RA          ; Return
```

### Debug Support

The BRK instruction triggers debugger breakpoints, allowing:
- Register inspection
- Memory examination
- Single-step execution
- Breakpoint management

## Performance Considerations

It is a Brainfuck-based architecture, so performance is predictable:

`D.I.S.A.S.T.E.R.`

## Summary

The Ripple VM provides a clean, orthogonal RISC architecture that's both powerful and predictable. Its 32-register design eliminates many bottlenecks found in smaller register sets, while the simple instruction formats make both assembly programming and compiler development straightforward.

Key advantages:
- **Rich register set** reduces memory traffic
- **Simple addressing** makes code generation easier
- **Clean design** minimizes surprises
- **Future-proof** with reserved registers for extensions