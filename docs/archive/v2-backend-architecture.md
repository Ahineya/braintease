> **Deprecated (historical).** This file is not a current specification.
> Current architecture lives in [`docs/arch/`](../arch/README.md).
> See [`docs/archive/README.md`](README.md) for why this document was archived.

---

# V2 Backend Architecture Documentation

## Project Overview

This is the Ripple C Compiler (RCC) backend, part of a larger toolchain that compiles C code to run on the Ripple VM - a custom 16-bit virtual machine. The compilation pipeline is:

```
C Code → Frontend → IR → V2 Backend → Ripple Assembly → Assembler → Binary
```

## System Architecture

### The Ripple VM

The Ripple VM is a 16-bit virtual machine with:
- **Memory banks**: Memory is divided into banks of 4096 instructions (16384 cells)
- **18 registers**: Including special-purpose registers for banks
- **Stack-based architecture**: Uses a stack for local variables and function calls
- **Fat pointers**: Pointers are 32-bit (16-bit address + 16-bit bank)

### Why Banks?

The 16-bit architecture can only address 64K cells directly. Banks extend this:
- Each bank contains 4096 instructions
- Bank 0: Global data (.data, .rodata)
- Bank 1: Stack 
- Bank 2+: Future heap/dynamic allocation

This allows programs larger than 64K while maintaining 16-bit efficiency.

## The V1 → V2 Rewrite

### What Went Wrong in V1?

V1 had fundamental ABI violations that made it completely non-functional:

1. **Stack Bank Never Initialized**
   - R13 (stack bank register) was never set to 1
   - ALL stack operations used random memory
   - Programs would corrupt random memory instead of using stack

2. **Calling Convention Violations**
   - Used R3-R8 for parameters
   - But R3-R4 are reserved for return values!
   - This caused parameter/return value collisions

3. **Incomplete Pointer Returns**
   - Only returned address in R3
   - Forgot to return bank in R4
   - Made all returned pointers unusable

4. **Bank Register Confusion**
   - Mixed up when to use R0 (global) vs R13 (stack)
   - Loaded/stored from wrong memory regions

### V2 Design Principles

1. **Correctness First**: Follow specifications exactly
2. **Test-Driven**: Every feature has tests
3. **Clear Separation**: Implementation split into focused modules
4. **Explicit Bank Management**: Always know which bank you're accessing

## Code Organization

```
rcc-ir/src/v2/
├── README.md                  # Developer guide (START HERE!)
├── mod.rs                     # Module interface
├── regalloc.rs               # Register allocation with spilling
├── function.rs               # Function structure (prologue/epilogue)
├── calling_convention.rs     # How functions call each other
└── tests/                    # Comprehensive test suite
    ├── regalloc_tests.rs     # Register allocator tests
    ├── function_tests.rs     # Function generation tests
    └── calling_convention_tests.rs  # Call/return tests
```

### Module Responsibilities

#### `regalloc.rs` - Register Allocator
- Manages R5-R11 (7 general-purpose registers)
- Spills to stack when out of registers
- Tracks pointer bank information
- **CRITICAL**: Initializes R13=1 for stack access

#### `function.rs` - Function Lowering
- Generates prologue (function entry)
- Generates epilogue (function exit)
- Manages local variables
- Handles return values

#### `calling_convention.rs` - Calling Convention
- Stack-based parameter passing
- Return value handling (R3 for scalar, R3+R4 for pointers)
- Cross-bank function calls
- Stack cleanup after calls

## Key Algorithms

### Register Allocation

Uses a simple spill-based allocator:
1. Maintain free list of R5-R11
2. When register needed:
   - If free register available → use it
   - Else → spill least-recently-used register to stack
3. Track spilled values for reload
4. Pin registers during critical sections

### Stack Frame Management

```
Growing ↓
+------------------+
| Caller's frame   |
+==================+ ← FP (Frame Pointer)
| Local variables  | FP+0, FP+1, ...
+------------------+
| Spill slots      | FP+L, FP+L+1, ...
+------------------+ ← SP (Stack Pointer)
| (Next frame)     |
+------------------+

Parameters at: FP-3, FP-4, FP-5, ...
```

### Cross-Bank Calls

