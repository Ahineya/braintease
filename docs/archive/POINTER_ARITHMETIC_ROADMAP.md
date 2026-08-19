> **Deprecated (historical).** This file is not a current specification.
> Current architecture lives in [`docs/arch/`](../arch/README.md).
> See [`docs/archive/README.md`](README.md) for why this document was archived.

---

# Pointer Arithmetic Implementation Roadmap

## Executive Summary

This document outlines the necessary changes to properly handle pointer arithmetic in the Ripple C compiler. Currently, the IR layer and V2 backend have full support for pointer arithmetic through GEP (GetElementPtr) instructions with bank overflow handling. However, the frontend (AST → IR) needs to be enhanced to generate GEP instructions instead of regular arithmetic for pointer operations.

**Key Insight**: All pointer arithmetic MUST go through GEP to ensure proper bank overflow handling in our segmented memory architecture (4096-instruction banks).

## Current State Analysis

### ✅ What's Already Working

#### IR Layer (`rcc-frontend/src/ir/` - moved from rcc-ir)
- **GetElementPtr instruction** fully defined with fat pointer support
- **IrBuilder methods** for pointer operations:
  - `build_pointer_offset()` - Basic pointer arithmetic
  - `build_pointer_offset_with_bank()` - With explicit bank control
- **Fat pointer representation** with address and bank components
- **Proper type system** distinguishing pointers from integers

#### V2 Backend (`rcc-backend/src/v2/instr/gep.rs`)
- **Complete GEP lowering** with bank overflow detection
- **Static optimization** for compile-time known offsets
- **Dynamic runtime handling** using DIV/MOD for bank calculations
- **Comprehensive tests** covering all edge cases
- **V1 backend removed** - Only clean V2 implementation remains

### ❌ What's Missing

#### Frontend (AST → IR Lowering)
- **No type-aware arithmetic** - Treats `ptr + int` as regular addition
- **No GEP generation** for pointer operations
- **No element size scaling** - Doesn't multiply by sizeof(element)
- **No array indexing lowering** - Doesn't convert `arr[i]` to GEP

## The Problem: Why This Matters

### Example: Array Crossing Bank Boundary

```c
int arr[2000];  // 8000 bytes, spans 2 banks!
int *p = &arr[0];
int *q = p + 1500;  // Should cross into bank 1
int value = *q;     // Must load from correct bank!
```

#### Current (WRONG) Behavior
```llvm
; Frontend generates:
%q = add i16 %p, 1500  ; Just adds 1500, not 1500*4!
; No bank update!
; Result: WRONG ADDRESS, WRONG BANK, CRASH!
```

#### Correct Behavior with GEP
```llvm
; Frontend should generate:
%q = getelementptr i16* %p, i32 1500  ; Scales by sizeof(int)=4
; Backend handles bank overflow:
; offset = 1500 * 4 = 6000
; new_bank = 0 + (6000 / 4096) = 1
; new_addr = 6000 % 4096 = 1904
; Result: Bank 1, Offset 1904 ✓
```

## Implementation Plan

### Phase 1: Frontend Type System Enhancement

#### Task 1.1: Add Type Information to Expression Nodes

**File to modify**: `rcc-frontend/src/ast.rs` (or equivalent)

```rust
pub enum TypedExpr {
    Binary {
        op: BinaryOp,
        left: Box<TypedExpr>,
        right: Box<TypedExpr>,
        expr_type: Type,  // Add type information
    },
    // ...
}
```

#### Task 1.2: Implement Type Checker

**File to create**: `rcc-frontend/src/type_checker.rs`

```rust
pub fn check_expr(expr: &Expr, env: &TypeEnv) -> Result<TypedExpr, TypeError> {
    match expr {
        Expr::Binary { op: Add, left, right } => {
            let left_typed = check_expr(left, env)?;
            let right_typed = check_expr(right, env)?;
            
            match (&left_typed.get_type(), &right_typed.get_type()) {
                (Type::Pointer(elem), Type::Integer) => {
                    // Pointer arithmetic!
                    Ok(TypedExpr::PointerArithmetic {
                        ptr: Box::new(left_typed),
                        offset: Box::new(right_typed),
                        elem_type: elem.clone(),
                    })
                }
                (Type::Integer, Type::Integer) => {
                    // Regular arithmetic
                    Ok(TypedExpr::Binary {
                        op: Add,
                        left: Box::new(left_typed),
                        right: Box::new(right_typed),
                        expr_type: Type::Integer,
                    })
                }
                _ => Err(TypeError::InvalidOperands)
            }
        }
        // ...
    }
}
```

