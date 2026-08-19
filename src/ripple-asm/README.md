# RASM - Ripple Assembler

A complete assembler, linker, and disassembler toolchain for the Ripple VM architecture.

## Overview

RASM is a RISC-like assembler that compiles assembly code for the Ripple Virtual Machine. It features:
- 16-bit architecture with configurable bank size
- 18 registers (including special-purpose registers)
- Rich instruction set with arithmetic, logical, memory, and control flow operations
- Virtual instructions that expand to real instruction sequences
- Two-pass assembly with label resolution
- Integrated linker for multi-module programs
- Disassembler for reverse engineering binaries

## Installation

### Building from Source

```bash
cd src/ripple-asm
cargo build --release
```

The binaries will be available in `target/release/`:
- `rasm` - The assembler
- `rlink` - The linker

## Usage

### Assembling

```bash
# Assemble to object file (default)
rasm assemble input.asm -o output.pobj

# Assemble directly to binary
rasm assemble input.asm -f binary -o output.bin

# Assemble to macro format (Brainfuck macros)
rasm assemble input.asm -f macro -o output.bfm

# With custom options
rasm assemble input.asm \
  --bank-size 4096 \
  --max-immediate 65535 \
  --memory-offset 2
```

### Linking

```bash
# Link object files to binary
rlink file1.pobj file2.pobj -o program.bin

# Link to macro format
rlink file.pobj -f macro -o program.bfm

# Create archive (library)
rlink --archive lib1.pobj lib2.pobj -o mylib.par

# Link with archives
rlink main.pobj mylib.par -o program.bin
```

### Disassembling

```bash
# Disassemble binary to assembly
rasm disassemble program.bin -o program.asm

# Works with both RASM and RLINK binary formats
```

### Other Commands

```bash
# Show instruction reference
rasm --reference

# Validate assembly file
rasm check input.asm

# Convert object file to macro format
rasm format input.pobj -o output.bfm
```

## Architecture

### Registers

32 registers. Numeric (`R0`–`R31`) and symbolic names are interchangeable. Full map: [`docs/arch/registers.md`](../../docs/arch/registers.md).

| Register | Index | Purpose |
|----------|-------|---------|
| R0 | 0 | Always reads as 0 |
| PC / PCB | 1 / 2 | Program counter |
| RA / RAB | 3 / 4 | Return address |
| RV0 / RV1 | 5 / 6 | Return values |
| A0–A3 | 7–10 | Arguments |
| X0–X3 | 11–14 | IMEM copy / wide scratch |
| T0–T7 | 15–22 | Temporaries (caller-saved) |
| S0–S3 | 23–26 | Callee-saved |
| SC / SB / SP / FP / GP | 27–31 | Scratch, stack bank, stack, frame, global bank |

C `crt0` uses SB=2, SP/FP=1, GP=1. See [`docs/arch/abi.md`](../../docs/arch/abi.md).

### Instruction Set

#### Arithmetic Instructions
- `ADD rd, rs1, rs2` - Addition
- `SUB rd, rs1, rs2` - Subtraction  
- `MUL rd, rs1, rs2` - Multiplication
- `DIV rd, rs1, rs2` - Division
- `MOD rd, rs1, rs2` - Modulo
- `ADDI rd, rs, imm` - Add immediate
- `MULI rd, rs, imm` - Multiply immediate
- `DIVI rd, rs, imm` - Divide immediate
- `MODI rd, rs, imm` - Modulo immediate

#### Logical Instructions
- `AND rd, rs1, rs2` - Bitwise AND
- `OR rd, rs1, rs2` - Bitwise OR
- `XOR rd, rs1, rs2` - Bitwise XOR
- `ANDI rd, rs, imm` - AND immediate
- `ORI rd, rs, imm` - OR immediate  
- `XORI rd, rs, imm` - XOR immediate

#### Shift Instructions
- `SLL rd, rs1, rs2` - Shift left logical
- `SRL rd, rs1, rs2` - Shift right logical
- `SLLI rd, rs, imm` - Shift left immediate
- `SRLI rd, rs, imm` - Shift right immediate

#### Comparison
- `SLT rd, rs1, rs2` - Set less than (signed)
- `SLTU rd, rs1, rs2` - Set less than (unsigned)

#### Memory Instructions
- `LI rd, imm` - Load immediate
- `LOAD rd, bank, addr` - Load from data memory
- `STORE rs, bank, addr` - Store to data memory
- `LOADC R0, bank, addr` - Load 4 instruction cells from IMEM[bank][addr] into X0..X3 (registers only)
- `STORC R0, bank, addr` - Store X0..X3 to 4 instruction cells at IMEM[bank][addr] (registers only)

#### Control Flow
- `JAL rd, target` - Jump and link (absolute)
- `JALR rd, rs, offset` - Jump and link register
- `BEQ rs1, rs2, label` - Branch if equal
- `BNE rs1, rs2, label` - Branch if not equal
- `BLT rs1, rs2, label` - Branch if less than
- `BGE rs1, rs2, label` - Branch if greater or equal

#### Special Instructions
- `NOP` - No operation (opcode 0x42)
- `BRK` - Breakpoint (triggers debugger)
- `HALT` - Stop execution (opcode 0x00)

