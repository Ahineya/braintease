//! V2 Lowering Module - Integrates All Lowering Components
//! 
//! This module provides the main entry point for lowering IR to assembly using
//! the V2 backend. It organizes the lowering logic into separate modules for
//! better maintainability.

pub mod module;
pub(crate) mod function;
mod instruction;
