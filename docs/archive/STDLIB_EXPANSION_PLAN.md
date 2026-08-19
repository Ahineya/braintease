> **Deprecated (historical).** This file is not a current specification.
> Current architecture lives in [`docs/arch/`](../arch/README.md).
> See [`docs/archive/README.md`](README.md) for why this document was archived.

---

# Ripple VM Standard Library Expansion Plan

## Executive Summary

This document outlines a phased approach to expand the Ripple VM standard library, focusing on implementing a robust memory allocator and essential C standard library functions. The plan addresses the unique constraints of the 16-bit architecture with word-based addressing and fat pointers.

## Phase 0: Compiler Prerequisites Investigation (Week 0)

### Objective
Verify that the RCC compiler supports all language features required for the chosen hybrid memory allocator implementation and identify any missing features that need to be implemented first.

### Required Compiler Features to Verify

#### 1. Pointer Arithmetic
- [ ] **Fat pointer arithmetic preservation** - Verify bank tags are maintained
- [ ] **Pointer comparison** - Required for boundary checks
- [ ] **Pointer subtraction** - Required for block size calculations
- [ ] **Void pointer casting** - Essential for generic allocator interface

**Test Code:**
```c
// test_ptr_arithmetic_malloc.c
void test_pointer_requirements() {
    void *p1 = (void*)0x1000;
    void *p2 = (void*)0x2000;
    
    // Test pointer comparison
    if (p2 > p1) putchar('Y'); else putchar('N');
    
    // Test pointer arithmetic
    char *cp = (char*)p1;
    cp += 100;
    if ((void*)cp > p1) putchar('Y'); else putchar('N');
    
    // Test pointer difference
    int diff = (char*)p2 - (char*)p1;
    if (diff == 0x1000) putchar('Y'); else putchar('N');
}
```

#### 2. Structure Support
- [ ] **Nested structures** - Required for free list management
- [ ] **Structure pointers in structures** - Self-referential structures
- [ ] **Structure assignment** - Block header manipulation
- [ ] **Bit fields (optional)** - For flags optimization

**Test Code:**
```c
// test_struct_malloc.c
struct block {
    unsigned short size;
    unsigned short flags;
    struct block *next;
    struct block *prev;
};

void test_struct_requirements() {
    struct block b1 = {16, 0x5A00, 0, 0};
    struct block b2;
    
    // Structure assignment
    b2 = b1;
    if (b2.size == 16) putchar('Y'); else putchar('N');
    
    // Self-referential pointers
    b1.next = &b2;
    b2.prev = &b1;
    if (b1.next->prev == &b1) putchar('Y'); else putchar('N');
}
```

#### 3. Union Support
- [ ] **Basic unions** - Memory reuse in block structures
- [ ] **Unions with pointers** - Free list overlay

**Test Code:**
```c
// test_union_malloc.c
union block_data {
    struct {
        void *next;
        void *prev;
    } free;
    char user_data[32];
};

void test_union_requirements() {
    union block_data block;
    block.free.next = (void*)0x1234;
    
    // Verify union overlay
    if (*(unsigned short*)block.user_data == 0x1234) 
        putchar('Y'); 
    else 
        putchar('N');
}
```

#### 4. Static Variables
- [ ] **Static local variables** - Heap state management
- [ ] **Static initialization** - One-time heap init

**Test Code:**
```c
// test_static_malloc.c
void *get_heap_start() {
    static void *heap_start = (void*)0x1000;
    static int initialized = 0;
    
    if (!initialized) {
        initialized = 1;
        putchar('I'); // Initialized
    }
    
    return heap_start;
}

void test_static_requirements() {
    void *p1 = get_heap_start(); // Should print 'I'
    void *p2 = get_heap_start(); // Should not print 'I'
    
    if (p1 == p2) putchar('Y'); else putchar('N');
}
```

#### 5. Type Casting
- [ ] **void* to char* conversions** - Byte-level access
- [ ] **Integer to pointer conversions** - Address manipulation
- [ ] **Pointer to integer conversions** - Size calculations

#### 6. Inline Assembly (Optional but Useful)
- [ ] **Memory fence instructions** - For thread safety (future)
- [ ] **Efficient bit operations** - For bitmap management

