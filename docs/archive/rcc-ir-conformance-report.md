> **Deprecated (historical).** This file is not a current specification.
> Current architecture lives in [`docs/arch/`](../arch/README.md).
> See [`docs/archive/README.md`](README.md) for why this document was archived.

---

# RCC-IR Conformance Report

## Executive Summary

This report analyzes the conformance of the RCC-IR compiler implementation to the Ripple VM Calling Convention specification. The analysis covers register allocation, function handling, pointer representation, memory operations, and calling conventions.

## Conformance Analysis

### ⚠️ Register Allocation (simple_regalloc.rs)

**Status: MOSTLY CONFORMANT WITH ISSUES**

**Conformant aspects:**
- Implementation correctly uses R5-R11 as allocatable pool (lines 46, 290-294, 391-394, 430)
- Uses R12 as scratch register for spill address calculation (correct)
- R14 (SP) and R15 (FP) are properly excluded from allocation
- Implements spilling mechanism with LRU-like selection
- Correctly preserves parameter registers across statement boundaries

**Critical Issues:**
- **R13 NEVER INITIALIZED**: The code uses R13 as stack bank register but NEVER sets it to 1!
  - All spill/reload operations use uninitialized R13
  - This would cause all stack operations to fail
- **Misleading comment**: Line 12 says "R3-R11" but implementation correctly uses only R5-R11
- **Not true LRU**: Uses first non-pinned register, not actual least-recently-used tracking

### ❌ Function Prologue/Epilogue (lower/functions.rs)

**Status: NON-CONFORMANT**

**Critical Issue:**
- **R13 (stack bank) is NEVER initialized to 1**
- Prologue/epilogue use R13 as bank register but it contains garbage
- ALL stack operations would fail

**Structure matches spec but broken due to uninitialized R13:**

**Prologue** (lines 192-216):
```asm
; MISSING: LI R13, 1    ; Initialize stack bank!!!
STORE RA, R13, R14    ; Save RA at SP (R13 uninitialized!)
ADDI R14, R14, 1      ; Increment SP
STORE R15, R13, R14   ; Save old FP (R13 uninitialized!)
ADDI R14, R14, 1      ; Increment SP
ADD R15, R14, R0      ; FP = SP
ADDI R14, R14, space  ; Reserve stack space
```

**Epilogue** (lines 219-230):
```asm
ADD R14, R15, R0      ; SP = FP (restore)
ADDI R14, R14, -1     ; Decrement SP
LOAD R15, R13, R14    ; Restore old FP (R13 uninitialized!)
ADDI R14, R14, -1     ; Decrement SP
LOAD RA, R13, R14     ; Restore RA (R13 uninitialized!)
```

### ⚠️ Pointer Handling (Fat Pointers)

**Status: PARTIALLY CONFORMANT**

**Conformant aspects:**
- Fat pointer structure with address and bank components (FatPtrComponents struct)
- Bank tag values: 0 for Global, 1 for Stack (matches spec)
- Pointer parameters passed as two values (address, bank)
- Pointer returns use R3 (address) and R4 (bank)
- GEP operations preserve bank tags

**Non-conformant/Missing aspects:**
- Bank tag is tracked separately via `bank_temp_key` rather than being fully integrated
- No explicit handling for bank overflow in GEP operations (critical safety issue)
- Mixed pointer provenance not fully handled (would error rather than work)

### ❌ Memory Operations (LOAD/STORE)

**Status: NON-CONFORMANT**

**Critical Issues:**
- **WRONG BANK REGISTER USAGE**: 
  - `get_bank_for_pointer` returns R0 for global (correct) but a register with value `1` for stack
  - Should return R13 (stack bank register) for stack, not a register containing `1`
  - Inconsistent usage: Sometimes uses R13 directly, sometimes uses value from `get_bank_for_pointer`

**Load operations** (lower/instr/load.rs):
- Uses `LOAD rd, bank_reg, addr_reg` format (correct structure)
- Handles fat pointer loads (loading both address and bank)
- **BROKEN**: Bank register is wrong for stack operations

**Store operations** (lower/instr/store.rs):
- Uses `STORE rs, bank_reg, addr_reg` format (correct structure)
- Handles fat pointer stores (storing both address and bank)
- **BROKEN**: Bank register is wrong for stack operations
- Inconsistent: func.rs:212 uses R13 directly, but store.rs uses `get_bank_for_pointer`

### ❌ Calling Convention

**Status: NON-CONFORMANT**

**Critical Non-conformance:**
- **WRONG REGISTER USAGE**: Implementation uses R3-R8 for parameters, but R3 is reserved for return values!
- According to spec: R3 = return value, R4 = second return/pointer bank
- Parameters should likely use R5-R11 or a different convention
- This is a **fundamental ABI mismatch** that would break interoperability

