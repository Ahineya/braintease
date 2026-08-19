> **Deprecated (historical).** This file is not a current specification.
> Current architecture lives in [`docs/arch/`](../arch/README.md).
> See [`docs/archive/README.md`](README.md) for why this document was archived.

---

# Compiler-Level Floating-Point Support for Ripple VM

## Overview
This document outlines the implementation plan for native `float` and `double` support in the RCC compiler, which will transparently lower floating-point operations to Ripple VM-compatible software implementations.

## Compiler Architecture Changes

### 1. Frontend Type System Extensions

#### Type Representation in IR
```rust
// rcc-frontend/src/ir/types.rs
#[derive(Debug, Clone, PartialEq)]
pub enum Type {
    // Existing types
    Int(IntType),
    Pointer(Box<Type>),
    Array(Box<Type>, usize),
    Struct(String),
    // New floating-point types
    Float,      // 32-bit IEEE 754 single precision
    Double,     // 64-bit IEEE 754 double precision
}

impl Type {
    pub fn size_in_cells(&self) -> usize {
        match self {
            Type::Float => 2,   // 32 bits = 2 words
            Type::Double => 4,  // 64 bits = 4 words
            // ... existing cases
        }
    }
}
```

#### Value Representation
```rust
// rcc-frontend/src/ir/value.rs
#[derive(Debug, Clone)]
pub enum Value {
    // Existing variants
    Constant(i64),
    Temp(TempId),
    Global(String),
    FatPtr(FatPointer),
    // New floating-point variants
    FloatConstant(f32),
    DoubleConstant(f64),
}
```

### 2. Frontend IR Instructions

```rust
// rcc-frontend/src/ir/instructions.rs
#[derive(Debug, Clone)]
pub enum Instruction {
    // Existing instructions...
    
    // Floating-point arithmetic
    FAdd { result: TempId, lhs: Value, rhs: Value, ty: Type },
    FSub { result: TempId, lhs: Value, rhs: Value, ty: Type },
    FMul { result: TempId, lhs: Value, rhs: Value, ty: Type },
    FDiv { result: TempId, lhs: Value, rhs: Value, ty: Type },
    FRem { result: TempId, lhs: Value, rhs: Value, ty: Type },
    
    // Floating-point comparison
    FCmp { result: TempId, op: FCmpOp, lhs: Value, rhs: Value, ty: Type },
    
    // Conversions
    FPToSI { result: TempId, value: Value, from: Type, to: IntType },  // float to signed int
    FPToUI { result: TempId, value: Value, from: Type, to: IntType },  // float to unsigned int
    SIToFP { result: TempId, value: Value, from: IntType, to: Type },  // signed int to float
    UIToFP { result: TempId, value: Value, from: IntType, to: Type },  // unsigned int to float
    FPExt { result: TempId, value: Value },   // float to double
    FPTrunc { result: TempId, value: Value }, // double to float
}

#[derive(Debug, Clone, PartialEq)]
pub enum FCmpOp {
    OEQ,  // Ordered and equal
    ONE,  // Ordered and not equal
    OLT,  // Ordered and less than
    OLE,  // Ordered and less than or equal
    OGT,  // Ordered and greater than
    OGE,  // Ordered and greater than or equal
    UEQ,  // Unordered or equal
    UNE,  // Unordered or not equal
    // ... etc
}
```

### 3. Lexer/Parser Updates

```c
// Support for float literals in lexer
float pi = 3.14159f;
double e = 2.71828;
float sci = 1.23e-4f;
double big = 6.022e23;

// Parser grammar extensions
primary_expression:
    | FLOAT_LITERAL
    | DOUBLE_LITERAL
    ;

type_specifier:
    | FLOAT
    | DOUBLE
    ;
```

### 4. Backend Lowering Strategy