#### 7. Extended Bank Access
- [ ] **LOAD with bank operand** - Cross-bank memory reads
- [ ] **STORE with bank operand** - Cross-bank memory writes
- [ ] **Register X3/X4 availability** - Reserved registers for heap
- [ ] **Inline assembly for bank operations** - Direct bank access

**Test Code:**
```c
// test_bank_access_malloc.c
void test_bank_operations() {
    // Test inline assembly for cross-bank access
    unsigned short value = 0x1234;
    unsigned short bank = 4;
    unsigned short addr = 0x1000;
    
    // Store to bank 4
    __asm__("STORE %0, %1, %2" : : "r"(value), "r"(bank), "r"(addr));
    
    // Load from bank 4
    unsigned short result;
    __asm__("LOAD %0, %1, %2" : "=r"(result) : "r"(bank), "r"(addr));
    
    if (result == 0x1234) putchar('Y'); else putchar('N');
}
```

### Required Runtime Features

#### 1. Memory Layout
- [ ] **Heap bank allocation** - Verify bank 3 is available
- [ ] **Stack/heap separation** - No collision
- [ ] **BSS section** - For static heap state

#### 2. Startup Code (crt0.asm)
- [ ] **Heap initialization hook** - Call heap_init before main
- [ ] **Exit handlers** - For heap cleanup

### Compiler Fixes/Features Needed

Based on initial analysis, these features may need implementation:

1. **Fat Pointer Arithmetic Enhancement**
   - Ensure pointer difference works correctly with fat pointers
   - Verify comparison operators preserve provenance

2. **Static Variable Support**
   - Implement if missing
   - Verify initialization order

3. **Union Support**
   - Full union implementation if missing
   - Verify memory overlay semantics

4. **Build System Updates**
   - Add malloc.c to runtime Makefile
   - Update linking order for heap initialization

### Testing Infrastructure

Create test suite in `c-test/tests/runtime/compiler_features/`:
```
compiler_features/
├── test_ptr_arithmetic.c
├── test_ptr_comparison.c
├── test_struct_self_ref.c
├── test_union_overlay.c
├── test_static_vars.c
└── test_type_casting.c
```

### Phase 0 Deliverables

1. **Compiler Feature Matrix** - Document which features work/don't work
2. **Bug Report List** - File issues for missing features
3. **Workaround Strategies** - Alternative implementations if features missing
4. **Updated Timeline** - Adjust based on required compiler work

## Phase 1: Core Memory Allocator (Weeks 1-2)

### Implementation Components

#### 1. Basic Allocator (Week 1)
- `malloc()` - Segregated free lists with best-fit
- `free()` - With basic coalescing
- Heap initialization in crt0.asm

#### 2. Extended Allocator (Week 2)
- `calloc()` - Zeroed allocation
- `realloc()` - Resize with copy
- Fragmentation mitigation
- Debug helpers (heap_check, heap_stats)

### File Structure
```
runtime/
├── src/
│   ├── malloc.c       # Main allocator
│   ├── heap_init.c    # Initialization
│   └── heap_debug.c   # Debug utilities
├── include/
│   └── stdlib.h       # Updated with prototypes
└── tests/
    └── (moved to c-test/tests/runtime/)
```

### Memory Layout Design - Extended Multi-Bank Heap

#### Multi-Bank Heap Architecture
Using LOAD/STORE with explicit bank operands and reserved registers X3/X4 for heap management:

```
Bank 3 (Primary Heap):
[0x0000-0x00FF] Heap metadata (global state, bank allocation table)
[0x0100-0x0FFF] Small allocations (4-64 words)
[0x1000-0x3FFF] Medium allocations (64-512 words)

Bank 4-7 (Extended Heap):
[0x0000-0x3FFF] Large allocations (512+ words)
Total: 4 banks × 16KB = 64KB extended heap

Bank 8-15 (Optional Ultra-Large Heap):
[0x0000-0x3FFF] Very large allocations
Total: 8 banks × 16KB = 128KB maximum heap
```

#### Register Allocation Strategy
```
X3: Current heap bank selector (for cross-bank operations)
X4: Heap metadata pointer (always points to bank 3)
```