### Phase 2: IR Generation with GEP

#### Task 2.1: Modify IR Builder Usage

**File to modify**: `rcc-frontend/src/ir_gen.rs` (or equivalent)

```rust
pub fn lower_expr(expr: &TypedExpr, builder: &mut IrBuilder) -> Result<Value, Error> {
    match expr {
        TypedExpr::PointerArithmetic { ptr, offset, elem_type } => {
            // Generate GEP instead of Add!
            let ptr_val = lower_expr(ptr, builder)?;
            let offset_val = lower_expr(offset, builder)?;
            let result_type = IrType::FatPtr(Box::new(elem_type.to_ir()));
            
            // Use the existing IrBuilder method!
            builder.build_pointer_offset(ptr_val, offset_val, result_type)
        }
        TypedExpr::Binary { op: Add, left, right, expr_type: Type::Integer } => {
            // Regular integer addition
            let lhs = lower_expr(left, builder)?;
            let rhs = lower_expr(right, builder)?;
            builder.build_binary(IrBinaryOp::Add, lhs, rhs, IrType::I16)
        }
        // ...
    }
}
```

#### Task 2.2: Handle Array Indexing

```rust
TypedExpr::ArrayIndex { array, index } => {
    let array_ptr = lower_expr(array, builder)?;
    let index_val = lower_expr(index, builder)?;
    
    // Generate GEP for address calculation
    let elem_ptr = builder.build_pointer_offset(
        array_ptr, 
        index_val, 
        array.elem_type.to_ir()
    )?;
    
    // Then load from that address
    builder.build_load(elem_ptr, array.elem_type.to_ir())
}
```

#### Task 2.3: Handle Struct Field Access

```rust
TypedExpr::FieldAccess { struct_ptr, field_name } => {
    let ptr = lower_expr(struct_ptr, builder)?;
    let field_info = get_field_info(struct_ptr.get_type(), field_name)?;
    
    // Field offset is compile-time constant
    let offset = Value::Constant(field_info.offset_in_elements);
    
    // Generate GEP for field access
    builder.build_pointer_offset(ptr, offset, field_info.type.to_ir())
}
```

### Phase 3: Pointer Arithmetic Operations

#### Task 3.1: Pointer Subtraction

```rust
TypedExpr::PointerDifference { ptr1, ptr2, elem_type } => {
    // ptr1 - ptr2 returns number of elements between them
    let p1 = lower_expr(ptr1, builder)?;
    let p2 = lower_expr(ptr2, builder)?;
    
    // Calculate byte difference
    let byte_diff = builder.build_binary(IrBinaryOp::Sub, 
                                         get_addr(p1), 
                                         get_addr(p2), 
                                         IrType::I16)?;
    
    // Divide by element size to get element count
    let elem_size = Value::Constant(elem_type.size_in_bytes());
    builder.build_binary(IrBinaryOp::UDiv, 
                        Value::Temp(byte_diff), 
                        elem_size, 
                        IrType::I16)
}
```

#### Task 3.2: Pointer Comparisons

```rust
TypedExpr::PointerComparison { op, ptr1, ptr2 } => {
    // Must consider both address AND bank!
    let p1 = lower_expr(ptr1, builder)?;
    let p2 = lower_expr(ptr2, builder)?;
    
    // For pointers in same bank, compare addresses
    // For pointers in different banks, compare banks first
    // This needs special handling in backend
    generate_bank_aware_comparison(builder, op, p1, p2)
}
```

### Phase 4: Testing Strategy

#### Task 4.1: Unit Tests for Type Checker

