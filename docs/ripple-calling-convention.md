# Ripple VM Calling Convention

CRITICAL! VM is 16-bit. There is no way to store two separate 8-bit values in a single 16-bit register.
All the values (char, etc) are stored in 16-bit, and the calling convention is designed to handle this.

## Register Assignments

### Register Numbering (Hardware)
```
R0  = 0   // Always zero
PC  = 1   // Program counter
PCB = 2   // Program counter bank
RA  = 3   // Return address
RAB = 4   // Return address bank
R3  = 5   // General purpose
R4  = 6   // General purpose
R5  = 7   // General purpose
R6  = 8   // General purpose
R7  = 9   // General purpose
R8  = 10  // General purpose
R9  = 11  // General purpose
R10 = 12  // General purpose
R11 = 13  // General purpose
R12 = 14  // General purpose
R13 = 15  // General purpose
R14 = 16  // General purpose
R15 = 17  // General purpose
```

### Special Purpose Registers
- **R0** (0): Always zero (hardware constraint)
- **PC** (1): Program counter
- **PCB** (2): Program counter bank
- **RA** (3): Return address register (used by JAL/JALR)
- **RAB** (4): Return address bank

### Convention Registers
- **R3** (5): Return value register (or pointer address for fat pointers)
- **R4** (6): Second return value (or pointer bank for fat pointers)
- **R5-R11** (7-13): General purpose, allocatable registers (7 registers)
- **R12** (14): Scratch register - reserved for address calculations during spill/reload
- **R13** (15): Stack bank register (SB) - holds bank ID for stack (initialized to 1)
- **R14** (16): Stack pointer (SP)
- **R15** (17): Frame pointer (FP)

### Register Classes
- **Caller-saved**: R3-R11 (return values and allocatable registers)
- **Callee-saved**: None in current convention
- **Allocatable pool**: [R5, R6, R7, R8, R9, R10, R11]
- **Reserved**: R12 (scratch), R13 (SB), R14 (SP), R15 (FP)

## Stack Frame Layout

```
Higher addresses
+----------------+
| Previous frame |
+================+ <- FP (Frame Pointer)
| Local vars     |
| FP+0 .. FP+L-1 |
+----------------+
| Spill slots    |
| FP+L .. FP+L+S-1|
+----------------+ <- SP (Stack Pointer)
| Next frame     |
+----------------+
Lower addresses
```

Where:
- L = number of local variable slots
- S = number of spill slots for temporaries

## Function Prologue

```asm
; Initialize stack bank register (if not already set)
LI    R13, 1            ; SB = 1 (stack in bank 1)

; Set up frame
ADD   FP, SP, R0        ; Set frame pointer to current stack pointer
ADDI  SP, SP, -(L+S)    ; Allocate stack frame
```

Note: R13 initialization may be done once at program start rather than in every function.
R12 is reserved as a scratch register for spill/reload address calculations.

## Function Epilogue

```asm
ADD   SP, FP, R0        ; Restore stack pointer
JALR  R0, R0, RA        ; Return to caller
```

## Calling Convention

### Before Call
1. Spill all live registers (conservative approach for M3/M4)
2. Evaluate arguments
3. Pass arguments according to ABI (stack-based for now)

### Call Instruction
```asm
JAL bankImm, addrImm    ; Sets RA/RAB, jumps to function
```

### After Call
- **Scalar result (16-bit)**: In R3
- **Pointer result**: Address in R3, bank in R4 (fat pointer)
- **32-bit result**: Low 16 bits in R3, high 16 bits in R4
- All caller-saved registers are considered clobbered
- Reload any spilled values as needed

### Fat Pointer Format
Pointers consist of two components:
- **Address**: Memory address within bank
- **Bank tag**: Identifies memory region

### Bank Tag Values
- `0`: Global memory (.rodata/.data) - use R0 for bank (always reads 0)
- `1`: Stack memory (frame/alloca) - stored in R13 (SB)
- `2`: Reserved for future heap

### Bank Register Usage
- **R0**: Used for global bank access (globals are in bank 0, R0 always reads 0)
- **R12**: Reserved as scratch register for address calculations during spill/reload
- **R13 (SB)**: Initialize to 1 at program/function start for stack in bank 1

### Pointer Parameter Passing
When passing pointer parameters:
1. Pass address in first register
2. Pass bank tag in second register
3. Maintain this order consistently