#### Cross-Bank Pointer Format
```c
// Extended fat pointer for multi-bank heap
typedef struct {
    unsigned short addr;    // Address within bank
    unsigned char bank;     // Bank number (3-15)
    unsigned char flags;    // Allocation flags
} heap_ptr_t;
```

## Phase 2: Essential String Functions (Week 3)

### Priority Functions
1. `strlen()` - String length
2. `strcpy()`, `strncpy()` - String copy
3. `strcmp()`, `strncmp()` - String comparison
4. `strcat()`, `strncat()` - String concatenation
5. `strchr()`, `strrchr()` - Character search
6. `memcmp()` - Memory comparison
7. `memmove()` - Overlapping memory copy

### Implementation Strategy
- Pure C implementation initially
- Assembly optimization for critical functions
- Fat pointer aware implementations

## Phase 3: I/O Enhancement (Week 4)

### Components
1. **Enhanced printf**
   - Varargs support (if compiler ready)
   - Additional format specifiers (%u, %o, %p)
   - Field width support

2. **Input Functions**
   - `getchar()` - via MMIO
   - `gets()` - Line input (with safety warnings)
   - Basic `scanf()` - Integer and string parsing

3. **Error Handling**
   - `errno` global variable
   - `perror()` - Error printing
   - Standard error codes

## Phase 4: Utility Functions (Week 5)

### Standard Library Additions
1. **Conversion Functions**
   - `atoi()`, `atol()` - String to integer
   - `itoa()` - Integer to string (non-standard)
   - `strtol()`, `strtoul()` - Advanced parsing

2. **Math Utilities**
   - `abs()`, `labs()` - Absolute value
   - `div()`, `ldiv()` - Division with remainder
   - `rand()`, `srand()` - Already implemented

3. **Program Control**
   - `exit()` - Program termination
   - `abort()` - Abnormal termination
   - `atexit()` - Exit handlers

## Phase 5: Advanced Features (Week 6)

### Character Classification (ctype.h)
- `isalpha()`, `isdigit()`, `isspace()`, etc.
- `toupper()`, `tolower()`
- Lookup table implementation

### Assertions (assert.h)
- `assert()` macro
- Compile-time control via NDEBUG

### Standard Types
- `stdint.h` - Fixed-width integers
- `stdbool.h` - Boolean type
- `stddef.h` - Standard definitions

## Testing Strategy

### Test Categories
1. **Unit Tests** - Individual function validation
2. **Integration Tests** - Multi-function scenarios
3. **Stress Tests** - Memory/performance limits
4. **Regression Tests** - Prevent breakage

### Test Framework Integration
```bash
# Add runtime tests to rct
rct add tests/runtime/test_malloc_basic.c "YY\n"
rct add tests/runtime/test_string_ops.c "strlen:Y strcpy:Y\n"

# Run runtime tests
rct test_malloc_basic test_string_ops

# Run all runtime tests
rct tests/runtime/*
```

## Success Metrics

### Functional Metrics
- [ ] Can compile and run linked list implementation
- [ ] Can compile and run simple text editor
- [ ] Can compile and run basic games (snake, tetris)
- [ ] Passes subset of C99 conformance tests

### Performance Metrics
- [ ] Malloc/free < 100 cycles typical case
- [ ] String operations within 2x of native
- [ ] Memory fragmentation < 20% after stress test
- [ ] Total runtime overhead < 4KB

### Quality Metrics
- [ ] 100% test coverage for implemented functions
- [ ] No memory leaks in test suite
- [ ] Clean compilation with -Wall equivalent
- [ ] Documentation for all public APIs

## Risk Mitigation

### Technical Risks
1. **Compiler limitations** - Mitigate with workarounds or compiler fixes
2. **Memory constraints** - Optimize allocator, use compact structures
3. **Performance issues** - Assembly optimization for critical paths
4. **Fat pointer complexity** - Extensive testing, clear documentation

### Schedule Risks
1. **Compiler fixes take longer** - Prioritize workarounds
2. **Testing reveals issues** - Buffer time in each phase
3. **Integration problems** - Incremental integration approach

