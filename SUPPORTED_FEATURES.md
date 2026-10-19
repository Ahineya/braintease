# RCC Compiler - Currently Supported Features

This document provides a comprehensive list of C99 features currently implemented in the RCC (Ripple C Compiler) frontend, preprocessor, and backend.

## Preprocessor (rcc-preprocessor)

### Directives
- `#include` - File inclusion (both `<>` and `""` forms)
- `#define` - Object-like and function-like macros
- `#undef` - Macro undefinition
- `#if` / `#ifdef` / `#ifndef` - Conditional compilation
- `#elif` / `#else` / `#endif` - Conditional branches
- `#pragma once` - Header guard optimization
- `#line` - Line number control
- Comment removal (line `//` and block `/* */`)
- Macro expansion with recursion protection
- Include path searching
- Include depth limiting (max 200)

### Macro Features
- Object-like macros
- Function-like macros with parameters
- Variadic macros (`__VA_ARGS__`)
- `defined()` operator in conditionals
- Signed/unsigned 64-bit `intmax_t`-style conditional arithmetic and short-circuit evaluation
- Recursive macro expansion with depth limiting

## Lexer

### Keywords (All C99 keywords)
- Storage classes: `auto`, `extern`, `register`, `static`, `typedef`
- Type specifiers: `char`, `short`, `int`, `long`, `signed`, `unsigned`, `float`, `double`, `void`
- Type qualifiers: `const`, `volatile`
- Control flow: `if`, `else`, `switch`, `case`, `default`, `for`, `while`, `do`, `break`, `continue`, `return`, `goto`
- Derived types: `struct`, `union`, `enum`
- Other: `sizeof`, `asm`/`__asm__`

### Literals
- Integer literals (decimal and hexadecimal)
- Character literals with escape sequences (`\n`, `\t`, `\r`, `\\`, `\'`, `\0`)
- String literals with escape sequences

### Operators
- Arithmetic: `+`, `-`, `*`, `/`, `%`
- Bitwise: `&`, `|`, `^`, `~`, `<<`, `>>`
- Logical: `&&`, `||`, `!`
- Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`
- Assignment: `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`
- Increment/Decrement: `++`, `--`
- Member access: `.`, `->`
- Ternary: `? :`
- Comma: `,`

### Comments
- Line comments (`//`)
- Block comments (`/* */`)

## Parser & AST

### Declarations
- Variable declarations with initializers
- Function declarations and definitions
- Parameter declarations (named and unnamed)
- Multiple declarators (`int x, y, z;`)
- Grouped declarators and typedef-array adjustment in function parameters
- Storage class specifiers
- Type definitions

### Statements
- Expression statements
- Compound statements (blocks)
- Selection: `if`/`else`, `switch`/`case`/`default`
- Iteration: `while`, `do-while`, `for`
- Jump: `break`, `continue`, `return`, `goto`
- Labeled statements
- Inline assembly (`asm` with constraints)
- Empty statements

### Expressions
- Primary: identifiers, literals, parenthesized expressions
- Postfix: function calls, array subscripting, member access (`.` and `->`)
- Unary: `++`, `--`, `&`, `*`, `+`, `-`, `~`, `!`, `sizeof`
- Binary: all arithmetic, logical, bitwise, and comparison operators
- Ternary conditional (`? :`)
- Assignment and compound assignment
- Cast expressions
- Compound literals (C99 feature)

### Type System
- Basic types: `void`, `_Bool`, `char`, `short`, `int`, `long` (with signed/unsigned)
- Derived types:
  - Pointers (including fat pointers with bank tags)
  - Arrays (with or without size)
  - Functions (with parameters and variadic support)
  - Structures (named and anonymous)
  - Unions (named and anonymous)
  - Packed integer bit-fields; see [layout and validation](docs/compiler-bit-fields.md)
  - Enums (with explicit values)
- Type qualifiers: `const`, `volatile`
- Typedef support

## Semantic Analysis

### Symbol Resolution
- Function and variable declarations
- Scope management (global, function, block)
- Symbol table with nested scopes
- Forward declarations

### Type Checking
- Expression type inference
- Type compatibility checking
- Implicit conversions (integer promotions, array-to-pointer decay)
- Assignment compatibility
- Function call argument checking
- Return type checking

### Struct/Union Support
- Field offset calculation
- Member access validation
- Anonymous structs/unions
- Nested structures

### Initializers
- Simple expression initializers
- Aggregate initializers for arrays and structs
- Designated initializers (C99)

## Code Generation (IR)

### Memory Model
- Fat pointers with bank tags (Global, Stack, Heap, Unknown, Mixed, Null)
- Automatic bank tracking for pointer operations
- Stack allocation (`alloca`)
- Global variable support

### Control Flow
- Basic blocks
- Conditional and unconditional branches
- Function calls and returns
- Labels and gotos

### Operations
- All arithmetic operations
- Pointer arithmetic with proper scaling
- Array indexing
- Structure member access (via GEP)
- Type casts (integer-to-integer, pointer-to-pointer, integer-to-pointer)

### Optimizations
- SSA form with phi nodes
- Temporary value management
- Dead code elimination (basic)

## Target Architecture Features

### Ripple VM Support
- 16-bit word architecture
- 18 registers (R0, PC, PCB, RA, RAB, R3-R15)
- Full instruction set support
- Bank-aware memory operations
- Runtime library integration

### Assembly Features
- Two-pass assembly
- Label resolution
- Cross-module linking
- Object file format
- Disassembly support

## Standard Library Support (Partial)

### I/O Functions
- `putchar()` - Character output
- `puts()` - String output
- Basic printf formatting (through runtime)

### Memory Functions
- Static allocation
- Stack allocation
- Pointer manipulation

## Special Features

### Inline Assembly
- GCC-style `asm` statements
- Input/output operand constraints
- Clobber lists
- Direct register access

### Compiler Extensions
- `__asm__` keyword
- Bank annotations on pointers
- Fat pointer support for memory safety

## Testing Infrastructure

### Test Runner (rct)
- Automated test execution
- Expected output validation
- Parallel test execution
- Debug mode support
- Build artifact preservation
- Interactive TUI

### Debugging Support
- Source location tracking
- Error messages with line/column info
- IR dumping
- Assembly listing generation
- Verbose compilation modes