```rust
#[test]
fn test_pointer_arithmetic_typing() {
    // int *p; p + 5 should be typed as pointer arithmetic
    let expr = parse("p + 5");
    let typed = type_check(expr, &env).unwrap();
    assert!(matches!(typed, TypedExpr::PointerArithmetic { .. }));
}
```

#### Task 4.2: IR Generation Tests

```rust
#[test]
fn test_pointer_arithmetic_generates_gep() {
    // int *p; p + 5 should generate GEP instruction
    let ir = lower_to_ir("int *p; p + 5;");
    assert!(ir.contains_instruction(|i| 
        matches!(i, Instruction::GetElementPtr { .. })
    ));
}
```

#### Task 4.3: End-to-End Tests

Create test files in `c-test/tests/`:

```c
// test_pointer_arithmetic.c
void putchar(int c);

int main() {
    int arr[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    int *p = arr;
    int *q = p + 5;  // Should point to arr[5]
    
    if (*q == 5) {
        putchar('Y');  // Success
    } else {
        putchar('N');  // Failure
    }
    putchar('\n');
    return 0;
}
```

```c
// test_bank_crossing.c
void putchar(int c);

int main() {
    int huge_array[2000];  // Spans multiple banks
    huge_array[0] = 42;
    huge_array[1500] = 99;  // In different bank!
    
    int *p = &huge_array[0];
    int *q = p + 1500;  // Must handle bank crossing
    
    if (*q == 99) {
        putchar('Y');  // Success - read from correct bank
    } else {
        putchar('N');  // Failure - wrong bank/address
    }
    putchar('\n');
    return 0;
}
```

## Implementation Checklist

### Phase 0: Preparation ✅ COMPLETED
- [x] Cover the current IR with tests, use separate test directory for it.
- [x] Rename the current rcc-ir folder to rcc-backend
- [x] Move the ir implementation to rcc-frontend
- [x] Change the root package.json to correctly build the compiler from the new location
- [x] Update scripts/install.sh to reflect the new structure
- [x] In the new backend, let's remove the old v1 backend implementation — the entry point for v1 is module_lowering.rs

**Phase 0 Completion Notes:**
- IR tests already existed in `rcc-frontend/src/ir/tests.rs`
- Successfully renamed `rcc-ir` → `rcc-backend`
- IR module moved to `rcc-frontend/src/ir/`
- Updated all Cargo.toml dependencies
- Removed v1 backend (`module_lowering.rs`, `lower/` directory, `simple_regalloc.rs`)
- Created compatibility layer with `LoweringOptions` for smooth API transition
- All 294 tests passing
- End-to-end compilation verified working

### Phase 1: Type System
- [x] Add type information to AST nodes - `expr_type: Option<Type>` field in Expression
- [x] Implement type checker - Created `type_checker.rs` with `TypedBinaryOp` classification
- [x] Distinguish pointer from integer expressions - `TypeChecker::check_binary_op()` properly classifies
- [x] Calculate element sizes for pointer types - Using `size_in_words()` for Ripple VM memory model

### Phase 2: IR Generation  
- [x] Route pointer+integer to `build_pointer_offset()`
- [x] Convert array indexing to GEP
- [x] Convert struct field access to GEP - ✅ COMPLETED Dec 2024

### Phase 3: Operations
- [x] Implement pointer subtraction (returns element count) - Implemented `TypedBinaryOp::PointerDifference` 
- [x] Implement pointer comparisons (bank-aware) - Implemented `TypedBinaryOp::Comparison` with `is_pointer_compare` flag
- [x] Handle NULL pointer checks - Not yet implemented
- [ ] Support pointer casts - Not yet implemented

### Phase 4: Testing
- [ ] Type checker unit tests
- [ ] IR generation tests
- [ ] Bank crossing tests
- [ ] Full test suite passes

## Critical Requirements

### MUST Have
1. **All pointer arithmetic through GEP** - Never use regular Add/Sub
2. **Element size scaling** - Multiply offset by sizeof(element)
3. **Bank overflow handling** - Let GEP handle bank boundaries
4. **Type safety** - Can't add two pointers, can't multiply pointers

### Should Have
1. **Optimization** - Use shift for power-of-2 element sizes
2. **Bounds checking** - Optional runtime array bounds checks
3. **Null checks** - Detect NULL pointer dereference