## Dependencies

### External Dependencies
- RCC compiler with required features
- Ripple assembler (rasm) and linker (rlink)
- Test runner (rct)

### Internal Dependencies
- Phase 0 must complete before Phase 1
- Malloc (Phase 1) enables many Phase 2-5 features
- String functions (Phase 2) required for I/O (Phase 3)

## Appendix A: Extended Multi-Bank Allocator Implementation

### Overview
The allocator leverages Ripple VM's bank-aware GEP implementation and LOAD/STORE with explicit bank operands to manage a heap spanning multiple banks (up to 192KB total).

### Key Design Elements

#### 1. Bank-Aware Pointer Structure
```c
// runtime/src/malloc.c

// Extended pointer with bank tracking
typedef struct heap_ptr {
    unsigned short addr;    // Address within bank
    unsigned short bank;    // Bank number (3-15)
} heap_ptr_t;

// Block header for allocations
typedef struct block_header {
    unsigned short size;        // Size in words
    unsigned short flags;       // Bits: [15:8]=magic, [7:1]=reserved, [0]=free
    heap_ptr_t next;           // Next block in free list
    heap_ptr_t prev;           // Previous block in free list
} block_header_t;

// Global heap metadata (stored at bank 3, address 0x0000)
typedef struct heap_meta {
    heap_ptr_t free_lists[NUM_BINS];  // Segregated free lists
    unsigned short bank_map[13];       // Bitmap of used banks (3-15)
    unsigned short total_free;         // Total free words
    unsigned short total_allocated;    // Total allocated words
    unsigned short num_allocations;    // Number of active allocations
    unsigned short largest_free;       // Largest contiguous free block
} heap_meta_t;
```

#### 2. Cross-Bank Operations Using X3/X4
```c
// Inline assembly helpers for cross-bank access
static inline unsigned short load_from_bank(unsigned short bank, unsigned short addr) {
    unsigned short value;
    __asm__ volatile(
        "MOVE X3, %1\n"        // Load bank into X3
        "LOAD %0, X3, %2\n"    // Load from [bank:addr]
        : "=r"(value)
        : "r"(bank), "r"(addr)
        : "X3"
    );
    return value;
}

static inline void store_to_bank(unsigned short bank, unsigned short addr, unsigned short value) {
    __asm__ volatile(
        "MOVE X3, %1\n"        // Load bank into X3
        "STORE %2, X3, %0\n"   // Store to [bank:addr]
        :
        : "r"(addr), "r"(bank), "r"(value)
        : "X3"
    );
}

// Helper to read block header from any bank
static block_header_t read_block_header(heap_ptr_t ptr) {
    block_header_t header;
    unsigned short base_addr = ptr.addr;
    
    header.size = load_from_bank(ptr.bank, base_addr);
    header.flags = load_from_bank(ptr.bank, base_addr + 1);
    header.next.addr = load_from_bank(ptr.bank, base_addr + 2);
    header.next.bank = load_from_bank(ptr.bank, base_addr + 3);
    header.prev.addr = load_from_bank(ptr.bank, base_addr + 4);
    header.prev.bank = load_from_bank(ptr.bank, base_addr + 5);
    
    return header;
}
```

