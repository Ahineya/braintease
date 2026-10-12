//! Tests for V2 instruction lowering
//! 
//! This module contains unit tests for individual instruction types
//! and integration tests for instruction combinations.

#[cfg(test)]
mod load_tests;

#[cfg(test)]
mod store_tests;

#[cfg(test)]
mod gep_tests;

#[cfg(test)]
mod gep_integration_tests;

#[cfg(test)]
mod binary_tests;

#[cfg(test)]
mod unary_tests;

#[cfg(test)]
mod branch_tests;

// Future test modules as instructions are implemented:
// mod icmp_tests;
// mod call_tests;
// mod return_tests;
// mod alloca_tests;

// Integration tests for instruction combinations
#[cfg(test)]
mod integration_tests;

pub(crate) mod machine;