#### Float Storage Layout
```rust
// rcc-backend/src/v2/float_layout.rs

/// Float representation in memory (2 words)
/// Word 0: Mantissa bits [15:0]
/// Word 1: [Sign:1][Exponent:8][Mantissa:7]
pub struct FloatLayout;

impl FloatLayout {
    pub fn store_float(value: f32) -> (u16, u16) {
        let bits = value.to_bits();
        let word0 = (bits & 0xFFFF) as u16;
        let word1 = ((bits >> 16) & 0xFFFF) as u16;
        (word0, word1)
    }
    
    pub fn load_float(word0: u16, word1: u16) -> f32 {
        let bits = ((word1 as u32) << 16) | (word0 as u32);
        f32::from_bits(bits)
    }
}

/// Double representation in memory (4 words)
pub struct DoubleLayout;

impl DoubleLayout {
    pub fn store_double(value: f64) -> (u16, u16, u16, u16) {
        let bits = value.to_bits();
        let word0 = (bits & 0xFFFF) as u16;
        let word1 = ((bits >> 16) & 0xFFFF) as u16;
        let word2 = ((bits >> 32) & 0xFFFF) as u16;
        let word3 = ((bits >> 48) & 0xFFFF) as u16;
        (word0, word1, word2, word3)
    }
}
```

#### Lowering Floating-Point Operations
```rust
// rcc-backend/src/v2/lower_float.rs

pub fn lower_fadd(
    mgr: &mut RegisterPressureManager,
    result: TempId,
    lhs: &Value,
    rhs: &Value,
    ty: &Type,
) -> Vec<AsmInst> {
    let mut insts = vec![];
    
    match ty {
        Type::Float => {
            // Load float components into registers
            let lhs_w0 = load_float_word(mgr, lhs, 0);
            let lhs_w1 = load_float_word(mgr, lhs, 1);
            let rhs_w0 = load_float_word(mgr, rhs, 0);
            let rhs_w1 = load_float_word(mgr, rhs, 1);
            
            // Call software float addition
            // __rcc_float_add(lhs_w0, lhs_w1, rhs_w0, rhs_w1)
            insts.push(AsmInst::Move(Reg::A0, lhs_w0));
            insts.push(AsmInst::Move(Reg::A1, lhs_w1));
            insts.push(AsmInst::Move(Reg::A2, rhs_w0));
            insts.push(AsmInst::Move(Reg::A3, rhs_w1));
            insts.push(AsmInst::Jal(Reg::Ra, "__rcc_float_add".to_string()));
            
            // Result in R0:R1, store to result temp
            store_float_result(mgr, result, Reg::R0, Reg::R1);
        }
        Type::Double => {
            // Similar but with 4 words per operand
            // Call __rcc_double_add
        }
        _ => panic!("Invalid type for FAdd"),
    }
    
    insts
}
```

### 5. Runtime Library Implementation

```c
// runtime/src/softfloat.c

// Float addition (called by compiler-generated code)
// Returns result in R0:R1
void __rcc_float_add(unsigned short a_w0, unsigned short a_w1,
                     unsigned short b_w0, unsigned short b_w1) {
    // Combine words into float representation
    float32_bits a = {.words = {a_w0, a_w1}};
    float32_bits b = {.words = {b_w0, b_w1}};
    
    // Extract components
    int sign_a = (a.words[1] >> 15) & 1;
    int exp_a = (a.words[1] >> 7) & 0xFF;
    unsigned int mant_a = ((a.words[1] & 0x7F) << 16) | a.words[0];
    
    int sign_b = (b.words[1] >> 15) & 1;
    int exp_b = (b.words[1] >> 7) & 0xFF;
    unsigned int mant_b = ((b.words[1] & 0x7F) << 16) | b.words[0];
    
    // Handle special cases
    if (exp_a == 0xFF) return a; // NaN or Inf
    if (exp_b == 0xFF) return b; // NaN or Inf
    if (exp_a == 0 && mant_a == 0) return b; // a is zero
    if (exp_b == 0 && mant_b == 0) return a; // b is zero
    
    // Align exponents
    if (exp_a < exp_b) {
        int diff = exp_b - exp_a;
        if (diff > 24) return b; // a is too small
        mant_a >>= diff;
        exp_a = exp_b;
    } else if (exp_b < exp_a) {
        int diff = exp_a - exp_b;
        if (diff > 24) return a; // b is too small
        mant_b >>= diff;
    }
    
    // Add or subtract based on signs
    unsigned int result_mant;
    int result_sign;
    if (sign_a == sign_b) {
        result_mant = mant_a + mant_b;
        result_sign = sign_a;
    } else {
        if (mant_a >= mant_b) {
            result_mant = mant_a - mant_b;
            result_sign = sign_a;
        } else {
            result_mant = mant_b - mant_a;
            result_sign = sign_b;
        }
    }
    
    // Normalize result
    int result_exp = exp_a;
    if (result_mant & 0x01000000) {
        // Overflow, shift right
        result_mant >>= 1;
        result_exp++;
    } else {
        // Normalize left
        while (result_exp > 0 && !(result_mant & 0x00800000)) {
            result_mant <<= 1;
            result_exp--;
        }
    }
    
    // Pack result
    __asm__("MOVE R0, %0" : : "r"(result_mant & 0xFFFF));
    __asm__("MOVE R1, %0" : : "r"((result_sign << 15) | (result_exp << 7) | ((result_mant >> 16) & 0x7F)));
}
```