### Nice to Have
1. **Pointer provenance tracking** - For advanced optimizations
2. **Alias analysis** - Determine if pointers might alias
3. **Escape analysis** - Optimize stack allocations

## Common Pitfalls

### ❌ DON'T
- Generate `Add(ptr, int)` - Always use GEP
- Forget element size scaling - `p+1` means next element, not next byte
- Ignore bank boundaries - They're critical for correctness
- Mix pointer and integer arithmetic freely

### ✅ DO
- Type check all expressions before IR generation
- Use `build_pointer_offset()` for all pointer arithmetic
- Test with arrays that span bank boundaries
- Verify element size calculations

## Benefits of Proper Implementation

1. **Correctness**: Programs work correctly across bank boundaries
2. **Safety**: Type system prevents invalid pointer operations
3. **Optimization**: Backend can optimize GEP patterns
4. **Debugging**: Clear separation of pointer vs integer ops
5. **Portability**: Clean IR that could target other architectures

## Example: Complete Flow

### C Code
```c
int arr[2000];
int value = arr[1500];
```

### After Type Checking
```
TypedExpr::ArrayIndex {
    array: TypedExpr::Variable { name: "arr", type: Pointer(I32) },
    index: TypedExpr::Constant { value: 1500, type: I32 }
}
```

### Generated IR
```llvm
%ptr = getelementptr [2000 x i32], [2000 x i32]* @arr, i32 0, i32 1500
%value = load i32, i32* %ptr
```

### V2 Backend Assembly
```asm
; Calculate offset: 1500 * 4 = 6000
LI R3, 1500
LI R4, 4
MUL R5, R3, R4

; Calculate bank crossing
LI R6, 4096
DIV R7, R5, R6    ; R7 = 1 (crossed 1 bank)
MOD R8, R5, R6    ; R8 = 1904 (offset in new bank)

; Load from Bank 1, Offset 1904
ADD R9, GP, R7    ; R9 = bank register (GP + 1)
LOAD R10, R9, R8  ; Load value using correct bank
```

## Timeline Estimate

- **Phase 0**: ✅ COMPLETED (December 2024)
- **Phase 1**: Type System - 1 week
- **Phase 2**: IR Generation - 1 week  
- **Phase 3**: Operations - 3 days
- **Phase 4**: Testing - 3 days

**Total**: ~3 weeks for complete implementation

## Ready for Next Phase

With Phase 0 complete, the codebase is now properly structured for implementing pointer arithmetic:
- IR definitions are in the frontend where type information is available
- Backend contains only the V2 implementation with proven GEP support
- Clean separation of concerns between frontend and backend
- All infrastructure in place for type-aware code generation

## Conclusion

Array and Struct Access → Direct Mapping to C Semantics

In C, array indexing and struct member access have direct semantic equivalents that naturally lower to GetElementPtr (GEP) in IR.

Key Rule
•	arr[i] is exactly *(arr + i) in C.
•	struct_ptr->field is exactly *((char*)struct_ptr + field_offset).

Implication for Lowering
•	Array indexing should never be compiled as raw pointer addition; it should be lowered to GEP(base_ptr, index) where the backend multiplies index by sizeof(element) and applies bank overflow rules.
•	Struct field access should be lowered to GEP(struct_ptr, constant_offset_in_elements) — the offset is known at compile-time from the struct layout.

Why This Matters
•	This approach ensures correct scaling by element size.
•	It automatically handles segmented/banked memory (because GEP is the only place bank overflow logic is implemented).
•	It keeps IR type-safe — index stays an integer, base stays a pointer.
•	It aligns perfectly with the C standard definition of these operators, so the compiler’s behavior matches programmer expectations.

Practical Lowering Pattern

// C:
x = arr[i];

// IR (conceptual):
%elem_ptr = getelementptr i32* %arr, i32 %i
%x = load i32, i32* %elem_ptr

Takeaway
Never special-case arrays or structs in backend assembly generation — always route them through the same GEP lowering path used for general pointer arithmetic.

⸻


Remember: **Every pointer arithmetic operation must go through GEP!**