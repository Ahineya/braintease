# V2 Backend Implementation Roadmap

## Executive Summary

This roadmap provides a complete guide for implementing the remaining components of the V2 backend. The V2 infrastructure (register management, function structure, calling convention) is complete and tested. This document outlines the steps to implement instruction lowering and integrate V2 into the main compilation pipeline.

**Latest Update (Phase 2 Complete!)**: All Phase 2 instructions are now fully implemented! Binary operations, comparison operations, and branch instructions are all working. The branch module supports both conditional and unconditional branches, with proper handling of comparison-based branching patterns. All branches use relative addressing (BEQ/BNE/BLT/BGE) as required by the VM. Unconditional branches are implemented using BEQ R0, R0, label. 211+ tests passing across all V2 modules.

## Current State

### ✅ Completed Components
- **Register Management** (`regmgmt/`) - Automatic R13 init, LRU spilling, bank tracking
- **Function Structure** (`function.rs`) - Prologue/epilogue with correct R13 initialization
- **Calling Convention** (`calling_convention.rs`) - Stack-based parameters, fat pointer returns
- **Parameter Type Tracking** - Correct handling of mixed scalar/fat pointer parameters
- **Callee-Saved Register Preservation** - S0-S3 properly saved/restored in prologue/epilogue
- **Unified Parameter Placement Logic** - Consistent behavior between caller and callee
- **Load Instruction** (`instr/load.rs`) - Loads from global/stack with proper bank handling
- **Store Instruction** (`instr/store.rs`) - Stores to global/stack with fat pointer support
- **GEP Instruction** (`instr/gep.rs`) - Full runtime bank overflow handling with DIV/MOD
- **Binary Operations** (`instr/binary/`) - All arithmetic, logical, and comparison operations with Sethi-Ullman ordering
- **Branch Instructions** (`instr/branch.rs`) - Conditional and unconditional branches with label support
- **Test Infrastructure** - 211+ passing tests across all V2 modules

### ❌ Missing Components
- Integration with main IR lowering pipeline

## Critical Development Practices

### Comprehensive Logging Requirements

**MANDATORY**: All new code MUST include comprehensive logging using the `log` crate:

1. **Use appropriate log levels**:
   - `trace!()` - Detailed execution flow, variable states, intermediate values
   - `debug!()` - Key decisions, register allocations, spills, important state changes
   - `info!()` - High-level operation summaries (reserved for major milestones)
   - `warn!()` - Recoverable issues, fallback behaviors
   - `error!()` - Unrecoverable errors before panicking

2. **What to log**:
   - **Entry/exit of major functions**: `debug!("lower_load: ptr={:?}, type={:?}", ptr, ty);`
   - **Register allocation decisions**: `debug!("Allocated {:?} for '{}'", reg, value);`
   - **Spill/reload operations**: `debug!("Spilling '{}' to slot {}", value, slot);`
   - **Bank calculations**: `trace!("GEP: base_bank={}, offset={}, new_bank={}", ...);`
   - **Instruction generation**: `trace!("Generated: {:?}", inst);`
   - **State before/after operations**: `trace!("Register state: {:?}", reg_contents);`

3. **Example logging pattern**:
```rust
pub fn lower_load(
    mgr: &mut RegisterPressureManager,
    ptr_value: &Value,
    result_type: &Type,
    result_name: String,
) -> Vec<AsmInst> {
    debug!("lower_load: ptr={:?}, type={:?}, result={}", ptr_value, result_type, result_name);
    trace!("  Current register state: {:?}", mgr.get_debug_state());
    
    let mut insts = vec![];
    
    // Get pointer components
    let addr_reg = match ptr_value {
        Value::Temp(t) => {
            let reg = mgr.get_register(format!("t{}", t.0));
            trace!("  Got address register {:?} for temp {}", reg, t.0);
            reg
        }
        // ...
    };
    
    // Get bank info
    let bank_info = mgr.get_pointer_bank(&ptr_value_name);
    debug!("  Pointer bank info: {:?}", bank_info);
    
    // Generate instruction
    let inst = AsmInst::Load { rd: dest_reg, bank: bank_reg, addr: addr_reg };
    trace!("  Generated LOAD: {:?}", inst);
    insts.push(inst);
    
    debug!("lower_load complete: generated {} instructions", insts.len());
    insts
}
```

