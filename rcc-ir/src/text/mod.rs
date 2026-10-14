//! Lossless textual form of an IR module.
//!
//! `emit_module` prints every field the in-memory module stores. `parse_module`
//! reads that text back into an equal `Module`. Names that are not plain
//! identifiers, and every string that can contain spaces or punctuation, are
//! quoted. `;` starts a comment that runs to the end of the line. A quoted
//! string is never a comment.
//!
//! Top-level items may appear in any order. Emitting a module prints type
//! definitions (sorted by name), then internal functions, globals, and
//! functions, which is the order stored in [`Module`](crate::ir::Module).
//!
//! ```text
//! module demo {
//!   type Point = struct Point { i16, i16 }
//!   internal function @hidden
//!   @g = external constant i16 = 7 symbol 3
//!   @e = declaration i16*
//!   function i16 @main(i16 %0) symbol 0 loc demo.c 4 1 {
//!     block 0 loop preds 2, 0 succs 1:
//!       %1 = add i16 %0, -1
//!       ret %1
//!     block 1 preds 0:
//!       br label 0
//!   }
//! }
//! ```
//!
//! Values distinguish `global @name` from `func @name`. Fat pointers are
//! `fat(value, bank)`. Instructions occupy one line each; delimiters `()`,
//! `[]`, and `{}` may contain newlines.

mod parse;
mod print;

#[cfg(test)]
mod tests;

pub use parse::parse_module;
pub use print::emit_module;

use crate::ir::Module;
use std::fmt;

/// A located syntax error in textual IR.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IrTextError {
    pub line: u32,
    pub column: u32,
    pub message: String,
}

impl fmt::Display for IrTextError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}: {}", self.line, self.column, self.message)
    }
}

impl std::error::Error for IrTextError {}

impl fmt::Display for Module {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&emit_module(self))
    }
}