### Pointer Return Values
Return pointers as two register values:
- **R3**: Pointer address
- **R4**: Bank tag
- This applies to all pointer-returning functions

## Memory Operations

### Load from Pointer
```asm
LOAD rd, bankReg, addrReg
```
Where:
- `rd`: Destination register
- `bankReg`: Register containing bank ID (R13 for stack, R0 for globals, or dynamic)
- `addrReg`: Register containing address

### Store to Pointer
```asm
STORE rs, bankReg, addrReg
```
Where:
- `rs`: Source register
- `bankReg`: Register containing bank ID (R13 for stack, R0 for globals, or dynamic)
- `addrReg`: Register containing address

### Instruction Memory Copies
```asm
LOADC R0, bankReg, addrReg
STORC R0, bankReg, addrReg
```
Copies one instruction (4 cells) between IMEM[bank][addr] and X0–X3. Bank and address are registers; the first operand is unused.

### Pointer Arithmetic (GEP)
- **CRITICAL**: Address arithmetic must respect bank boundaries
- Bank register must be preserved through arithmetic
- Formula: `addr' = addr + index * element_size`
- **Bank Overflow Handling**:
  - Option 1: Error on compile-time detectable overflow
  - Option 2: Wrap within bank (modulo bank_size)
  - Option 3: Runtime bounds checking
- **Recommendation**: For arrays spanning banks, use explicit bank calculation:
  ```asm
  ; For large arrays crossing banks:
  total_offset = base_addr + (index * element_size)
  new_bank = base_bank + (total_offset / bank_size)
  new_addr = total_offset % bank_size
  ```

## Spilling Strategy

### Spill Slot Allocation
- Spill slots start at FP+L
- Each spilled register gets a unique slot
- Slots are word-sized (1 cell)

### Spill Operation
```asm
ADD   R12, FP, R0       ; R12 is dedicated scratch for address calc
ADDI  R12, R12, (L+slot); Calculate spill address  
STORE reg, R13, R12     ; Store to stack (R13 = SB)
```

### Reload Operation
```asm
ADD   R12, FP, R0       ; R12 is dedicated scratch for address calc
ADDI  R12, R12, (L+slot); Calculate spill address
LOAD  reg, R13, R12     ; Load from stack (R13 = SB)
```

## Register Allocation Algorithm

### LRU-based Allocation
1. Maintain free list of available registers
2. Track LRU queue of in-use registers
3. When out of registers:
   - Select victim (least recently used)
   - Spill victim to stack
   - Reuse victim's register

### Expression Evaluation Order (Sethi-Ullman)
1. Calculate register need for each subexpression
2. Evaluate higher-need subexpression first
3. This minimizes total register pressure

## Addressing Modes

### Local Variable Access
```asm
ADD   r, FP, R0         ; Base = frame pointer
ADDI  r, r, offset      ; Add local offset
LOAD  result, SB, r     ; Load from stack bank
```

### Global Variable Access
```asm
LI    r, offset         ; Load global address
LOAD  result, R0, r     ; Load from global bank (R0 reads 0, globals in bank 0)
```

## Inter-procedural Considerations

### Parameter Areas
- Currently stack-based parameter passing
- Future: First N parameters in registers

### Variable Arguments
- Not yet supported
- Future: Passed on stack after fixed parameters

### Struct Returns
- **Small structs (≤1 word)**: In R3
- **Small structs (2 words)**: Low word in R3, high word in R4
- **Pointers**: Address in R3, bank in R4
- **Large structs (>2 words)**: Caller allocates, passes hidden pointer

## Optimizations

### Leaf Functions
- Functions that don't call others
- Can skip saving RA/RAB
- May use simplified prologue/epilogue

### Tail Calls
- Replace epilogue + call with:
```asm
JAL   bankImm, addrImm  ; Direct tail call
```

## Debugging Support

### Stack Walking
- FP forms linked list of frames
- Each frame has predictable layout
- Enables backtrace functionality

### Variable Location
- Locals: FP + known offset
- Spills: FP + L + spill_slot
- Temporaries: In registers or spilled

## Instruction Set Architecture

### Instruction Format
- **Instruction size**: 8 bytes (1 opcode byte + padding byte + 3x 16-bit words)
- **Formats**:
  - **R-format**: Register operations (opcode, rd, rs1, rs2)
  - **I-format**: Immediate operations (opcode, rd, rs/imm, imm)
  - **I1-format**: Special format for LI instruction