4. **Running with debug output**:
```bash
# Enable trace logging for register management
RUST_LOG=rcc_ir::regmgmt=trace cargo test

# Enable debug logging for all V2 components
RUST_LOG=rcc_ir::v2=debug cargo build --release

# Full trace for debugging
RUST_LOG=trace cargo run -- compile test.c
```

5. **Benefits of comprehensive logging**:
   - **Debugging**: Quickly identify where things go wrong
   - **Understanding**: Trace execution flow during development
   - **Optimization**: Identify inefficiencies (excessive spills, etc.)
   - **Testing**: Verify correct behavior in tests
   - **Maintenance**: Future developers can understand decisions

**NOTE**: The V2 register management module now has comprehensive logging (added in this session). Use it as a reference for logging patterns.

## Phase 1: Memory Operations (Week 1) ✅ COMPLETE

### Task 1.1: Implement Load Instruction Lowering ✅ COMPLETED

**File created**: `rcc-ir/src/v2/instr/load.rs`

**Implementation completed**:
- ✅ Loads from global memory with GP register
- ✅ Loads from stack memory with SB register  
- ✅ Dynamic bank support for runtime-determined banks
- ✅ Fat pointer loading (both address and bank components)
- ✅ Proper integration with RegisterPressureManager
- ✅ Comprehensive unit and integration tests

**Original implementation steps**:
1. Create the load module structure:
```rust
use rcc_frontend::ir::{Value, Type};
use crate::regmgmt::{RegisterPressureManager, BankInfo};
use rcc_codegen::{AsmInst, Reg};
use log::{debug, trace};

pub fn lower_load(
    mgr: &mut RegisterPressureManager,
    ptr_value: &Value,
    result_type: &Type,
    result_name: String,
) -> Vec<AsmInst> {
    debug!("lower_load: ptr={:?}, type={:?}, result={}", ptr_value, result_type, result_name);
    let mut insts = vec![];
    
    // 1. Get pointer components (address and bank)
    let addr_reg = match ptr_value {
        Value::Temp(t) => {
            let reg = mgr.get_register(format!("t{}", t.0));
            trace!("  Got address register {:?} for temp {}", reg, t.0);
            reg
        }
        Value::Local(idx) => {
            // Load local pointer from stack
            // Address at FP+idx, bank at FP+idx+1
            debug!("  Loading local pointer {} from stack", idx);
            // Implementation here
            todo!("Load local pointer")
        }
        _ => {
            error!("Invalid pointer value for load: {:?}", ptr_value);
            panic!("Invalid pointer value for load")
        }
    };
    
    // 2. Get bank register based on pointer's bank info
    let bank_info = mgr.get_pointer_bank(&ptr_value_name);
    debug!("  Pointer bank info: {:?}", bank_info);
    let bank_reg = match bank_info {
        BankInfo::Global => {
            trace!("  Using Gp for global bank pointer");
            Reg::Gp   // Global bank pointer (Gp)
        }
        BankInfo::Stack => {
            trace!("  Using R13 for stack bank");
            Reg::Sb   // Stack bank (already initialized)
        }
        BankInfo::Register(r) => {
            trace!("  Using {:?} for dynamic bank", r);
            r    // Dynamic bank
        }
    };
    
    // 3. Allocate destination register
    let dest_reg = mgr.get_register(result_name.clone());
    
    // 4. Generate LOAD instruction
    let load_inst = AsmInst::Load {
        rd: dest_reg,
        bank: bank_reg,
        addr: addr_reg,
    };
    trace!("  Generated LOAD: {:?}", load_inst);
    insts.push(load_inst);
    
    // 5. If loading a fat pointer, also load the bank component
    if result_type.is_pointer() {
        debug!("  Loading fat pointer - need to load bank component");
        let bank_addr = /* calculate addr + 1 */;
        let bank_dest = mgr.get_register(format!("{}_bank", result_name));
        let bank_load = AsmInst::Load {
            rd: bank_dest,
            bank: bank_reg,
            addr: bank_addr,
        };
        trace!("  Generated bank LOAD: {:?}", bank_load);
        insts.push(bank_load);
        mgr.set_pointer_bank(result_name, BankInfo::Register(bank_dest));
        debug!("  Fat pointer loaded: addr in {:?}, bank in {:?}", dest_reg, bank_dest);
    }
    
    debug!("lower_load complete: generated {} instructions", insts.len());
    insts
}
```

