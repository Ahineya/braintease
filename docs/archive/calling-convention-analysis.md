> **Deprecated (historical).** This file is not a current specification.
> Current architecture lives in [`docs/arch/`](../arch/README.md).
> See [`docs/archive/README.md`](README.md) for why this document was archived.

---

# Calling Convention Analysis and Standardization Plan

## Executive Summary

The current calling convention implementation in `rcc-backend/src/v2/function/calling_convention.rs` contains multiple functions with overlapping responsibilities and inconsistent usage patterns. This document analyzes the current state and proposes a standardization plan to ensure a single, consistent calling convention throughout the compiler.

## Current State Analysis

### Core Functions in calling_convention.rs

1. **`analyze_placement`** (private, lines 30-63)
   - Core logic for determining register vs stack placement
   - Takes a closure to identify fat pointers
   - Returns `(register_items_with_slots, first_stack_item_index)`
   - **Good**: Reusable core logic

2. **`analyze_arg_placement`** (private, lines 66-68)
   - Wrapper around `analyze_placement` for CallArg types
   - Used by `setup_call_args`

3. **`analyze_param_placement`** (private, lines 71-74)
   - Wrapper around `analyze_placement` for IrType parameters
   - Used by `load_param`

4. **`setup_call_args`** (public, lines 80-192)
   - Sets up arguments before a function call
   - Handles both register (A0-A3) and stack arguments
   - Returns instructions but does NOT spill registers
   - **Issue**: Caller must handle spilling before calling this

5. **`emit_call`** (public, lines 195-221)
   - Generates the actual JAL instruction
   - Handles cross-bank calls by setting PCB
   - Simple and focused

6. **`handle_return_value`** (public, lines 225-260)
   - Binds return value to Rv0/Rv1 registers
   - Updates register manager with result name
   - Handles both scalar and fat pointer returns

7. **`cleanup_stack`** (public, lines 263-274)
   - Adjusts SP after call to remove arguments
   - Simple SP adjustment

8. **`make_complete_call`** (public, lines 278-335)
   - Combines setup_call_args + emit_call + handle_return_value + cleanup_stack
   - Takes numeric function address
   - **Good**: All-in-one solution

9. **`make_complete_call_by_label`** (public, lines 339-394)
   - Same as `make_complete_call` but uses label instead of address
   - **Duplication**: 90% identical to `make_complete_call`

10. **`load_param`** (public, lines 401-526)
    - Loads parameters in callee function
    - Handles both register and stack parameters
    - Manages spilling via RegisterPressureManager
    - **Complex**: Lots of offset calculations

## Usage Patterns

### Main Usage Points

1. **instruction.rs (lines 212-226)**:
   ```rust
   let cc = CallingConvention::new();
   let (call_insts, _return_regs) = cc.make_complete_call_by_label(
       mgr, naming, &func_name, call_args,
       result_type.is_pointer(), result_name,
   );
   ```
   - Uses the complete call sequence with label

2. **function.rs (lines 97-112)**:
   ```rust
   let cc = CallingConvention::new();
   for (idx, (param_id, _ty)) in function.parameters.iter().enumerate() {
       let (param_insts, preg, bank_reg) = cc.load_param(idx, &pt, mgr, naming);
       // ... binding logic
   }
   ```
   - Uses load_param for function prologue

3. **FunctionBuilder (builder.rs)**:
   - Uses individual functions: `emit_call`, `cleanup_stack`
   - Does NOT use `make_complete_call`
   - Manages its own call sequence

## Problems Identified

### 1. Duplication
- `make_complete_call` and `make_complete_call_by_label` are nearly identical
- Both calculate stack words the same way
- Both follow the same 4-step sequence

### 2. Inconsistent Interfaces
- Some code uses `make_complete_call_by_label` (instruction.rs)
- Other code builds calls manually using individual functions (FunctionBuilder)
- No single "right way" to make a call

### 3. Hidden Complexity
- `load_param` does register allocation internally
- Stack offset calculations are duplicated between setup and load
- Fat pointer handling is spread across multiple functions

### 4. Spilling Responsibility
- `setup_call_args` explicitly does NOT spill registers
- Caller must remember to spill before calling
- Easy source of bugs

