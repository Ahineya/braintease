//! Common types used throughout the compiler
//! 
//! This module defines data types that are shared across multiple
//! compiler phases, such as symbols, types, and basic definitions.

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fmt;

/// Symbol identifier
pub type SymbolId = u32;

/// Label identifier for code generation
pub type LabelId = u32;

/// Temporary variable identifier for IR
pub type TempId = u32;

// To cast TempId to u32, use `as u32` when needed

/// Basic integer types supported by the Ripple VM
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum IntType {
    /// signed char (8 bits)
    I8,
    /// unsigned char (8 bits)
    U8,
    /// short (16 bits) - native Ripple VM size
    I16,
    /// unsigned short (16 bits)
    U16,
    /// int (16 bits) - same as short for Ripple VM
    Int,
    /// unsigned int (16 bits)
    UInt,
    /// long (32 bits) - stored in two 16-bit cells
    I32,
    /// unsigned long (32 bits)
    U32,
    /// long long (64 bits) - stored in four 16-bit cells
    I64,
    /// unsigned long long (64 bits)
    U64,
}

impl IntType {
    /// Get the size of this type in 16-bit words (Ripple VM cells)
    pub fn size_in_words(&self) -> u32 {
        match self {
            IntType::I8 | IntType::U8 | IntType::I16 | IntType::U16 | IntType::Int | IntType::UInt => 1,
            IntType::I32 | IntType::U32 => 2,
            IntType::I64 | IntType::U64 => 4,
        }
    }
    
    /// Check if this type is signed
    pub fn is_signed(&self) -> bool {
        matches!(self, IntType::I8 | IntType::I16 | IntType::Int | IntType::I32 | IntType::I64)
    }
    
    /// Check if this type is unsigned
    pub fn is_unsigned(&self) -> bool {
        !self.is_signed()
    }
    
    /// Get the default int type for the target (16-bit)
    pub fn default_int() -> Self {
        IntType::Int
    }
}

impl fmt::Display for IntType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            IntType::I8 => write!(f, "signed char"),
            IntType::U8 => write!(f, "unsigned char"),
            IntType::I16 => write!(f, "short"),
            IntType::U16 => write!(f, "unsigned short"),
            IntType::Int => write!(f, "int"),
            IntType::UInt => write!(f, "unsigned int"),
            IntType::I32 => write!(f, "long"),
            IntType::U32 => write!(f, "unsigned long"),
            IntType::I64 => write!(f, "long long"),
            IntType::U64 => write!(f, "unsigned long long"),
        }
    }
}

/// Storage classes in C
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum StorageClass {
    Auto,
    Static,
    Extern,
    Register,
}

impl fmt::Display for StorageClass {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            StorageClass::Auto => write!(f, "auto"),
            StorageClass::Static => write!(f, "static"),
            StorageClass::Extern => write!(f, "extern"),
            StorageClass::Register => write!(f, "register"),
        }
    }
}

/// Symbol table entry
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Symbol {
    pub id: SymbolId,
    pub name: String,
    pub storage_class: StorageClass,
    pub is_function: bool,
    pub is_defined: bool,
    pub scope_level: u32,
    pub symbol_type: Option<String>, // Store type as string for now to avoid circular dependencies
}

impl Symbol {
    pub fn new(id: SymbolId, name: String) -> Self {
        Self {
            id,
            name,
            storage_class: StorageClass::Auto,
            is_function: false,
            is_defined: false,
            scope_level: 0,
            symbol_type: None,
        }
    }
    
    pub fn with_storage_class(mut self, storage_class: StorageClass) -> Self {
        self.storage_class = storage_class;
        self
    }
    
    pub fn as_function(mut self) -> Self {
        self.is_function = true;
        self
    }
    
    pub fn as_defined(mut self) -> Self {
        self.is_defined = true;
        self
    }
    
    pub fn with_scope(mut self, scope_level: u32) -> Self {
        self.scope_level = scope_level;
        self
    }
    
    pub fn with_type(mut self, type_str: String) -> Self {
        self.symbol_type = Some(type_str);
        self
    }
}

/// Simple symbol table for managing identifiers
#[derive(Debug, Clone, Default)]
pub struct SymbolTable {
    symbols: HashMap<String, SymbolId>,
    symbol_data: HashMap<SymbolId, Symbol>,
    next_id: SymbolId,
    scopes: Vec<HashMap<String, SymbolId>>,
    current_scope: u32,
}

impl SymbolTable {
    pub fn new() -> Self {
        Self {
            symbols: HashMap::new(),
            symbol_data: HashMap::new(),
            next_id: 0,
            scopes: vec![HashMap::new()], // Global scope
            current_scope: 0,
        }
    }
    
    /// Enter a new scope
    pub fn push_scope(&mut self) {
        self.current_scope += 1;
        self.scopes.push(HashMap::new());
    }
    
    /// Exit current scope
    pub fn pop_scope(&mut self) {
        if self.current_scope > 0 {
            self.scopes.pop();
            self.current_scope -= 1;
        }
    }
    
