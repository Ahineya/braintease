//! Type system definitions for C99
//! 
//! This module defines the type system used throughout the compiler,
//! including basic types, pointers, arrays, structs, and functions.

use serde::{Deserialize, Serialize};
use std::fmt;

/// Memory bank tag for fat pointers
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum BankTag {
    Global = 0,  // .rodata/.data
    Stack = 1,   // frame/alloca
    Heap = 2,    // Reserved for future heap
    Unknown,     // Parameter or loaded pointer
    Mixed,       // Can be different banks on different paths
    Null,        // NULL pointer - invalid to dereference
}

impl fmt::Display for BankTag {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            BankTag::Global => write!(f, "global"),
            BankTag::Stack => write!(f, "stack"),
            BankTag::Heap => write!(f, "heap"),
            BankTag::Unknown => write!(f, "unknown"),
            BankTag::Mixed => write!(f, "mixed"),
            BankTag::Null => write!(f, "null"),
        }
    }
}

/// C99 type system
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum Type {
    /// Void type
    Void,
    
    /// Boolean type (_Bool)
    Bool,
    
    /// Character types
    Char,
    SignedChar,
    UnsignedChar,
    
    /// Integer types
    Short,
    UnsignedShort,
    Int,
    UnsignedInt,
    Long,
    UnsignedLong,
    
    /// Pointer to another type with optional bank information
    Pointer {
        target: Box<Type>,
        bank: Option<BankTag>,
    },
    
    /// Array type with optional size
    Array {
        element_type: Box<Type>,
        size: Option<u64>,
    },
    
    /// Function type
    Function {
        return_type: Box<Type>,
        parameters: Vec<Type>,
        is_variadic: bool,
    },
    
    /// Struct type (simplified - no bitfields in MVP)
    Struct {
        name: Option<String>,
        fields: Vec<StructField>,
    },
    
    /// Union type
    Union {
        name: Option<String>,
        fields: Vec<StructField>,
    },
    
    /// Enum type
    Enum {
        name: Option<String>,
        variants: Vec<EnumVariant>,
    },
    
    /// Typedef alias
    Typedef(String),
    
    /// Placeholder for type resolution errors
    Error,
}

impl Type {
    /// Get the size of this type in Ripple VM words (16-bit cells)
    pub fn size_in_words(&self) -> Option<u64> {
        match self {
            Type::Void => None,
            Type::Bool | Type::Char | Type::SignedChar | Type::UnsignedChar => Some(1),
            Type::Short | Type::UnsignedShort => Some(1),
            Type::Int | Type::UnsignedInt => Some(1), // 16-bit int on Ripple
            Type::Long | Type::UnsignedLong => Some(2),
            Type::Pointer { .. } => Some(2), // Fat pointers: 1 word address + 1 word bank
            Type::Array { element_type, size: Some(count) } => {
                element_type.size_in_words().map(|elem_size| elem_size * count)
            }
            Type::Array { size: None, .. } => None, // Incomplete type
            Type::Function { .. } => None, // Functions don't have size
            Type::Struct { fields, .. } => {
                // By the time we call size_in_words, all struct fields should be fully resolved
                // by the semantic analyzer
                let mut total = 0;
                for field in fields {
                    total += field.field_type.size_in_words()?;
                }
                Some(total)
            }
            Type::Union { fields, .. } => {
                fields.iter()
                    .filter_map(|f| f.field_type.size_in_words())
                    .max()
            }
            Type::Enum { .. } => Some(2), // Enum is like int
            Type::Typedef(_) | Type::Error => None,
        }
    }
    
    /// Get pointer target type
    pub fn pointer_target(&self) -> Option<&Type> {
        match self {
            Type::Pointer { target, .. } => Some(target),
            Type::Array { element_type, .. } => Some(element_type),
            _ => None,
        }
    }
}

