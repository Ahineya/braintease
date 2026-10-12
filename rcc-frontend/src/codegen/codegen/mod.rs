//! Typed code generation from TypedAST to IR
//!
//! This module transforms the typed AST into IR, ensuring proper GEP generation
//! for all pointer arithmetic operations.

mod utils;
mod function_gen;
pub(crate) mod global_gen;
mod constant_float;
mod constant_address;

use std::collections::{HashMap, HashSet};
use crate::ir::{Module, IrBuilder};
use crate::typed_ast::{TypedTranslationUnit, TypedTopLevelItem};
use crate::CompilerError;
use super::VarInfo;

/// Typed code generator - transforms TypedAST to IR
pub struct TypedCodeGenerator {
    module: Module,
    builder: IrBuilder,
    variables: HashMap<String, VarInfo>,
    array_variables: HashSet<String>,
    parameter_variables: HashSet<String>,
    string_literals: HashMap<String, String>,
    next_string_id: u32,
    break_labels: Vec<rcc_common::LabelId>,
    continue_labels: Vec<rcc_common::LabelId>,
}

impl TypedCodeGenerator {
    /// Create a new typed code generator
    pub fn new(module_name: String) -> Self {
        Self {
            module: Module::new(module_name),
            builder: IrBuilder::new(),
            variables: HashMap::new(),
            array_variables: HashSet::new(),
            parameter_variables: HashSet::new(),
            string_literals: HashMap::new(),
            next_string_id: 0,
            break_labels: Vec::new(),
            continue_labels: Vec::new(),
        }
    }
    
    /// Generate IR module from typed AST
    pub fn generate(mut self, ast: &TypedTranslationUnit) -> Result<Module, CompilerError> {
        // Process all top-level items
        for item in &ast.items {
            self.generate_top_level_item(item)?;
        }
        
        Ok(self.module)
    }
    
    /// Generate IR for a top-level item
    fn generate_top_level_item(&mut self, item: &TypedTopLevelItem) -> Result<(), CompilerError> {
        match item {
            TypedTopLevelItem::Function(func) => {
                if func.storage_class == crate::StorageClass::Static {
                    self.module.internal_functions.push(func.name.clone());
                }
                function_gen::generate_function(
                    &mut self.builder,
                    &mut self.module,
                    &mut self.variables,
                    &mut self.array_variables,
                    &mut self.parameter_variables,
                    &mut self.string_literals,
                    &mut self.next_string_id,
                    &mut self.break_labels,
                    &mut self.continue_labels,
                    func,
                )?;
            }
            TypedTopLevelItem::GlobalVariable { name, var_type, initializer, storage_class } => {
                global_gen::generate_global_variable(
                    &mut self.builder,
                    &mut self.module,
                    &mut self.variables,
                    &mut self.array_variables,
                    &self.parameter_variables,
                    &mut self.string_literals,
                    &mut self.next_string_id,
                    name,
                    var_type,
                    initializer.as_ref(),
                    *storage_class,
                )?;
            }
        }
        Ok(())
    }
}
