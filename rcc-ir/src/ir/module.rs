//! Module and Global Variables
//! 
//! Defines the top-level module structure and global variable management.

use rcc_common::SymbolId;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use crate::ir::{Function, IrType, Value};

/// Linkage types for global symbols
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum Linkage {
    External,  // Visible to other modules
    Internal,  // Only visible within this module (static)
    Private,   // Not visible outside this function
    Declaration, // extern declaration without storage
}

/// Global variable definition
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct GlobalVariable {
    pub name: String,
    pub var_type: IrType,
    pub is_constant: bool,
    pub initializer: Option<Value>,
    pub linkage: Linkage,
    pub symbol_id: Option<SymbolId>,
}

/// IR Module - represents a complete compilation unit
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Module {
    #[serde(default)]
    pub target: crate::target::Target,
    pub name: String,
    pub functions: Vec<Function>,
    pub globals: Vec<GlobalVariable>,
    #[serde(default)]
    pub internal_functions: Vec<String>,
    pub type_definitions: HashMap<String, IrType>,
}

impl Module {
    pub fn new(name: String) -> Self {
        Self {
            target: crate::target::Target::Ripple,
            name,
            functions: Vec::new(),
            globals: Vec::new(),
            internal_functions: Vec::new(),
            type_definitions: HashMap::new(),
        }
    }
    
    pub fn add_function(&mut self, function: Function) {
        self.functions.push(function);
    }
    
    pub fn add_global(&mut self, global: GlobalVariable) {
        self.globals.push(global);
    }
    
    pub fn get_function(&self, name: &str) -> Option<&Function> {
        self.functions.iter().find(|f| f.name == name)
    }
    
    pub fn get_function_mut(&mut self, name: &str) -> Option<&mut Function> {
        self.functions.iter_mut().find(|f| f.name == name)
    }
    
    pub fn get_global(&self, name: &str) -> Option<&GlobalVariable> {
        self.globals.iter().find(|g| g.name == name)
    }
}