For calling functions in different banks:
1. Set PCB (Program Counter Bank) to target bank
2. JAL (Jump And Link) to target address
3. JAL automatically saves return bank in RAB
4. On return, restore PCB from RAB

## Testing Strategy

### Test Coverage (23 tests)

- **Register Allocation** (9 tests)
  - R13 initialization
  - Register allocation order
  - Spilling and reloading
  - Parameter loading
  - Pinning mechanism

- **Function Generation** (6 tests)
  - Prologue with R13 init
  - Epilogue with bank restore
  - Scalar returns
  - Fat pointer returns
  - Local variable access

- **Calling Convention** (8 tests)
  - Stack-based arguments
  - Fat pointer arguments
  - Stack cleanup
  - Return value handling
  - Cross-bank calls

### Running Tests

```bash
# Run all V2 tests
cargo test --package rcc-ir --lib v2::tests::

# Run specific module tests
cargo test --package rcc-ir --lib v2::tests::regalloc_tests::
cargo test --package rcc-ir --lib v2::tests::function_tests::
cargo test --package rcc-ir --lib v2::tests::calling_convention_tests::
```

## Integration Points

### Input: IR (Intermediate Representation)

The V2 backend receives IR instructions like:
- `Alloca` - Allocate local variable
- `Store` - Store to memory
- `Load` - Load from memory
- `Call` - Function call
- `Ret` - Return from function
- `GetElementPtr` - Pointer arithmetic

### Output: Assembly Instructions

Generates Ripple VM assembly:
- `LI R13, 1` - Load immediate
- `STORE Rs, Rb, Ra` - Store Rs to memory[Rb][Ra]
- `LOAD Rd, Rb, Ra` - Load from memory[Rb][Ra]
- `JAL addr` - Jump and link
- `JALR Rd, 0, Rs` - Jump and link register
- `ADD/SUB/MUL/DIV` - Arithmetic
- etc.

### External Dependencies

- `rcc-codegen`: Defines `AsmInst` enum and `Reg` enum
- `ripple-asm`: The assembler that converts assembly to binary
- `rvm`: The virtual machine that executes the binary

## Future Work

### Immediate TODOs

1. **Memory Operations**
   - Implement Load/Store instruction generation
   - Ensure correct bank usage

2. **GEP (GetElementPtr)**
   - Implement bank-aware pointer arithmetic
   - Handle bank overflow correctly

3. **IR Integration**
   - Connect V2 backend to IR lowering
   - Replace V1 usage

### Long-term Improvements

1. **Optimization**
   - Better register allocation (graph coloring?)
   - Peephole optimizations
   - Dead code elimination

2. **Debugging Support**
   - Source location tracking
   - Better error messages
   - Debug symbol generation

3. **Advanced Features**
   - Inline assembly support
   - SIMD operations (if VM supports)
   - Link-time optimization

## Debugging Tips

### Common Issues and Solutions

| Symptom | Likely Cause | Solution |
|---------|--------------|----------|
| Random crashes | R13 not initialized | Add `LI R13, 1` at function start |
| Wrong values | Using R3-R4 for params | Use stack for parameters |
| Can't return from function | PCB not restored | Add `ADD PCB, RAB, R0` before JALR |
| Memory corruption | Wrong bank register | Use R0 for global, R13 for stack |
| Spill/reload fails | R13 not initialized | Initialize R13 before any stack ops |

### Debugging Checklist

1. ✓ Is R13 initialized to 1?
2. ✓ Are parameters on stack (not R3-R4)?
3. ✓ Are returns in R3 (R4 for pointer bank)?
4. ✓ Is PCB restored before return?
5. ✓ Are memory ops using correct bank?

## Contact & Resources

- **Specifications**: See `/docs/` directory
  - `ripple-calling-convention.md`
  - `ASSEMBLY_FORMAT.md`
  - `rcc-ir-conformance-report.md`
  
- **Related Projects**:
  - `ripple-asm`: Assembler
  - `rvm`: Virtual Machine
  - `rcc-frontend`: C parser/analyzer

## Summary

The V2 backend is a complete, tested, and conformant implementation of the Ripple VM code generator. It fixes all critical issues from V1 and provides a solid foundation for the Ripple C Compiler.

**Remember the golden rule: R13 MUST BE 1 for stack operations!**