### 6. Optimization Opportunities

#### Constant Folding
```rust
// During IR generation, fold float constants
match (lhs, rhs) {
    (Value::FloatConstant(a), Value::FloatConstant(b)) => {
        // Compute at compile time
        let result = a + b;
        Value::FloatConstant(result)
    }
    _ => // Generate FAdd instruction
}
```

#### Intrinsic Recognition
```rust
// Recognize patterns and use optimized implementations
// x * 2.0 -> add 1 to exponent
// x / 2.0 -> subtract 1 from exponent
// x * 1.0 -> no-op
// x + 0.0 -> copy (but handle -0.0 correctly)
```

#### Inline Expansion for Simple Operations
```rust
// For simple operations, inline instead of calling runtime
fn lower_float_negate(value: &Value) -> Vec<AsmInst> {
    // Just flip the sign bit
    let mut insts = vec![];
    let word1 = load_float_word(mgr, value, 1);
    insts.push(AsmInst::XorI(word1, word1, 0x8000)); // Flip sign bit
    insts
}
```

### 7. GEP and Memory Access

```rust
// Float arrays need special GEP handling
impl GEP {
    fn handle_float_array(&self, base: &Value, index: &Value) -> Vec<AsmInst> {
        // Each float is 2 words
        let offset = index * 2;
        // Use existing GEP with correct element size
        lower_gep(mgr, naming, base, &[offset], 2, result_temp, bank_size)
    }
}
```

### 8. ABI Considerations

```rust
// Calling convention for float parameters
// Option 1: Pass in registers (2 registers per float)
// Option 2: Pass on stack (simpler but slower)

pub struct FloatABI;

impl FloatABI {
    pub fn pass_float_arg(arg_num: usize, value: f32) -> Vec<AsmInst> {
        let (w0, w1) = FloatLayout::store_float(value);
        if arg_num < 2 {
            // First 2 floats in registers A0:A1, A2:A3
            vec![
                AsmInst::Li(Reg::from_arg(arg_num * 2), w0 as i16),
                AsmInst::Li(Reg::from_arg(arg_num * 2 + 1), w1 as i16),
            ]
        } else {
            // Rest on stack
            push_to_stack(w0, w1)
        }
    }
    
    pub fn return_float(value: f32) -> Vec<AsmInst> {
        let (w0, w1) = FloatLayout::store_float(value);
        vec![
            AsmInst::Li(Reg::R0, w0 as i16),
            AsmInst::Li(Reg::R1, w1 as i16),
        ]
    }
}
```

## Implementation Phases

### Phase 1: Basic Float Support (Week 1-2)
1. **Frontend**
   - [ ] Add float/double types to type system
   - [ ] Parse float literals
   - [ ] Type checking for float operations
   - [ ] Generate float IR instructions

2. **Backend**
   - [ ] Float storage layout
   - [ ] Lower FAdd, FSub to runtime calls
   - [ ] Load/store float values
   - [ ] Float constant handling

3. **Runtime**
   - [ ] Basic arithmetic (add, sub, mul, div)
   - [ ] Comparison operations
   - [ ] NaN/Inf handling

### Phase 2: Conversions (Week 3)
1. **Frontend**
   - [ ] Implicit conversions (int promotion)
   - [ ] Explicit casts
   - [ ] Type coercion rules

2. **Backend**
   - [ ] Lower conversion instructions
   - [ ] Optimize trivial conversions

