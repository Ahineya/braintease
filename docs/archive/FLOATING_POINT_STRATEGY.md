> **Deprecated (historical).** This file is not a current specification.
> Current architecture lives in [`docs/arch/`](../arch/README.md).
> See [`docs/archive/README.md`](README.md) for why this document was archived.

---

# Floating-Point Support Strategy for Ripple VM

## Overview
Ripple VM is a 16-bit architecture without native floating-point support. This document outlines strategies for implementing IEEE 754-compliant floating-point operations in software, considering the platform's word-based addressing and limited register set.

## Representation Options

### Option 1: IEEE 754 Binary32 (float) - 32-bit
**Standard single-precision format**
```
[Sign:1][Exponent:8][Mantissa:23]
Total: 32 bits (2 words in Ripple VM)
```

**Pros:**
- Industry standard compliance
- Existing algorithms available
- Good precision for most applications

**Cons:**
- Requires 2 words per float
- Complex to manipulate with 16-bit operations

### Option 2: IEEE 754 Binary16 (half) - 16-bit
**Half-precision format**
```
[Sign:1][Exponent:5][Mantissa:10]
Total: 16 bits (1 word in Ripple VM)
```

**Pros:**
- Fits in single word
- Faster operations
- Less memory usage

**Cons:**
- Limited precision (3-4 decimal digits)
- Limited range (±65,504)
- May not be sufficient for many applications

### Option 3: Custom 32-bit Format (Recommended)
**Optimized for 16-bit operations**
```
Word 0: [Sign:1][Exponent:7][Mantissa High:8]
Word 1: [Mantissa Low:16]
Total: 32 bits (2 words)
```

**Pros:**
- Easier to extract components with 16-bit ops
- Byte-aligned exponent
- Good balance of range and precision

**Cons:**
- Non-standard (needs conversion for I/O)
- Custom implementation required

## Implementation Architecture

### 1. Type Definition
```c
// Use union for easy component access
typedef union {
    struct {
        unsigned short low;   // Mantissa low bits
        unsigned short high;  // Sign, exponent, mantissa high
    } words;
    struct {
        unsigned int mantissa : 24;  // If bit fields work
        unsigned int exponent : 7;
        unsigned int sign : 1;
    } parts;
    unsigned int raw;  // For whole-number operations
} soft_float;

// Alternative: Use struct with explicit layout
typedef struct {
    unsigned short mantissa_low;
    unsigned char mantissa_high;
    unsigned char exp_sign;  // [Sign:1][Exp:7]
} float32_t;
```

### 2. Basic Operations Structure
```c
// Addition/Subtraction algorithm
soft_float float_add(soft_float a, soft_float b) {
    // 1. Extract components
    int sign_a = extract_sign(a);
    int exp_a = extract_exponent(a);
    unsigned int mant_a = extract_mantissa(a);
    
    // 2. Handle special cases
    if (is_zero(a)) return b;
    if (is_zero(b)) return a;
    if (is_nan(a) || is_nan(b)) return float_nan();
    
    // 3. Align exponents (shift mantissa of smaller number)
    int exp_diff = exp_a - exp_b;
    if (exp_diff < 0) {
        mant_a >>= -exp_diff;
        exp_a = exp_b;
    } else {
        mant_b >>= exp_diff;
    }
    
    // 4. Add/subtract mantissas based on signs
    unsigned int result_mant;
    if (sign_a == sign_b) {
        result_mant = mant_a + mant_b;
    } else {
        result_mant = mant_a - mant_b;
    }
    
    // 5. Normalize result
    return normalize_float(sign_a, exp_a, result_mant);
}
```