**Testing**:
- Test loading from global memory (bank 0)
- Test loading from stack (bank 1 with R13)
- Test loading fat pointers (both components)
- Test with register pressure (spilling)

### Task 1.2: Implement Store Instruction Lowering ✅ COMPLETED

**File created**: `rcc-ir/src/v2/instr/store.rs`

**Implementation completed**:
- ✅ Stores to global memory with GP register
- ✅ Stores to stack memory with SB register
- ✅ Dynamic bank support for runtime-determined banks
- ✅ Fat pointer storing (both address and bank components)
- ✅ Immediate value handling (loading into registers first)
- ✅ Proper integration with RegisterPressureManager
- ✅ Comprehensive unit and integration tests

**Original implementation steps**:
1. Similar structure to load but reversed:
```rust
pub fn lower_store(
    mgr: &mut RegisterPressureManager,
    value: &Value,
    ptr_value: &Value,
) -> Vec<AsmInst> {
    // 1. Get value to store
    // 2. Get pointer components
    // 3. Get bank register
    // 4. Generate STORE instruction
    // 5. If storing fat pointer, store both components
}
```

**Critical considerations**:
- Ensure R13 is already initialized (done by RegisterPressureManager)
- Handle immediate values that need to be loaded into registers first
- Correctly identify bank from pointer provenance

### Task 1.3: Implement GEP with Bank Overflow Handling ✅ COMPLETED

**File created**: `rcc-ir/src/v2/instr/gep.rs`

**Implementation completed**:
- ✅ Full runtime bank overflow handling with DIV/MOD instructions
- ✅ Static offset optimization for compile-time known indices
- ✅ Power-of-2 element size optimization using shift instructions
- ✅ Support for Stack, Global, and dynamic bank pointers
- ✅ Proper fat pointer propagation through GEP operations
- ✅ 13 unit tests covering all GEP scenarios
- ✅ 10 integration tests with load/store operations
- ✅ Chained GEP support for multi-dimensional arrays

**Original implementation steps**:
```rust
use log::{debug, trace, warn};

pub fn lower_gep(
    mgr: &mut RegisterPressureManager,
    base_ptr: &Value,
    indices: &[Value],
    element_size: i16,
    result_name: String,
) -> Vec<AsmInst> {
    debug!("lower_gep: base={:?}, indices={:?}, elem_size={}, result={}", 
           base_ptr, indices, element_size, result_name);
    let mut insts = vec![];
    
    // 1. Get base pointer components
    let base_addr = /* get address register */;
    let base_bank_info = mgr.get_pointer_bank(&base_ptr_name);
    
    // 2. Calculate total offset
    // total_offset = sum(index[i] * stride[i])
    
    // 3. CRITICAL: Handle bank overflow
    // For static offsets:
    if let Some(static_offset) = try_get_static_offset(indices) {
        let total_offset = base_addr_value + static_offset;
        trace!("  Static offset: base={} + offset={} = total={}", 
               base_addr_value, static_offset, total_offset);
        
        if total_offset >= BANK_SIZE {
            // Calculate bank crossing
            let new_bank = base_bank + (total_offset / BANK_SIZE);
            let new_addr = total_offset % BANK_SIZE;
            warn!("  Bank overflow detected! Crossing from bank {} to {}", base_bank, new_bank);
            debug!("  New address: bank={}, offset={}", new_bank, new_addr);
            // Update bank info accordingly
        }
    } else {
        debug!("  Dynamic offset - generating runtime bank calculation");
        // Dynamic offset - need runtime calculation
        // Generate code to:
        // - Calculate total_offset = base + (index * element_size)
        // - new_bank = base_bank + (total_offset >> 12)  // div by 4096
        // - new_addr = total_offset & 0xFFF              // mod 4096
        trace!("  Will generate runtime bank overflow check");
    }
    
    // 4. Store result with updated bank info
    mgr.set_pointer_bank(result_name, new_bank_info);
    
    insts
}
```

