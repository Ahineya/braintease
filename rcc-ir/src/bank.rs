//! Memory bank tag for fat pointers.

use serde::{Deserialize, Serialize};
use std::fmt;

/// Memory bank tag for fat pointers.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum BankTag {
    Global,      // .rodata/.data (bank 1)
    Stack,       // frame/alloca (bank 2)
    Heap(u16),   // Heap with specific bank number (banks 3+)
    Unknown,     // Parameter or loaded pointer
    Mixed,       // Can be different banks on different paths
    Null,        // NULL pointer - invalid to dereference
}

impl fmt::Display for BankTag {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            BankTag::Global => write!(f, "global"),
            BankTag::Stack => write!(f, "stack"),
            BankTag::Heap(bank) => write!(f, "heap@{}", bank),
            BankTag::Unknown => write!(f, "unknown"),
            BankTag::Mixed => write!(f, "mixed"),
            BankTag::Null => write!(f, "null"),
        }
    }
}