### 3. Multiplication Strategy
```c
soft_float float_mul(soft_float a, soft_float b) {
    // Key challenge: 24-bit × 24-bit = 48-bit result
    // Solution: Break into 16-bit chunks
    
    // Split mantissas into high and low parts
    unsigned short a_low = a.words.low;
    unsigned short a_high = (a.words.high & 0xFF);
    unsigned short b_low = b.words.low;
    unsigned short b_high = (b.words.high & 0xFF);
    
    // Compute partial products (each fits in 32 bits)
    unsigned int p00 = a_low * b_low;
    unsigned int p01 = a_low * b_high;
    unsigned int p10 = a_high * b_low;
    unsigned int p11 = a_high * b_high;
    
    // Sum partial products with proper shifts
    unsigned int result_low = p00 + ((p01 + p10) << 8);
    unsigned int result_high = p11 + ((p01 + p10) >> 8);
    
    // Add exponents
    int result_exp = extract_exponent(a) + extract_exponent(b) - BIAS;
    
    // XOR signs
    int result_sign = extract_sign(a) ^ extract_sign(b);
    
    return normalize_float(result_sign, result_exp, result_high);
}
```

## Memory Layout Considerations

### Word-Based Addressing Impact
```c
// Storing floats in arrays needs careful alignment
typedef struct {
    unsigned short data[2];  // Two words per float
} float_storage;

// Array access pattern
float_storage float_array[100];

// Reading a float
soft_float read_float(int index) {
    soft_float result;
    result.words.low = float_array[index].data[0];
    result.words.high = float_array[index].data[1];
    return result;
}
```

### Bank Crossing Considerations
```c
// Large float arrays may span banks
// GEP will handle this automatically
float_storage *large_array = malloc(1000 * sizeof(float_storage));
// This could span multiple banks, but GEP handles indexing
```

## Optimization Strategies

### 1. Table-Driven Approaches
```c
// Precomputed tables for common operations
static const unsigned short recip_table[256];  // 1/x approximations
static const unsigned short sqrt_table[256];   // sqrt approximations
static const short exp_table[128];             // 2^x for small x

// Fast reciprocal approximation
soft_float fast_recip(soft_float x) {
    int exp = extract_exponent(x);
    unsigned int mant = extract_mantissa(x);
    
    // Table lookup for mantissa reciprocal
    unsigned char index = mant >> 16;  // Top 8 bits
    unsigned short recip_approx = recip_table[index];
    
    // Adjust exponent
    int result_exp = BIAS - (exp - BIAS);
    
    // One Newton-Raphson iteration for accuracy
    return newton_raphson_recip(x, recip_approx, result_exp);
}
```

### 2. Fixed-Point Fallback
```c
// For many operations, fixed-point may be sufficient
typedef struct {
    int whole;      // Integer part
    unsigned short frac;  // Fractional part (0.16 format)
} fixed32_t;

// Faster than float for add/subtract
fixed32_t fixed_add(fixed32_t a, fixed32_t b) {
    fixed32_t result;
    unsigned int frac_sum = a.frac + b.frac;
    result.frac = frac_sum & 0xFFFF;
    result.whole = a.whole + b.whole + (frac_sum >> 16);
    return result;
}
```

### 3. Compiler Intrinsics
```c
// Compiler could recognize patterns and optimize
#define FLOAT_ADD(a, b) __builtin_float_add(a, b)
#define FLOAT_MUL(a, b) __builtin_float_mul(a, b)

// Backend generates optimized instruction sequences
```

## Implementation Phases

### Phase 1: Basic Operations (Week 1)
- [ ] Float representation structure
- [ ] Addition/subtraction
- [ ] Basic comparison operations
- [ ] Zero/NaN/Inf handling

### Phase 2: Multiplication/Division (Week 2)
- [ ] Multiplication with 16-bit decomposition
- [ ] Division using Newton-Raphson
- [ ] Remainder operation

### Phase 3: Conversions (Week 3)
- [ ] int to float
- [ ] float to int (with truncation/rounding modes)
- [ ] String to float (strtof)
- [ ] Float to string (printf %f support)

### Phase 4: Math Library (Week 4)
- [ ] Square root (Newton-Raphson or CORDIC)
- [ ] Trigonometric functions (CORDIC or Taylor series)
- [ ] Exponential/logarithm (table + interpolation)
- [ ] Power function

### Phase 5: Optimization (Week 5)
- [ ] Assembly implementations for critical paths
- [ ] Lookup tables for common operations
- [ ] Fast approximation functions
- [ ] Vectorization for array operations

## Testing Strategy