**Bank overflow formula**:
```
BANK_SIZE = 4096 instructions = 16384 bytes
new_bank = base_bank + (total_offset / BANK_SIZE)
new_addr = total_offset % BANK_SIZE
```

**Testing requirements**:
- Test array access within single bank
- Test array access crossing bank boundary
- Test nested GEP operations
- Test with both static and dynamic indices

## Phase 2: Arithmetic & Control Flow (Week 2)

### Task 2.1: Complete Binary Operations ✅ COMPLETED

**Files created**: 
- `rcc-ir/src/v2/instr/binary/mod.rs` - Module structure
- `rcc-ir/src/v2/instr/binary/lowering.rs` - Main lowering logic with Sethi-Ullman ordering
- `rcc-ir/src/v2/instr/binary/arithmetic.rs` - Arithmetic operations (Add, Sub, Mul, Div, Mod, And, Or, Xor, Shift)
- `rcc-ir/src/v2/instr/binary/comparison.rs` - Comparison operations (Eq, Ne, Lt, Le, Gt, Ge, both signed and unsigned)
- `rcc-ir/src/v2/instr/binary/helpers.rs` - Helper functions for register allocation

**Implementation completed**:
- ✅ All arithmetic operations (Add, Sub, Mul, Div, Mod)
- ✅ All logical operations (And, Or, Xor)
- ✅ All shift operations (Shl, LShr, AShr)
- ✅ All comparison operations (Eq, Ne, Slt, Sle, Sgt, Sge, Ult, Ule, Ugt, Uge)
- ✅ Sethi-Ullman ordering for optimal register usage
- ✅ Immediate value optimizations (AddI, MulI, DivI, ModI)
- ✅ Comprehensive test suite (17 tests in `binary_tests.rs`)

### Task 2.2: Implement Comparison Operations ✅ COMPLETED

**Note**: Comparison operations were implemented as part of binary operations in `binary/comparison.rs`

All comparison predicates are fully implemented:
- Equality (Eq, Ne) using XOR and SLTU
- Signed comparisons (Slt, Sle, Sgt, Sge) using SLT
- Unsigned comparisons (Ult, Ule, Ugt, Uge) using SLTU

### Task 2.3: Implement Branch Instructions ✅ COMPLETED

**File created**: `rcc-ir/src/v2/instr/branch.rs`

**Implementation completed**:
- ✅ Unconditional branches using BEQ R0, R0, label
- ✅ Conditional branches with BEQ/BNE/BLT/BGE
- ✅ Comparison-based branching patterns
- ✅ Support for all comparison types (Eq, Ne, Lt, Le, Gt, Ge)
- ✅ Proper label generation and relative addressing
- ✅ 18 unit tests for branch patterns

**Key design decisions**:
- Use BEQ R0, R0, label for unconditional branches (always true since R0 == R0)
- All branches use relative addressing as required by the VM
- Inverse logic for GT and LE comparisons using swapped operands
- Labels are passed as strings to be resolved by the assembler

## Phase 3: Integration (Week 3)

### Task 3.1: Create V2 Main Lowering Function

**File to create**: `rcc-ir/src/v2/lower.rs`

