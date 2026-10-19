//! Intermediate representation shared by language frontends and the Ripple backend.
//!
//! The C frontend lowers into this module. The backend lowers it to assembly.
//! `BankTag` lives here because fat pointers carry it through both sides.

mod bank;
pub mod float;
pub mod ir;
pub mod link;
pub mod native_image;
pub mod target;
mod text;

pub use bank::BankTag;
pub use text::{emit_module, parse_module, IrTextError};
