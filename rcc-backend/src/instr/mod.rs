//! Instruction lowering modules for V2 backend
//! 
//! This module contains the implementation for lowering individual IR instructions
//! to Ripple assembly instructions.

pub mod helpers;
pub mod load;
pub mod store;
pub mod gep;
pub mod binary;
pub mod unary;
pub mod branch;
pub mod inline_asm;
pub mod wide;
pub mod i64;

pub use helpers::get_value_register;
pub use load::lower_load;
pub use store::lower_store;
pub use gep::lower_gep;
pub use binary::{lower_binary_op, lower_binary_op_immediate};
pub use unary::lower_unary_op;
pub use branch::{lower_branch, lower_branch_cond, lower_compare_and_branch, ComparisonType};
pub use inline_asm::{lower_inline_asm_basic, lower_inline_asm_extended};

// Test modules
#[cfg(test)]
mod tests;