#### 3. Allocation Strategy
```c
void *malloc(int size) {
    // Convert bytes to words (round up)
    unsigned short words = (size + 1) / 2;
    unsigned short total_words = words + HEADER_SIZE;
    
    // Align to minimum block size
    if (total_words < MIN_BLOCK_SIZE) {
        total_words = MIN_BLOCK_SIZE;
    }
    
    // Load metadata from bank 3
    __asm__ volatile("MOVE X4, 3");  // X4 = metadata bank
    heap_meta_t *meta = (heap_meta_t *)0x0000;
    
    // Determine allocation strategy based on size
    if (total_words <= 64) {
        // Small allocation - use segregated lists in bank 3
        return alloc_small(meta, total_words);
    } else if (total_words <= 512) {
        // Medium allocation - use bank 3 large area
        return alloc_medium(meta, total_words);
    } else {
        // Large allocation - find available bank (4-15)
        return alloc_large_multibank(meta, total_words);
    }
}

// Large allocation across banks
static void *alloc_large_multibank(heap_meta_t *meta, unsigned short words) {
    // Find contiguous banks if needed
    unsigned short banks_needed = (words + BANK_SIZE - 1) / BANK_SIZE;
    
    if (banks_needed == 1) {
        // Single bank allocation
        for (int bank = 4; bank <= 15; bank++) {
            if (!(meta->bank_map[bank - 3] & 0x8000)) {  // Check if bank is free
                // Allocate from this bank
                heap_ptr_t ptr = {0x0000, bank};
                
                // Mark bank as used
                meta->bank_map[bank - 3] = 0x8000 | words;
                
                // Set up block header
                block_header_t header = {
                    .size = words,
                    .flags = MAGIC_ALLOCATED,
                    .next = {0, 0},
                    .prev = {0, 0}
                };
                write_block_header(ptr, &header);
                
                // Return user pointer (skip header)
                return make_fat_pointer(bank, HEADER_SIZE);
            }
        }
    } else {
        // Multi-bank allocation (very large)
        // Find contiguous free banks
        for (int start_bank = 4; start_bank <= 16 - banks_needed; start_bank++) {
            if (can_allocate_banks(meta, start_bank, banks_needed)) {
                return alloc_multibank(meta, start_bank, banks_needed, words);
            }
        }
    }
    
    return NULL;  // Out of memory
}
```

#### 4. Compiler Integration
```c
// Helper to create fat pointer that compiler understands
static void *make_fat_pointer(unsigned short bank, unsigned short addr) {
    // This creates a fat pointer with proper bank tagging
    // The compiler will track this through its GEP implementation
    void *result;
    __asm__ volatile(
        "MOVE X3, %1\n"        // Bank in X3
        "MOVE %0, %2\n"        // Address in result
        "# FAT_PTR bank=X3"    // Compiler hint
        : "=r"(result)
        : "r"(bank), "r"(addr)
        : "X3"
    );
    return result;
}
```

### Testing Strategy

#### Phase 0 Tests - Verify Bank Operations
```c
// test_bank_operations.c
void test_cross_bank_access() {
    // Test direct bank access
    store_to_bank(4, 0x1000, 0x1234);
    unsigned short val = load_from_bank(4, 0x1000);
    if (val == 0x1234) putchar('Y'); else putchar('N');
    
    // Test X3/X4 preservation
    __asm__("MOVE X3, 5");
    __asm__("MOVE X4, 6");
    store_to_bank(7, 0x2000, 0x5678);
    unsigned short x3_val, x4_val;
    __asm__("MOVE %0, X3" : "=r"(x3_val));
    __asm__("MOVE %0, X4" : "=r"(x4_val));
    
    // X3 should be modified (7), X4 preserved (6)
    if (x3_val == 7) putchar('Y'); else putchar('N');
    if (x4_val == 6) putchar('Y'); else putchar('N');
}
```

#### Integration with GEP
Since the compiler's GEP implementation is already bank-aware:
1. Pointers returned by malloc will have proper bank tags
2. Array indexing will automatically handle bank crossings
3. The compiler tracks bank info through BankInfo enum

### Performance Considerations

1. **Bank Switching Overhead**: Minimize by segregating sizes
2. **Metadata Access**: Keep in bank 3 with X4 as dedicated pointer
3. **Coalescing**: Only within same bank to avoid complexity
4. **Free List Management**: Per-bank free lists for O(1) operations

## Appendix B: Compiler Architecture Integration

### Frontend GEP Model
The RCC compiler follows the LLVM model where all pointer arithmetic is handled through GetElementPtr (GEP) instructions in the frontend IR. This provides several advantages for the standard library implementation:

1. **Separation of Concerns**
   - Frontend: Handles all type-aware pointer arithmetic
   - Backend: Handles bank boundary crossing and fat pointer management
   - Runtime: Can focus on allocation strategy without pointer arithmetic complexity

2. **Type Safety**
   - GEP ensures type-correct pointer arithmetic
   - Element sizes are computed at compile time
   - Array bounds can be checked statically when possible