3. **Runtime**
   - [ ] int ↔ float conversions
   - [ ] float ↔ double conversions
   - [ ] String conversions (for printf/scanf)

### Phase 3: Math Library (Week 4-5)
1. **Standard Functions**
   - [ ] sqrt, cbrt
   - [ ] sin, cos, tan
   - [ ] exp, log, pow
   - [ ] ceil, floor, round

2. **Optimizations**
   - [ ] Table-driven approximations
   - [ ] CORDIC implementations
   - [ ] Fast reciprocal

### Phase 4: Advanced Features (Week 6)
1. **Compiler Optimizations**
   - [ ] Constant folding
   - [ ] Strength reduction
   - [ ] Common subexpression elimination

2. **Vectorization**
   - [ ] SIMD-style operations on float arrays
   - [ ] Loop optimizations

3. **Debugging Support**
   - [ ] Float value printing in debugger
   - [ ] NaN/Inf detection and reporting

## Testing Strategy

### Compiler Tests
```c
// c-test/tests/float/test_float_basic.c
int main() {
    float a = 3.14f;
    float b = 2.71f;
    float c = a + b;
    
    // Should print 5.85 (approximately)
    if (c > 5.84f && c < 5.86f) {
        putchar('Y');
    } else {
        putchar('N');
    }
    
    return 0;
}
```

### IR Generation Tests
```rust
#[test]
fn test_float_ir_generation() {
    let input = "float x = 1.0f + 2.0f;";
    let ir = parse_and_generate_ir(input);
    
    assert!(matches!(
        ir.instructions[0],
        Instruction::FAdd { .. }
    ));
}
```

### Backend Tests
```rust
#[test]
fn test_float_lowering() {
    let fadd = Instruction::FAdd {
        result: 0,
        lhs: Value::FloatConstant(1.0),
        rhs: Value::FloatConstant(2.0),
        ty: Type::Float,
    };
    
    let asm = lower_instruction(&fadd);
    
    // Should generate call to __rcc_float_add
    assert!(asm.iter().any(|inst| 
        matches!(inst, AsmInst::Jal(_, name) if name.contains("float_add"))
    ));
}
```

## Performance Targets

| Operation | Target Cycles | Acceptable Range |
|-----------|--------------|------------------|
| Float Add/Sub | 50-75 | 50-100 |
| Float Mul | 75-100 | 75-150 |
| Float Div | 150-200 | 150-300 |
| Float Sqrt | 200-250 | 200-400 |
| Float Sin/Cos | 300-400 | 300-600 |
| Int→Float | 25-40 | 25-50 |
| Float→Int | 25-40 | 25-50 |

## Memory Overhead

- **Runtime library**: ~8KB for basic ops, ~16KB with full math library
- **Lookup tables**: ~2KB for optimization tables
- **Per float**: 2 words (4 bytes)
- **Per double**: 4 words (8 bytes)

## Compiler Flags

```bash
# Compilation options
rcc -msoft-float    # Use software floating point (default)
rcc -ffast-math     # Enable unsafe optimizations
rcc -fno-float      # Disable float support entirely
rcc -fsingle-precision-constant  # Treat unsuffixed float constants as float, not double
```

## ABI Documentation

### Calling Convention
- **Float arguments**: Passed in register pairs (A0:A1, A2:A3) or on stack
- **Float return**: Returned in R0:R1
- **Double arguments**: Passed in 4 registers or on stack
- **Double return**: Returned in R0:R1:R2:R3

### Structure Layout
```c
struct with_float {
    int x;      // Offset 0 (1 word)
    float y;    // Offset 1 (2 words, may need padding)
    int z;      // Offset 3 (1 word)
};  // Total: 4 words
```

## Known Limitations

1. **Performance**: Software floats are 50-100x slower than hardware
2. **Precision**: May not be fully IEEE 754 compliant in all edge cases
3. **Library size**: Full math library adds significant code size
4. **Debugging**: Harder to debug float operations in assembly

## Future Enhancements

1. **Hardware Acceleration**: MMIO-based FPU coprocessor
2. **Fixed-Point Alternative**: Compiler option for Q16.16 fixed-point
3. **Vector Operations**: SIMD-style float array operations
4. **Profile-Guided Optimization**: Optimize hot float code paths