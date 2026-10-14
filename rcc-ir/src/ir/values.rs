//! IR Value Representations
//! 
//! Defines values that can be used as operands in IR instructions,
//! including temporaries, constants, globals, and fat pointers.

use rcc_common::TempId;
use serde::{Deserialize, Serialize};
use std::fmt;
use crate::ir::types::{FatPointer};

/// IR Value - represents operands in IR instructions
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum Value {
    /// Temporary variable
    Temp(TempId),
    
    /// Constant integer
    Constant(i64),
    
    /// Global symbol reference
    Global(String),

    /// Static pointer initializer displacement, in VM cells. The base is a
    /// global symbol or a constant offset; the enclosing FatPtr supplies its
    /// bank. Keep the symbol separate so the linker can relocate it normally.
    AddressOffset { base: Box<Value>, cells: i64 },
    
    /// Function reference
    Function(String),
    
    /// Fat pointer (address + bank)
    FatPtr(FatPointer),
    
    /// Array of constant values (for initializers)
    ConstantArray(Vec<i64>),

    /// Typed aggregate initializer; preserves pointers and multiword scalars.
    ConstantAggregate(Vec<Value>),
    
    /// Undefined value (for uninitialized variables)
    Undef,
}

impl fmt::Display for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Value::Temp(id) => write!(f, "%{id}"),
            Value::Constant(val) => write!(f, "{val}"),
            Value::Global(name) => write!(f, "@{name}"),
            Value::AddressOffset { base, cells } => write!(f, "address({base}, {cells:+} cells)"),
            Value::Function(name) => write!(f, "@{name}"),
            Value::FatPtr(ptr) => write!(f, "{{addr: {}, bank: {:?}}}", ptr.addr, ptr.bank),
            Value::ConstantAggregate(values) => {
                write!(f, "{{")?;
                for (i, value) in values.iter().enumerate() {
                    if i > 0 { write!(f, ", ")?; }
                    write!(f, "{value}")?;
                }
                write!(f, "}}")
            }
            Value::ConstantArray(values) => {
                write!(f, "[")?;
                for (i, val) in values.iter().enumerate() {
                    if i > 0 { write!(f, ", ")?; }
                    write!(f, "{val}")?;
                }
                write!(f, "]")
            }
            Value::Undef => write!(f, "undef"),
        }
    }
}
