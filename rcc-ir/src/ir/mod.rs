//! Full Intermediate Representation for C99
//! 
//! This module defines a comprehensive IR that can represent all C99 constructs.
//! It's designed to be lowered from AST and then compiled to assembly.
//! 
//! ## Architecture
//! 
//! The module is structured as follows:
//! - `types` - Type system (IrType, BankTag, FatPointer)
//! - `values` - Value representations
//! - `ops` - Binary and unary operations
//! - `instructions` - IR instructions
//! - `blocks` - Basic block management
//! - `function` - Function definitions
//! - `module` - Module and global variables
//! - `builder` - IR construction utilities

// Public exports - clean API surface
pub use self::types::{IrType, FatPointer};
pub use self::values::Value;
pub use self::ops::{IrBinaryOp, IrUnaryOp};
pub use self::instructions::{Instruction, AsmOperandIR};
pub use self::blocks::BasicBlock;
pub use self::function::Function;
pub use self::module::{Module, GlobalVariable, Linkage};
pub use self::builder::IrBuilder;

// Internal modules
mod types;
mod values;
mod ops;
mod instructions;
mod blocks;
mod function;
mod module;
mod builder;

#[cfg(test)]
mod tests;