//! Variable declaration code generation

use super::TypedStatementGenerator;
use crate::ir::{IrType, Value};
use crate::typed_ast::{TypedExpr};
use crate::types::{Type, BankTag};
use crate::codegen::{VarInfo, types::{convert_type, complete_type_from_initializer}};
use crate::CompilerError;

// Helper function for convert_type with default location
fn convert_type_default(ast_type: &Type) -> Result<IrType, CompilerError> {
    convert_type(ast_type, rcc_common::SourceLocation::new_simple(0, 0))
}

pub fn generate_declaration(
    gen: &mut TypedStatementGenerator,
    name: &str,
    var_type: &Type,
    initializer: Option<&TypedExpr>,
) -> Result<(), CompilerError> {
    // Use the general helper to complete incomplete types
    let completed_type = complete_type_from_initializer(var_type, initializer);
    let ir_type = convert_type_default(&completed_type)?;
    
    // Allocate stack space for the variable
    // For arrays, we need to pass the array size as the count
    let var_addr = match &ir_type {
        IrType::Array { size, element_type } => {
            // For arrays, allocate space for all elements
            // Pass the element type and the count
            gen.builder.build_alloca(element_type.as_ref().clone(), Some(Value::Constant(*size as i64)))?
        }
        _ => {
            // For non-arrays, allocate a single element
            gen.builder.build_alloca(ir_type.clone(), None)?
        }
    };
    
    // Track the variable.
    // C arrays decay to pointers. Unions also lower to IrType::Array (largest
    // field, in words) but they are objects: loading them as i16* interprets
    // the first stored word as an address (e.g. 'A' = 65).
    let tracked_type = if matches!(completed_type, Type::Array { .. }) {
        match &ir_type {
            IrType::Array { element_type, .. } => IrType::FatPtr(element_type.clone()),
            _ => ir_type.clone(),
        }
    } else {
        ir_type.clone()
    };
    
    let var_info = VarInfo {
        value: var_addr.clone(),
        ir_type: tracked_type,
        bank: Some(BankTag::Stack),
    };
    gen.variables.insert(name.to_string(), var_info);
    
    // Handle array variables specially
    if matches!(var_type, Type::Array { .. }) {
        gen.array_variables.insert(name.to_string());
    }
    
    // Initialize if needed
    if let Some(init_expr) = initializer {
        let mut expr_gen = gen.create_expression_generator();
        crate::codegen::expressions::store_initializer(
            &mut expr_gen,
            var_addr,
            &completed_type,
            init_expr,
        )?;
    }
    
    Ok(())
}