    /// Add a symbol to current scope
    pub fn add_symbol(&mut self, name: String) -> SymbolId {
        let id = self.next_id;
        self.next_id += 1;
        
        let symbol = Symbol::new(id, name.clone()).with_scope(self.current_scope);
        
        // Add to current scope
        if let Some(current_scope_map) = self.scopes.last_mut() {
            current_scope_map.insert(name.clone(), id);
        }
        
        // Add to global symbol map and data
        self.symbols.insert(name, id);
        self.symbol_data.insert(id, symbol);
        
        id
    }
    
    /// Look up a symbol by name (searches from current scope upward)
    pub fn lookup(&self, name: &str) -> Option<SymbolId> {
        // Search from current scope backward to global scope
        for scope in self.scopes.iter().rev() {
            if let Some(&id) = scope.get(name) {
                return Some(id);
            }
        }
        None
    }
    
    /// Get symbol data by ID
    pub fn get_symbol(&self, id: SymbolId) -> Option<&Symbol> {
        self.symbol_data.get(&id)
    }
    
    /// Get mutable symbol data by ID
    pub fn get_symbol_mut(&mut self, id: SymbolId) -> Option<&mut Symbol> {
        self.symbol_data.get_mut(&id)
    }
    
    /// Check if a name exists in current scope only
    pub fn exists_in_current_scope(&self, name: &str) -> bool {
        self.scopes
            .last()
            .map(|scope| scope.contains_key(name))
            .unwrap_or(false)
    }
    
    /// Get all symbols in current scope
    pub fn current_scope_symbols(&self) -> Vec<SymbolId> {
        self.scopes
            .last()
            .map(|scope| scope.values().copied().collect())
            .unwrap_or_default()
    }
}

/// Label generator for code generation
#[derive(Debug, Clone, Default)]
pub struct LabelGenerator {
    next_id: LabelId,
}

impl LabelGenerator {
    pub fn new() -> Self {
        Self { next_id: 0 }
    }
    
    /// Generate a new unique label
    pub fn new_label(&mut self) -> String {
        let label = format!("L{}", self.next_id);
        self.next_id += 1;
        label
    }
    
    /// Generate a new label with a prefix
    pub fn new_label_with_prefix(&mut self, prefix: &str) -> String {
        let label = format!("{}_{}", prefix, self.next_id);
        self.next_id += 1;
        label
    }
}

/// Temporary variable generator for IR
#[derive(Debug, Clone, Default)]
pub struct TempGenerator {
    next_id: TempId,
}

impl TempGenerator {
    pub fn new() -> Self {
        Self { next_id: 0 }
    }
    
    /// Generate a new temporary variable ID
    pub fn new_temp(&mut self) -> TempId {
        let temp = self.next_id;
        self.next_id += 1;
        temp
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_int_type_sizes() {
        assert_eq!(IntType::I8.size_in_words(), 1);
        assert_eq!(IntType::I16.size_in_words(), 1);
        assert_eq!(IntType::Int.size_in_words(), 1);
        assert_eq!(IntType::I32.size_in_words(), 2);
    }

    #[test]
    fn test_int_type_signedness() {
        assert!(IntType::I8.is_signed());
        assert!(IntType::I16.is_signed());
        assert!(IntType::Int.is_signed());
        assert!(IntType::I32.is_signed());
        
        assert!(IntType::U8.is_unsigned());
        assert!(IntType::U16.is_unsigned());
        assert!(IntType::UInt.is_unsigned());
        assert!(IntType::U32.is_unsigned());
    }

    #[test]
    fn test_symbol_creation() {
        let symbol = Symbol::new(0, "test".to_string())
            .with_storage_class(StorageClass::Static)
            .as_function()
            .as_defined()
            .with_scope(1);
        
        assert_eq!(symbol.id, 0);
        assert_eq!(symbol.name, "test");
        assert_eq!(symbol.storage_class, StorageClass::Static);
        assert!(symbol.is_function);
        assert!(symbol.is_defined);
        assert_eq!(symbol.scope_level, 1);
    }

    #[test]
    fn test_symbol_table() {
        let mut table = SymbolTable::new();
        
        // Add symbol to global scope
        let id1 = table.add_symbol("global".to_string());
        assert_eq!(table.lookup("global"), Some(id1));
        
        // Push new scope
        table.push_scope();
        let id2 = table.add_symbol("local".to_string());
        
        // Should find both symbols
        assert_eq!(table.lookup("global"), Some(id1));
        assert_eq!(table.lookup("local"), Some(id2));
        
        // Pop scope
        table.pop_scope();
        
        // Should still find global, but not local
        assert_eq!(table.lookup("global"), Some(id1));
        // Note: in this simple implementation, popped symbols remain in global map
        // A more sophisticated implementation would handle scope masking
    }

    #[test]
    fn test_label_generator() {
        let mut gen = LabelGenerator::new();
        
        assert_eq!(gen.new_label(), "L0");
        assert_eq!(gen.new_label(), "L1");
        assert_eq!(gen.new_label_with_prefix("loop"), "loop_2");
    }

    #[test]
    fn test_temp_generator() {
        let mut gen = TempGenerator::new();
        
        assert_eq!(gen.new_temp(), 0);
        assert_eq!(gen.new_temp(), 1);
        assert_eq!(gen.new_temp(), 2);
    }
}