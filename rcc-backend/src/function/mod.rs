//! Function generation module for V2 backend
//! 
//! This module provides a safe, high-level API for generating complete functions.
//! All internal complexity is hidden behind the `FunctionBuilder` interface.
//! 
//! ## Architecture
//! 
//! - `builder.rs` - Public safe API (FunctionBuilder)
//! - `lowering.rs` - Internal function lowering logic (NOT exposed)
//! - `calling_convention.rs` - Internal calling convention (NOT exposed)
//! 
//! Only the builder API is exposed to prevent misuse of internal components.

// Internal modules - implementation details
mod internal;  // Internal implementation for FunctionBuilder (was lowering.rs)
mod calling_convention;  // Calling convention logic (private to this module)
mod builder;  // Public API for building functions
pub(crate) mod frame;

// Public exports - ONLY the safe API
pub use builder::{FunctionBuilder, CallArg, CallTarget};  // All function-related operations go through FunctionBuilder

// Note: CallingConvention is now completely encapsulated within FunctionBuilder.
// All function-related operations (building functions, making calls) must go through FunctionBuilder.

// Tests
#[cfg(test)]
mod tests;