### 5. Register Assignment Assumptions
- Hardcoded A0-A3 for arguments
- Hardcoded Rv0-Rv1 for returns
- Mixed use of symbolic names

## Proposed Standardization Plan

### Phase 1: Consolidate Duplicate Functions

**Action 1.1**: Merge `make_complete_call` and `make_complete_call_by_label`
```rust
pub enum CallTarget {
    Address { addr: u16, bank: u16 },
    Label(String),
}

pub fn make_complete_call(
    &self,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    target: CallTarget,
    args: Vec<CallArg>,
    returns_pointer: bool,
    result_name: Option<String>,
) -> (Vec<AsmInst>, Option<(Reg, Option<Reg>)>)
```

### Phase 2: Establish Clear Calling Convention Rules

**Document and enforce**:
1. First 4 scalar args OR 2 fat pointers in A0-A3
2. Mixed args: scalars and fat pointers share A0-A3 space, if the fat pointer does not fit, spill it IN FULL to the stack.
3. Remaining args on stack in reverse order
4. Return: Rv0 for scalar/address, Rv1 for bank
5. Callee-saved: S0-S3 must be preserved
6. Caller-saved: T0-T7, A0-A3 can be clobbered

### Phase 3: Create Single Entry Points

**Action 3.1**: For making calls, enforce using `make_complete_call`
- Remove public access to individual functions
- Make `setup_call_args`, `emit_call`, `cleanup_stack` private
- Keep `handle_return_value` public for special cases only

**Action 3.2**: For loading parameters, keep `load_param` as the single entry point

### Phase 4: Fix Spilling Responsibility

**Action 4.1**: Make `setup_call_args` handle spilling
- Before setting up arguments, spill all live registers
- Document this behavior clearly
- Remove the warning comment

### Phase 5: Centralize Offset Calculations

**Action 5.1**: Create shared helper for stack parameter offsets
```rust
fn calculate_stack_param_offset(
    param_index: usize,
    param_types: &[(TempId, IrType)],
    first_stack_param: usize,
) -> i16
```

### Phase 6: Improve Testing

**Action 6.1**: Add integration tests that verify:
- Calls with 0-10 arguments work correctly
- Mixed scalar/fat pointer arguments
- Stack cleanup is correct
- Return values are properly bound

## Implementation Priority

1. **Immediate** (Week 1):
   - Merge duplicate `make_complete_call` functions
   - Fix spilling in `setup_call_args`
   - Add helper for stack offset calculations

2. **Short-term** (Week 2):
   - Make individual call functions private
   - Update all call sites to use unified interface
   - Add comprehensive tests

3. **Medium-term** (Week 3):
   - Document calling convention formally
   - Add debug assertions for convention violations
   - Profile and optimize hot paths

## Migration Path

### Step 1: Update FunctionBuilder
```rust
// Instead of:
builder.begin_call(args)
    .emit_call(addr, bank)
    .end_call(stack_words);

// Use:
let cc = CallingConvention::new();
let (insts, regs) = cc.make_complete_call(
    mgr, naming,
    CallTarget::Address { addr, bank },
    args, returns_pointer, result_name
);
builder.add_instructions(insts);
```

### Step 2: Update all test files
- Replace manual call sequences with `make_complete_call`
- Verify tests still pass

### Step 3: Make functions private
- Change visibility of internal functions
- Ensure no external dependencies

## Success Metrics

1. **Single source of truth**: All calls go through `make_complete_call`
2. **No duplication**: No repeated logic for stack calculations
3. **Clear ownership**: Spilling handled consistently
4. **Testable**: Each aspect of calling convention has tests
5. **Documented**: Clear specification of the convention

## Risks and Mitigations

### Risk 1: Breaking existing code
**Mitigation**: Gradual migration with compatibility layer

### Risk 2: Performance regression
**Mitigation**: Benchmark before/after changes

### Risk 3: Hidden dependencies
**Mitigation**: Comprehensive test suite before changes

## Conclusion

The current calling convention implementation works but has significant technical debt. By consolidating duplicate functions, establishing clear interfaces, and centralizing complexity, we can create a more maintainable and reliable calling convention that serves as a solid foundation for the compiler's code generation.

The proposed changes are backwards-compatible and can be implemented incrementally, minimizing risk while improving code quality.