3. **Bank Transparency for Library Code**
   ```c
   // In malloc implementation, this code:
   block_header_t *next = current + 1;
   
   // Becomes in IR:
   // %next = getelementptr %current, 1
   
   // Backend automatically handles:
   // - Computing offset (1 * sizeof(block_header_t))
   // - Checking for bank overflow
   // - Updating bank register if needed
   ```

### Implications for Standard Library

#### Memory Functions
Since pointer arithmetic is handled by GEP, memory functions can be simpler:
```c
void memcpy(void *dest, const void *src, int n) {
    char *d = (char*)dest;
    const char *s = (const char*)src;
    
    // Simple loop - GEP handles bank crossing
    for (int i = 0; i < n; i++) {
        d[i] = s[i];  // GEP handles d+i and s+i
    }
}
```

#### String Functions
String operations benefit from automatic bank handling:
```c
int strlen(const char *s) {
    int len = 0;
    // GEP handles s+len across banks
    while (s[len] != '\0') {
        len++;
    }
    return len;
}
```

#### Malloc Implementation
The allocator can treat pointers as opaque:
```c
typedef struct block {
    struct block *next;  // GEP handles dereferencing
    struct block *prev;  // Even across banks
    unsigned short size;
    unsigned short flags;
} block_t;

void *malloc(int size) {
    block_t *current = free_list;
    
    while (current) {
        if (current->size >= size) {
            // Split block if needed
            if (current->size > size + MIN_BLOCK) {
                // GEP computes new block address
                block_t *new_block = (block_t*)((char*)current + size + sizeof(block_t));
                new_block->size = current->size - size - sizeof(block_t);
                // ...
            }
            return (char*)current + sizeof(block_t);
        }
        current = current->next;  // GEP handles traversal
    }
    
    // Allocate from new bank if needed
    return alloc_from_new_bank(size);
}
```

### Compiler Support Requirements

#### Required Features (Phase 0 Verification)
1. ✓ **GEP with bank overflow** - Already implemented in backend
2. ✓ **Fat pointer tracking** - BankInfo enum tracks through compilation
3. ? **Inline assembly** - For bank-specific operations
4. ? **Volatile operations** - For MMIO and metadata access
5. ? **Static variables** - For heap state
6. ? **Function pointers** - For atexit handlers

#### Nice-to-Have Features
1. **Builtin memcpy** - Compiler could optimize block copies
2. **Builtin memset** - Compiler could optimize memory clearing
3. **Bank hints** - Pragma to suggest bank allocation

### Testing the Integration

```c
// test_gep_malloc_integration.c
void test_malloc_with_gep() {
    // Allocate array
    int *arr = (int*)malloc(100 * sizeof(int));
    
    // Fill array - GEP handles indexing
    for (int i = 0; i < 100; i++) {
        arr[i] = i * i;  // May cross banks transparently
    }
    
    // Verify - GEP handles reading
    for (int i = 0; i < 100; i++) {
        if (arr[i] != i * i) {
            putchar('N');
            return;
        }
    }
    
    putchar('Y');
    free(arr);
}

// test_cross_bank_structure.c
typedef struct large_struct {
    char data[8192];  // Spans 2 banks
} large_struct_t;

void test_cross_bank_struct() {
    large_struct_t *s = (large_struct_t*)malloc(sizeof(large_struct_t));
    
    // Write pattern
    for (int i = 0; i < 8192; i++) {
        s->data[i] = i & 0xFF;  // GEP handles bank crossing
    }
    
    // Verify pattern
    for (int i = 0; i < 8192; i++) {
        if (s->data[i] != (i & 0xFF)) {
            putchar('N');
            return;
        }
    }
    
    putchar('Y');
    free(s);
}
```

## Appendix C: Compiler Feature Test Results

[To be filled after Phase 0 investigation]

## Appendix C: API Documentation Template

```c
/**
 * malloc - Allocate memory from heap
 * @size: Number of bytes to allocate
 * 
 * Returns pointer to allocated memory or NULL on failure.
 * Memory is not initialized. Free with free().
 * 
 * Implementation: Segregated free lists with best-fit
 * Time complexity: O(n) worst case, O(1) typical
 * Space overhead: 4 words per allocation
 */
void *malloc(int size);
```