impl fmt::Display for Type {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Type::Void => write!(f, "void"),
            Type::Bool => write!(f, "_Bool"),
            Type::Char => write!(f, "char"),
            Type::SignedChar => write!(f, "signed char"),
            Type::UnsignedChar => write!(f, "unsigned char"),
            Type::Short => write!(f, "short"),
            Type::UnsignedShort => write!(f, "unsigned short"),
            Type::Int => write!(f, "int"),
            Type::UnsignedInt => write!(f, "unsigned int"),
            Type::Long => write!(f, "long"),
            Type::UnsignedLong => write!(f, "unsigned long"),
            Type::Pointer { target, bank } => {
                write!(f, "{target}*")?;
                if let Some(bank) = bank {
                    write!(f, "@{bank}")?;
                }
                Ok(())
            },
            Type::Array { element_type, size: Some(n) } => write!(f, "{element_type}[{n}]"),
            Type::Array { element_type, size: None } => write!(f, "{element_type}[]"),
            Type::Function { return_type, parameters, is_variadic } => {
                write!(f, "{return_type} (")?;
                for (i, param) in parameters.iter().enumerate() {
                    if i > 0 { write!(f, ", ")?; }
                    write!(f, "{param}")?;
                }
                if *is_variadic { write!(f, ", ...")?; }
                write!(f, ")")
            }
            Type::Struct { name: Some(name), .. } => write!(f, "struct {name}"),
            Type::Struct { name: None, .. } => write!(f, "struct <anonymous>"),
            Type::Union { name: Some(name), .. } => write!(f, "union {name}"),
            Type::Union { name: None, .. } => write!(f, "union <anonymous>"),
            Type::Enum { name: Some(name), .. } => write!(f, "enum {name}"),
            Type::Enum { name: None, .. } => write!(f, "enum <anonymous>"),
            Type::Typedef(name) => write!(f, "{name}"),
            Type::Error => write!(f, "<error>"),
        }
    }
}

/// Struct/union field
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct StructField {
    pub name: String,
    pub field_type: Type,
    pub offset: Option<u64>, // Computed during semantic analysis
}

/// Enum variant
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct EnumVariant {
    pub name: String,
    pub value: i64,
}

/// Storage class specifier
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum StorageClass {
    Auto,
    Static,
    Extern,
    Register,
    Typedef,
}

impl fmt::Display for StorageClass {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let class_str = match self {
            StorageClass::Auto => "auto",
            StorageClass::Static => "static",
            StorageClass::Extern => "extern",
            StorageClass::Register => "register",
            StorageClass::Typedef => "typedef",
        };
        write!(f, "{class_str}")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_type_sizes() {
        assert_eq!(Type::Char.size_in_words(), Some(1));
        assert_eq!(Type::Int.size_in_words(), Some(1)); // 16-bit int = 1 word
        assert_eq!(Type::Long.size_in_words(), Some(2)); // 32-bit long = 2 words
        assert_eq!(Type::Pointer { target: Box::new(Type::Int), bank: None }.size_in_words(), Some(2)); // Fat pointer = 2 words
        
        let array_type = Type::Array { 
            element_type: Box::new(Type::Int), 
            size: Some(10) 
        };
        assert_eq!(array_type.size_in_words(), Some(10)); // 10 * 1 word
    }

    #[test]
    fn test_type_properties() {
        assert!(Type::Int.is_integer());
        assert!(Type::Int.is_signed_integer());
        assert!(!Type::UnsignedInt.is_signed_integer());
        assert!(Type::Pointer { target: Box::new(Type::Int), bank: None }.is_pointer());
        assert!(!Type::Int.is_pointer());
    }

    #[test]
    fn test_type_compatibility() {
        let int_type = Type::Int;
        let uint_type = Type::UnsignedInt;
        let int_ptr = Type::Pointer { target: Box::new(Type::Int), bank: None };
        let void_ptr = Type::Pointer { target: Box::new(Type::Void), bank: None };
        
        // Integer types are compatible
        assert!(int_type.is_assignable_from(&uint_type));
        
        // void* is compatible with any pointer
        assert!(void_ptr.is_assignable_from(&int_ptr));
        assert!(int_ptr.is_assignable_from(&void_ptr));
        
        // Different types are not compatible
        assert!(!int_type.is_assignable_from(&int_ptr));
    }

    #[test]
    fn test_type_display() {
        assert_eq!(format!("{}", Type::Int), "int");
        assert_eq!(format!("{}", Type::Pointer { target: Box::new(Type::Char), bank: None }), "char*");
        assert_eq!(format!("{}", Type::Pointer { target: Box::new(Type::Int), bank: Some(BankTag::Stack) }), "int*@stack");
        assert_eq!(format!("{}", Type::Array { 
            element_type: Box::new(Type::Int), 
            size: Some(10) 
        }), "int[10]");
    }
}