```rust
use rcc_frontend::ir::{Instruction, Function};
use crate::regmgmt::RegisterPressureManager;
use crate::function::{emit_prologue, emit_epilogue};

pub fn lower_function_v2(func: &Function) -> Vec<AsmInst> {
    let mut insts = vec![];
    let mut mgr = RegisterPressureManager::new(func.locals.len() as i16);
    
    // 1. Initialize and emit prologue
    mgr.init();  // Initializes R13
    insts.extend(emit_prologue(&mut mgr, func.locals.len()));
    
    // 2. Process each basic block
    for block in &func.blocks {
        // Analyze block for lifetime info (optional optimization)
        mgr.analyze_block(block);
        
        // 3. Lower each instruction
        for inst in &block.instructions {
            let inst_asm = lower_instruction(&mut mgr, inst);
            insts.extend(mgr.take_instructions());
            insts.extend(inst_asm);
        }
    }
    
    // 4. Emit epilogue
    insts.extend(emit_epilogue(&mut mgr));
    
    insts
}

fn lower_instruction(
    mgr: &mut RegisterPressureManager,
    inst: &Instruction,
) -> Vec<AsmInst> {
    match inst {
        Instruction::Load { .. } => lower_load(mgr, ...),
        Instruction::Store { .. } => lower_store(mgr, ...),
        Instruction::GetElementPtr { .. } => lower_gep(mgr, ...),
        Instruction::BinaryOp { .. } => mgr.emit_binary_op(...),
        Instruction::Icmp { .. } => lower_icmp(mgr, ...),
        Instruction::Call { .. } => lower_call(mgr, ...),
        Instruction::Ret { .. } => lower_return(mgr, ...),
        Instruction::Br { .. } => lower_branch(mgr, ...),
        Instruction::Alloca { .. } => lower_alloca(mgr, ...),
        _ => vec![],
    }
}
```

### Task 3.2: Switch Main Pipeline to V2

**File to modify**: `rcc-ir/src/lower/mod.rs`

```rust
// Add feature flag or configuration
pub fn lower_module(module: &Module, use_v2: bool) -> Vec<AsmInst> {
    if use_v2 {
        // Use V2 backend
        v2::lower::lower_module_v2(module)
    } else {
        // Keep V1 for comparison/fallback
        lower_module_v1(module)
    }
}
```

## Phase 4: Testing & Validation (Week 4)

### Task 4.1: Unit Tests for Each Instruction Type

Create test files:
- `rcc-ir/src/v2/tests/load_store_tests.rs`
- `rcc-ir/src/v2/tests/gep_tests.rs`
- `rcc-ir/src/v2/tests/binary_ops_tests.rs`
- `rcc-ir/src/v2/tests/control_flow_tests.rs`

### Task 4.2: Integration Tests

**Test progression**:
1. Start with simplest tests from `c-test/tests/`:
   - `test_return_42.c` - Just return a constant
   - `test_add.c` - Simple arithmetic
   - `test_local_var.c` - Local variable access

2. Progress to more complex:
   - `test_array.c` - Array access (tests GEP)
   - `test_pointer.c` - Pointer operations
   - `test_function_call.c` - Function calls

3. Run full test suite:
```bash
python3 c-test/run_tests.py --verbose
```

### Task 4.3: Performance Comparison

Compare V1 vs V2:
- Register usage efficiency
- Number of spills
- Code size
- Execution speed in RVM

## Implementation Checklist

### Week 0: Preparation
- [x] Migrate project to 32 registers architecture, described in `/docs/arch/registers.md`
  - [x] Register allocations updated (A0-A3 for args, RV0-RV1 for returns, S0-S3 callee-saved)
  - [x] Calling convention updated to use A0-A3 for first 4 arguments
  - [x] Parameter type tracking for correct fat pointer handling
  - [x] Callee-saved registers (S0-S3) properly preserved
  - [x] Comprehensive tests for all register combinations

### Week 1: Memory Operations
- [x] Implement Load instruction
- [x] Implement Store instruction  
- [x] Implement GEP with bank overflow
- [x] Unit tests for memory operations (load/store/GEP)
- [x] Verify R13 initialization works

### Week 2: Arithmetic & Control
- [x] Complete binary operations
- [x] Implement comparison operations
- [x] Implement branch instructions
- [x] Unit tests for binary, comparison, and branch operations

### Week 3: Integration
- [ ] Create main V2 lowering function
- [ ] Connect all instruction types
- [ ] Add V2/V1 switch mechanism
- [ ] Basic integration tests pass

### Week 4: Validation
- [ ] All unit tests pass
- [ ] Simple C programs compile
- [ ] Complex C programs compile
- [ ] Full test suite passes
- [ ] Performance metrics collected

## Critical Success Factors

### Must Have
1. **R13 always initialized** - RegisterPressureManager handles this
2. **Correct bank registers** - R0 for global, R13 for stack
3. **Bank overflow handling** - GEP must handle crossing banks
4. **Fat pointer support** - Load/store both address and bank
5. **Stack-based parameters** - No parameters in R3-R4

