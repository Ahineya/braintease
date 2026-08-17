//! Global variable code generation

use std::collections::{HashMap, HashSet};
use crate::ir::{Value, IrBuilder, Module, GlobalVariable, Linkage};
use crate::typed_ast::TypedExpr;
use crate::types::{Type, BankTag};
use crate::CompilerError;
use crate::codegen::CodegenError;
use super::super::{VarInfo, expressions::TypedExpressionGenerator, types::complete_type_from_initializer};
use super::utils::convert_type_default;

/// Generate IR for a global variable
pub fn generate_global_variable(
    builder: &mut IrBuilder,
    module: &mut Module,
    variables: &mut HashMap<String, VarInfo>,
    array_variables: &mut HashSet<String>,
    parameter_variables: &HashSet<String>,
    string_literals: &mut HashMap<String, String>,
    next_string_id: &mut u32,
    name: &str,
    var_type: &Type,
    initializer: Option<&TypedExpr>,
) -> Result<(), CompilerError> {
    // Handle initializer first to determine array size if needed
    let init_value = if let Some(init_expr) = initializer {
        // We need to generate the initializer value
        // For global variables, only constant initializers are allowed
        let mut expr_gen = TypedExpressionGenerator {
            builder,
            module,
            variables,
            array_variables,
            parameter_variables,
            string_literals,
            next_string_id,
        };
        
        match expr_gen.generate(init_expr) {
            Ok(value) => match value {
                Value::Constant(_) | Value::ConstantArray(_) => Some(value),
                Value::FatPtr(ref fp) => {
                    // Allow FatPtr for pointer initializers (e.g., string literals)
                    // But only if the address is a constant or global
                    match fp.addr.as_ref() {
                        Value::Global(_) | Value::Constant(_) => Some(value),
                        _ => {
                            return Err(CodegenError::UnsupportedConstruct {
                                construct: format!("Non-constant pointer initializer for global variable '{}'", name),
                                location: rcc_common::SourceLocation::new_simple(0, 0),
                            }.into());
                        }
                    }
                }
                Value::Global(_) => Some(value), // Allow references to other globals
                _ => {
                    return Err(CodegenError::UnsupportedConstruct {
                        construct: format!("Non-constant initializer for global variable '{}': {:?}", name, value),
                        location: rcc_common::SourceLocation::new_simple(0, 0),
                    }.into());
                }
            }
            Err(e) => return Err(e),
        }
    } else {
        None
    };
    
    // Use the general helper to complete incomplete types
    let completed_type = complete_type_from_initializer(var_type, initializer);
    let ir_type = convert_type_default(&completed_type)?;
    
    // `extern T x;` plus a later tentative definition or definition in the same
    // TU must not emit two globals. Merge into the existing one.
    if let Some(existing) = module.globals.iter_mut().find(|g| g.name == name) {
        if let Some(init) = init_value {
            if existing.initializer.is_some() {
                return Err(CodegenError::InternalError {
                    message: format!("Redefinition of global variable '{name}'"),
                    location: rcc_common::SourceLocation::new_simple(0, 0),
                }.into());
            }
            existing.initializer = Some(init);
        }
        if !variables.contains_key(name) {
            variables.insert(name.to_string(), VarInfo {
                value: Value::Global(name.to_string()),
                ir_type,
                bank: Some(BankTag::Global),
            });
        }
        if matches!(var_type, Type::Array { .. }) {
            array_variables.insert(name.to_string());
        }
        return Ok(());
    }
    
    let global = GlobalVariable {
        name: name.to_string(),
        var_type: ir_type.clone(),
        is_constant: false,
        initializer: init_value,
        linkage: Linkage::External,
        symbol_id: None,
    };
    
    module.add_global(global);
    
    // Add to variables map for later reference
    variables.insert(name.to_string(), VarInfo {
        value: Value::Global(name.to_string()),
        ir_type,
        bank: Some(BankTag::Global),
    });
    
    // Track global arrays
    if matches!(var_type, Type::Array { .. }) {
        array_variables.insert(name.to_string());
    }
    
    Ok(())
}