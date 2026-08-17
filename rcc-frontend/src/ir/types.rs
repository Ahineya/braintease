//! IR Type System
//! 
//! Defines the type system for the IR, including integer types,
//! pointers with bank tags, arrays, structs, and functions.

use serde::{Deserialize, Serialize};
use std::fmt;
use crate::BankTag;

/// Fat pointer representation - carries both address and bank
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct FatPointer {
    pub addr: Box<crate::ir::Value>,  // Address value
    pub bank: BankTag,                // Bank tag
}

/// IR Type system
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum IrType {
    /// Void type
    Void,
    
    /// Integer types with bit width
    I1,   // Boolean
    I8,   // 8-bit integer (char)
    I16,  // 16-bit integer (short, int on Ripple)
    I32,  // 32-bit integer (long on Ripple)
    I64,  // 64-bit integer (not supported on Ripple in MVP)
    
    /// Pointer type
    FatPtr(Box<IrType>),
    
    /// Array type [size x element_type]
    Array { size: u64, element_type: Box<IrType> },
    
    /// Function type
    Function {
        return_type: Box<IrType>,
        param_types: Vec<IrType>,
        is_vararg: bool,
    },
    
    /// Struct type
    Struct {
        name: Option<String>,
        fields: Vec<IrType>,
        packed: bool,
    },
    
    /// Label type (for basic block addresses)
    Label,
}

impl IrType {
    /// Get the size of this type in bytes
    pub fn size_in_words(&self) -> Option<u64> {
        match self {
            IrType::Void => None,
            IrType::I1 => Some(1), // Stored in full byte
            IrType::I8 => Some(1),
            IrType::I16 => Some(1),
            IrType::I32 => Some(2),
            IrType::I64 => Some(4),
            IrType::FatPtr(_) => Some(2), // Fat pointers: 2 words (address + bank tag)
            IrType::Array { size, element_type } => {
                element_type.size_in_words().map(|elem_size| elem_size * size)
            }
            IrType::Function { .. } => None, // Functions don't have size
            IrType::Struct { fields, .. } => {
                let mut total = 0;
                for field in fields {
                    total += field.size_in_words()?;
                }
                Some(total)
            }
            IrType::Label => None,
        }
    }
    
    /// Check if this is an integer type
    pub fn is_integer(&self) -> bool {
        matches!(self, IrType::I1 | IrType::I8 | IrType::I16 | IrType::I32 | IrType::I64)
    }
    
    /// Check if this is a pointer type
    pub fn is_pointer(&self) -> bool {
        matches!(self, IrType::FatPtr(_))
    }

    /// 32-bit integer: two 16-bit words (Ripple `long` / `unsigned long`)
    pub fn is_wide(&self) -> bool {
        matches!(self, IrType::I32)
    }

    /// Values that occupy two ABI slots / two memory words (fat pointers and I32)
    pub fn takes_two_slots(&self) -> bool {
        self.is_pointer() || self.is_wide()
    }
    
    /// Get the element type for pointers and arrays
    pub fn element_type(&self) -> Option<&IrType> {
        match self {
            IrType::FatPtr(elem) => Some(elem),
            IrType::Array { element_type, .. } => Some(element_type),
            _ => None,
        }
    }
}

impl fmt::Display for IrType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            IrType::Void => write!(f, "void"),
            IrType::I1 => write!(f, "i1"),
            IrType::I8 => write!(f, "i8"),
            IrType::I16 => write!(f, "i16"),
            IrType::I32 => write!(f, "i32"),
            IrType::I64 => write!(f, "i64"),
            IrType::FatPtr(target) => write!(f, "{target}*"),
            IrType::Array { size, element_type } => write!(f, "[{size} x {element_type}]"),
            IrType::Function { return_type, param_types, is_vararg } => {
                write!(f, "{return_type} (")?;
                for (i, param) in param_types.iter().enumerate() {
                    if i > 0 { write!(f, ", ")?; }
                    write!(f, "{param}")?;
                }
                if *is_vararg { write!(f, ", ...")?; }
                write!(f, ")")
            }
            IrType::Struct { name: Some(name), .. } => write!(f, "%{name}"),
            IrType::Struct { name: None, .. } => write!(f, "%struct"),
            IrType::Label => write!(f, "label"),
        }
    }
}