### Should Have
1. **Sethi-Ullman ordering** - Already in RegisterPressureManager
2. **LRU spilling** - Already implemented
3. **Clean error handling** - Proper error messages
4. **Debug information** - Source location tracking

### Nice to Have
1. **Optimization passes** - Dead code elimination
2. **Advanced register allocation** - Graph coloring
3. **Peephole optimization** - Pattern-based improvements

## Common Pitfalls to Avoid

### ❌ DON'T
- Don't forget to use RegisterPressureManager.init() 
- Don't manually manage R13 - let the manager handle it
- Don't use R3-R4 for parameters
- Don't ignore bank overflow in GEP
- Don't forget to test fat pointer operations

### ✅ DO
- Always use the RegisterPressureManager API
- Test with both small and large offsets
- Verify bank calculations with concrete examples
- Run tests after each component implementation
- Keep V1 as fallback during development

## Debugging Guide

### Using Logging Effectively

1. **Enable logging during development**:
```bash
# See all V2 decisions
RUST_LOG=rcc_ir::v2=debug cargo test

# Trace register allocation
RUST_LOG=rcc_ir::regmgmt=trace cargo test specific_test

# Debug specific module
RUST_LOG=rcc_ir::instr::load=trace cargo run
```

2. **Add temporary trace points**:
```rust
// When debugging a specific issue
trace!("=== DEBUGGING: Before spill, registers: {:?} ===", self.reg_contents);
// Fix the issue
// Remove or downgrade to trace! once fixed
```

3. **Use structured logging**:
```rust
// Good: Structured, searchable
debug!("GEP calculation: base_bank={}, offset={}, new_bank={}, new_addr={}", 
       base_bank, offset, new_bank, new_addr);

// Bad: Unstructured
debug!("Doing GEP stuff");
```

### When things go wrong:

1. **Check R13 initialization**:
   - Look for `LI R13, 1` in generated assembly
   - Must appear before ANY stack operation

2. **Verify bank registers**:
   - Global access should use R0
   - Stack access should use R13
   - Check with `rvm --verbose` to see actual banks used

3. **Trace pointer operations**:
   - Fat pointers need TWO registers (addr + bank)
   - GEP must update BOTH components
   - Check bank overflow calculations

4. **Debug with small examples**:
   ```c
   // Minimal test case
   int main() {
       int x = 42;
       return x;
   }
   ```
   
5. **Use debug flags**:
   ```bash
   rcc compile test.c --debug 3
   RUST_LOG=trace cargo run
   ```

## Resources

### Documentation
- `/docs/arch/` - Current ISA, registers, ABI, memory, MMIO
- `/docs/archive/` - Deprecated specs (18-reg ABI, draft ISA, old roadmaps)
- `/TOOLCHAIN.md` - Build and run
- `/rcc-backend/src/README_ARCHITECTURE.md` - Backend module map

### Reference Implementation
- V1 implementations in `/rcc-ir/src/lower/instr/` (fix the bugs!)
- Test cases in `/c-test/tests/`
- ISA examples in `/docs/arch/isa.md`

### Tools
- `rvm --verbose` - Debug VM execution
- `rasm disassemble` - Verify generated assembly
- `python3 c-test/run_tests.py` - Run test suite

## Success Criteria

The V2 backend is complete when:
1. ✅ All tests pass
2. ✅ All instruction types are implemented
3. ✅ Simple C programs compile and run correctly
4. ✅ Full test suite passes (python3 c-test/run_tests.py)
5. ✅ No regressions from V1 functionality
6. ✅ Bank overflow is handled correctly

## Contact & Support

- Check test files for examples: `/rcc-ir/src/v2/tests/`
- Refer to specifications in `/docs/arch/`, `/src/ripple-asm/src/types.rs`, and `./README_ARCHITECTURE.md`
- Run tests frequently to catch issues early
- Remember: R13 MUST BE 1 for stack operations!

---

**Estimated Timeline**: 4 weeks for full implementation
**Complexity**: Medium (infrastructure is done, just need instruction lowering)
**Risk**: Low (V2 infrastructure is well-tested and correct)