### Instruction Set with Opcodes

#### Arithmetic Instructions
- **ADD** (0x01): `rd = rs1 + rs2` - R-format
- **SUB** (0x02): `rd = rs1 - rs2` - R-format
- **MUL** (0x1A): `rd = rs1 * rs2` - R-format
- **DIV** (0x1B): `rd = rs1 / rs2` - R-format (signed)
- **MOD** (0x1C): `rd = rs1 % rs2` - R-format (signed)
- **ADDI** (0x0A): `rd = rs + imm` - I-format
- **MULI** (0x1D): `rd = rs * imm` - I-format
- **DIVI** (0x1E): `rd = rs / imm` - I-format (signed)
- **MODI** (0x1F): `rd = rs % imm` - I-format (signed)

#### Logical Instructions
- **AND** (0x03): `rd = rs1 & rs2` - R-format
- **OR** (0x04): `rd = rs1 | rs2` - R-format
- **XOR** (0x05): `rd = rs1 ^ rs2` - R-format
- **SLL** (0x06): `rd = rs1 << rs2` - R-format (shift left logical)
- **SRL** (0x07): `rd = rs1 >> rs2` - R-format (shift right logical)
- **SLT** (0x08): `rd = (rs1 < rs2) ? 1 : 0` - R-format (signed)
- **SLTU** (0x09): `rd = (rs1 < rs2) ? 1 : 0` - R-format (unsigned)
- **ANDI** (0x0B): `rd = rs & imm` - I-format
- **ORI** (0x0C): `rd = rs | imm` - I-format
- **XORI** (0x0D): `rd = rs ^ imm` - I-format
- **SLLI** (0x0F): `rd = rs << imm` - I-format
- **SRLI** (0x10): `rd = rs >> imm` - I-format

#### Memory Instructions
- **LI** (0x0E): `rd = imm` - I1-format (load immediate)
- **LOAD** (0x11): `rd = mem[bank][addr]` - I-format (data memory)
- **STORE** (0x12): `mem[bank][addr] = rs` - I-format (data memory)
- **LOADC** (0x20): `X0..X3 = IMEM[bank][addr]` - R-format (instruction memory; registers only)
- **STORC** (0x21): `IMEM[bank][addr] = X0..X3` - R-format (instruction memory; registers only)

#### Control Flow Instructions
- **JAL** (0x13): Jump and link (sets RA/RAB) - I-format
- **JALR** (0x14): Jump and link register - R-format
- **BEQ** (0x15): Branch if equal - I-format
- **BNE** (0x16): Branch if not equal - I-format
- **BLT** (0x17): Branch if less than (signed) - I-format
- **BGE** (0x18): Branch if greater or equal (signed) - I-format

#### Special Instructions
- **NOP** (0x00): No operation - R-format
- **BRK** (0x19): Breakpoint/debug - R-format
- **HALT**: Special encoding (NOP with all operands = 0)

## Bank Safety Considerations

### Bank Boundary Issues
- **Problem**: Simple address arithmetic can overflow bank boundaries
- **Example**: Array at bank[0]:4090 with 8-byte elements will overflow at index 1
- **Solutions**:
  1. **Static Analysis**: Compiler tracks maximum offsets and warns/errors
  2. **Bank-aware GEP**: Calculate bank crossings explicitly
  3. **Contiguous Virtual Addressing**: Abstract over banks in compiler

### Safe Array Access Pattern
For arrays that might span banks:
```asm
; Given: base_bank, base_addr, index, element_size
; Calculate absolute offset
MUL   R5, index, element_size
ADD   R5, R5, base_addr

; Calculate bank offset (assuming power-of-2 bank_size)
SRL   R6, R5, log2(bank_size)  ; bank_offset = total / bank_size
ADD   R6, R6, base_bank         ; new_bank = base_bank + bank_offset

; Calculate address within bank
ANDI  R5, R5, (bank_size - 1)  ; new_addr = total % bank_size

; Now safe to access
LOAD  result, R6, R5
```

### Compiler Strategies
1. **Small Objects**: Guarantee single-bank allocation
2. **Large Arrays**: Use virtual addressing with bank calculation
3. **Stack Arrays**: Limit size or use heap-like allocation
4. **String Literals**: Pack efficiently but track bank crossings
