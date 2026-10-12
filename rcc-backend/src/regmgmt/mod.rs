//! Register Management Module for V2 Backend
//! 
//! This module provides a clean, encapsulated API for register management
//! in the V2 backend. It ensures proper R13 initialization, handles spilling
//! and reloading, and manages bank information for pointers.
//! 
//! ## Architecture
//! 
//! The module is structured as follows:
//! - `RegisterPressureManager` - Public API for register management
//! - `RegAllocV2` - Internal allocator (not exposed)
//! - `BankInfo` - Bank information for pointers
//! 
//! ## Safety Invariants
//! 
//! - R13 is automatically initialized when needed for stack operations
//! - Register allocation follows LRU spilling policy
//! - Bank information is tracked for all pointers
//! - Spill slots are managed automatically

// Public exports - only what consumers need
pub use self::pressure::RegisterPressureManager;
pub use self::bank::{BankInfo, BankTagValue};

// Keep internal modules private
mod allocator;
pub mod pressure;
mod bank;
pub(crate) mod liveness;