### Virtual Instructions

These expand to real instruction sequences:

- `MOVE rd, rs` → `ADD rd, rs, R0`
- `INC rd` → `ADDI rd, rd, 1`
- `DEC rd` → `ADDI rd, rd, -1`
- `PUSH rs` → Stack push (2 instructions)
- `POP rd` → Stack pop (2 instructions)
- `CALL target` → `JAL RA, R0, target` (assembler); `rlink` patches the bank/offset
- `RET` → `JALR R0, RAB, RA`

## Assembly Syntax

### Basic Structure

```asm
.code           ; Code section
main:           ; Label definition
    LI R3, 42   ; Load immediate
    ADD R4, R3, R3  ; R4 = R3 + R3
    BEQ R4, R0, end ; Branch if R4 == 0
    HALT        ; Stop execution
end:
    RET         ; Return

.data           ; Data section
message:
    .asciiz "Hello, World!"  ; Null-terminated string
    .byte 0x41, 0x42         ; Raw bytes
    .word 1000, 2000         ; 16-bit words
```

### Comments

```asm
# Hash comment
; Semicolon comment  
// C-style comment
```

### Number Formats

```asm
42          ; Decimal
0x2A        ; Hexadecimal
0b101010    ; Binary
-42         ; Negative (two's complement)
```

### Directives

- `.code` / `.text` - Start code section
- `.data` - Start data section
- `.byte` / `.db` - Define bytes
- `.word` / `.dw` - Define 16-bit words
- `.asciiz` - Define null-terminated string
- `.string` - Define string (no null terminator)

## Examples

### Hello World

```asm
.code
main:
    LI R3, 0        ; Memory address for string
loop:
    LOAD R4, R0, R3 ; Load character
    BEQ R4, R0, done ; Check for null terminator
    STORE R4, R0, 0 ; Output character (memory-mapped I/O)
    INC R3          ; Next character
    BEQ R0, R0, loop ; Unconditional branch
done:
    HALT

.data
    .asciiz "Hello, World!\n"
```

### Function Call

```asm
.code
main:
    LI R3, 5
    LI R4, 3
    CALL multiply   ; Call function
    ; Result in R5
    HALT

multiply:
    MUL R5, R3, R4
    RET
```

### Stack Operations

```asm
.code
    LI R13, 1000    ; Initialize stack pointer
    LI R3, 42
    PUSH R3         ; Push value
    ; ... do other work ...
    POP R4          ; Pop value into R4
    HALT
```

## Binary Format

### Object File Format (.pobj)

JSON format containing:
- `version`: Format version
- `instructions`: Array of assembled instructions
- `data`: Data section bytes
- `labels`: Symbol table with addresses
- `unresolved_references`: External symbols
- `entry_point`: Optional entry point label

### Binary Format (.bin)

#### RASM Format (from assembler)
```
[0x00] "RASM" - Magic number (4 bytes)
[0x04] Version (4 bytes, little-endian)
[0x08] Instruction count (4 bytes, little-endian)
[0x0C] Instructions (8 bytes each)
[...] Data size (4 bytes, little-endian)
[...] Data section
```

#### RLINK Format (from linker)
```
[0x00] "RLINK" - Magic number (5 bytes)
[0x05] Entry point (4 bytes, little-endian)
[0x09] Instruction count (4 bytes, little-endian)
[0x0D] Instructions (8 bytes each)
[...] Data size (4 bytes, little-endian)
[...] Data section
```

### Instruction Encoding

Each instruction is 8 bytes:
```
[0] Opcode (1 byte)
[1] Reserved (1 byte, copy of opcode)
[2-3] Word1 (2 bytes, little-endian)
[4-5] Word2 (2 bytes, little-endian)
[6-7] Word3 (2 bytes, little-endian)
```

## Configuration Options

### Assembler Options

- `--bank-size <size>` - Bank size for memory addressing (default: 16)
- `--max-immediate <value>` - Maximum immediate value (default: 65535)
- `--memory-offset <offset>` - Memory offset for data addresses (default: 2)
- `--case-insensitive <bool>` - Case insensitive parsing (default: true)

### Linker Options

- `--entry <label>` - Set entry point (default: first instruction)
- `--format <fmt>` - Output format: binary or macro (default: binary)
- `--standalone` - Create standalone executable
- `--archive` - Create archive file

## Integration with Ripple VM

The assembled binaries can be executed using the Ripple VM:

```bash
# Run a binary
rvm program.bin

# With options
rvm program.bin \
  --bank-size 4096 \
  --memory 65536 \
  --debug
```

## Integration with C Compiler

RASM is used as the backend assembler for the Ripple C compiler:

```bash
# C → Assembly → Binary pipeline
rcc compile program.c -o program.asm
rasm assemble program.asm -o program.pobj
rlink program.pobj runtime.par -o program.bin
rvm program.bin
```

## Error Handling

The assembler provides detailed error messages:
- Line numbers for syntax errors
- Undefined label detection
- Invalid instruction format warnings
- Immediate value overflow detection
- Unresolved reference reporting

## License

Part of the Ripple VM toolchain.