**Conformant aspects:**
- Fat pointers passed as two consecutive values (for parameters)
- Proper argument shuffling to avoid conflicts (lines 143-199)
- All registers considered caller-saved

**Additional Non-conformance:**
- **BROKEN POINTER RETURNS**: Return only puts address in R3, NOT bank in R4!
- Pointer returns are completely broken - missing bank information

**Other issues:**
- No callee-saved registers (spec says "none" but could be clearer)
- Stack parameters at incorrect offsets (should account for saved RA/FP)
- No support for varargs (spec says "not yet supported")

### ✅ Register Numbering

**Status: CONFORMANT**

The implementation uses the correct hardware register numbers as defined in the specification. The Reg enum in the codebase matches the numbering in types.rs.

## Critical Issues

### 1. Completely Broken Calling Convention ❌❌❌
**SEVERE ABI VIOLATIONS**:

**A. Wrong Parameter Registers**: 
- Implementation uses R3-R8 for parameters
- But R3-R4 are reserved for return values!
- Parameters should use R5-R11 (or different convention)

**B. Broken Pointer Returns**:
- Implementation only returns address in R3
- **Completely missing bank tag in R4 for pointer returns**
- This means returned pointers are unusable!

**Impact**: 
- Functions cannot correctly return pointers
- Parameter passing conflicts with return values
- Total ABI incompatibility

**Fix Required**: 
- Implement proper fat pointer returns (R3=addr, R4=bank)
- Move parameters to R5-R11 or revise convention
- This is blocking any real use of the compiler

### 2. R13 Stack Bank Register NEVER INITIALIZED ❌❌❌
**CATASTROPHIC**: R13 is supposed to be the stack bank register but is NEVER set to 1:
- ALL prologue/epilogue operations use uninitialized R13
- ALL spill/reload operations use uninitialized R13  
- ALL stack memory access would fail with garbage bank value
- The entire stack mechanism is completely broken

### 3. Wrong Bank Register Usage ❌
**SEVERE**: Memory operations use wrong bank registers:
- `get_bank_for_pointer` loads value `1` into a register for stack bank
- Should return R13 (the stack bank register) directly
- Inconsistent: Sometimes uses R13, sometimes uses `get_bank_for_pointer` result

### 4. Bank Overflow in GEP ❌
The implementation does not handle bank boundary crossing in pointer arithmetic. This could lead to memory corruption when arrays span banks.

**Recommendation**: Implement bank-aware GEP as specified in the "Bank Safety Considerations" section.

### 5. Mixed Pointer Provenance ⚠️
The current implementation would error on mixed pointer provenance rather than handling it at runtime.

**Recommendation**: Implement runtime bank tracking for mixed provenance cases.

## Minor Deviations

### 1. Bank Tag Storage
Bank tags are stored separately using `bank_temp_key` rather than being fully integrated into the value system.

**Impact**: Low - functionality is correct but code is more complex.

### 2. Spill Slot Management
Spill slots are managed correctly but the offset calculation could be clearer about the relationship to the frame layout.

**Impact**: Low - works correctly but could be better documented.

## Recommendations

### High Priority
1. **Implement bank-aware GEP**: Add overflow checking and proper bank calculation for arrays spanning banks
2. **Document bank tag flow**: Add clear documentation about how bank tags flow through the compiler
3. **Add safety assertions**: Add runtime checks for bank boundary violations in debug builds

### Medium Priority
1. **Unify pointer representation**: Consider fully integrating fat pointers into the value system
2. **Improve spill slot documentation**: Document the exact frame layout including spill slots
3. **Add mixed provenance support**: Implement runtime bank selection for mixed provenance

### Low Priority
1. **Optimize register allocation**: Consider implementing callee-saved registers for better performance
2. **Add varargs support**: Implement variable argument lists when needed
3. **Improve parameter passing**: Consider register-based parameter passing for first N arguments

## Conclusion

The RCC-IR implementation has **significant non-conformance** issues with the Ripple VM Calling Convention, most critically in the parameter passing mechanism. While some core mechanics like register allocation and memory operations are correct, the fundamental ABI violation makes this implementation incompatible with the specification.

Main issues requiring immediate attention:

1. **Completely broken calling convention** (CRITICAL - ABI BREAKING)
2. **R13 never initialized - ALL stack access broken** (CATASTROPHIC)
3. **Wrong bank register usage** (CRITICAL - MEMORY ACCESS BROKEN)
4. **Bank boundary safety in pointer arithmetic** (CRITICAL - MEMORY SAFETY)
5. **Mixed pointer provenance handling** (IMPORTANT)

Overall conformance score: **20%**

The implementation cannot be considered conformant until the parameter passing ABI is fixed. This is not just a safety issue but a fundamental compatibility problem that would prevent interoperability with correctly implemented code.