### Compliance Tests
```c
// test_ieee_compliance.c
void test_float_special_cases() {
    soft_float zero = float_from_int(0);
    soft_float one = float_from_int(1);
    soft_float inf = float_inf();
    soft_float nan = float_nan();
    
    // Test special case arithmetic
    assert(float_is_inf(float_div(one, zero)));
    assert(float_is_nan(float_mul(zero, inf)));
    assert(float_is_nan(float_add(inf, float_neg(inf))));
}
```

### Precision Tests
```c
// test_float_precision.c
void test_float_accuracy() {
    // Test against known values
    soft_float pi = float_from_string("3.14159265");
    soft_float e = float_from_string("2.71828183");
    
    soft_float result = float_mul(pi, e);
    // Should be approximately 8.5397342
    
    float error = float_to_native(float_sub(result, float_from_string("8.5397342")));
    assert(fabs(error) < 0.0001);
}
```

### Performance Benchmarks
```c
// benchmark_float_ops.c
void benchmark_float_operations() {
    soft_float a = float_from_int(12345);
    soft_float b = float_from_int(67890);
    
    // Time 1000 operations
    unsigned int start = get_cycle_count();
    for (int i = 0; i < 1000; i++) {
        a = float_add(a, b);
    }
    unsigned int cycles = get_cycle_count() - start;
    
    printf("Float add: %u cycles/op\n", cycles / 1000);
}
```

## Platform-Specific Optimizations

### 1. Leverage 16-bit multiply
```c
// Ripple VM has MUL instruction for 16×16→16
// Use for partial products in float multiplication
unsigned int mul32(unsigned short a_high, unsigned short a_low,
                   unsigned short b_high, unsigned short b_low) {
    // Each multiplication fits in 16-bit result
    unsigned short ll = a_low * b_low;
    unsigned short lh = a_low * b_high;
    unsigned short hl = a_high * b_low;
    unsigned short hh = a_high * b_high;
    
    // Combine with shifts
    return (hh << 16) + ((lh + hl) << 8) + ll;
}
```

### 2. Use shift instructions efficiently
```c
// For normalization, use native shift operations
soft_float normalize(unsigned int mantissa, int exponent) {
    // Count leading zeros (could be optimized with CLZ if available)
    int shift = 0;
    while (!(mantissa & 0x800000)) {
        mantissa <<= 1;
        shift++;
    }
    
    exponent -= shift;
    return pack_float(0, exponent, mantissa);
}
```

### 3. Bank-aware float arrays
```c
// Store float arrays to minimize bank crossings
typedef struct {
    float_storage floats[2048];  // Fits in one bank
} float_bank;

// Allocate per-bank for better locality
float_bank *banks[4];  // 4 banks of floats
```

## Compiler Integration

### Frontend Support
```c
// Compiler recognizes float type and generates soft-float calls
float a = 3.14f;  // Generates: soft_float a = float_from_literal(0x4048F5C3);
float b = a * 2;  // Generates: soft_float b = float_mul(a, float_from_int(2));
```

### Backend Optimization
- Recognize common patterns (multiply by 2 = add exponent)
- Inline simple operations
- Use specialized sequences for constants

## Alternative: Q-Number Fixed Point

For applications that don't need full float:
```c
// Q16.16 format (32-bit fixed point)
typedef struct {
    short integer;
    unsigned short fraction;
} q16_16_t;

// Much faster operations
q16_16_t q_mul(q16_16_t a, q16_16_t b) {
    int result = ((int)a.integer << 16 | a.fraction) * 
                 ((int)b.integer << 16 | b.fraction);
    return (q16_16_t){result >> 32, result >> 16};
}
```

## Recommendations

1. **Start with IEEE 754 binary32** for compatibility
2. **Implement in C first**, optimize later with assembly
3. **Provide both float and fixed-point** options
4. **Use lookup tables** for transcendental functions
5. **Consider hardware acceleration** via MMIO coprocessor (future)

## Resource Requirements

- **Code size**: ~8KB for basic operations, ~16KB with math library
- **Data size**: ~2KB for lookup tables
- **Stack usage**: ~32 words for complex operations
- **Performance**: 50-200 cycles per operation (estimated)