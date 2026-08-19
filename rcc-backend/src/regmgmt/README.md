# Register Management Module

> Stack bank is **SB (R28) = 2**, not R13. R13 is X2. Current ABI: [`docs/arch/`](../../../docs/arch/README.md).

## Overview

This module provides the encapsulated register management system for the V2 backend. It ensures safe, automatic handling of register allocation, spilling, and bank management.

## Architecture

```
regmgmt/
├── mod.rs           # Public API exports
├── pressure.rs      # RegisterPressureManager (main public interface)
├── allocator.rs     # RegAllocV2 (internal, not exposed except for tests)
└── bank.rs          # BankInfo enum for pointer bank management
```

## Key Features

### Automatic R13 Initialization
- R13 (stack bank register) is automatically initialized to 1 when needed
- No manual flag management required
- Prevents common bugs from forgetting initialization

### LRU Spilling
- Implements Least Recently Used spilling policy
- Automatic spill/reload generation
- Optimal register usage with Sethi-Ullman ordering

### Bank Management
- Tracks bank information for all pointers
- Supports Global (bank 0), Stack (bank 1), and Dynamic banks
- Ensures correct bank register usage in memory operations

## Public API

### RegisterPressureManager

The main interface for register management:

```rust
// Core allocation
pub fn new(local_count: i16) -> Self
pub fn init(&mut self)
pub fn get_register(&mut self, for_value: String) -> Reg
pub fn free_register(&mut self, reg: Reg)
pub fn take_instructions(&mut self) -> Vec<AsmInst>

// Spilling
pub fn spill_all(&mut self)
pub fn reload_value(&mut self, value: String) -> Reg
pub fn get_spill_count(&self) -> usize

// Special operations
pub fn load_parameter(&mut self, param_idx: usize) -> Reg
pub fn set_pointer_bank(&mut self, ptr_value: String, bank: BankInfo)

// Binary operations (with Sethi-Ullman ordering)
pub fn emit_binary_op(&mut self, op: IrBinaryOp, lhs: &Value, rhs: &Value, result_temp: TempId) -> Vec<AsmInst>

// Lifetime analysis (for optimization)
pub fn analyze_block(&mut self, block: &BasicBlock)
```

### BankInfo

Represents bank information for pointers:

```rust
pub enum BankInfo {
    Global,           // Bank 0 - use R0
    Stack,            // Bank 1 - use R13 (auto-initialized)
    Register(Reg),    // Dynamic bank in a register
}
```

## Usage Example

```rust
use crate::regmgmt::{RegisterPressureManager, BankInfo};

// Create manager with 10 local variables
let mut manager = RegisterPressureManager::new(10);
manager.init();  // Initializes R13 automatically

// Allocate registers
let r1 = manager.get_register("temp1".to_string());
let r2 = manager.get_register("temp2".to_string());

// Set pointer bank info
manager.set_pointer_bank("ptr1".to_string(), BankInfo::Stack);

// Spill all registers before a call
manager.spill_all();

// Take generated instructions
let instructions = manager.take_instructions();
```

## Safety Invariants

1. **Sb Initialization**: Stack Bank always initialized before any stack operation
2. **Register Consistency**: Register contents always match internal tracking
3. **Spill Slot Management**: Each value has at most one spill slot
4. **Bank Tracking**: All pointers have associated bank information
5. **LRU Ordering**: Most recently used registers are kept in registers

## Implementation Notes

- RegAllocV2 is completely encapsulated and not exposed in the public API
- All register management must go through RegisterPressureManager
- The module design prevents direct manipulation of internal state