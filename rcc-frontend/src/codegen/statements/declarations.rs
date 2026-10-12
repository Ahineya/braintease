//! Variable declaration code generation

use super::TypedStatementGenerator;
use crate::ir::{IrType, Value};
use crate::typed_ast::{TypedExpr};
use crate::types::{Type, BankTag};
use crate::codegen::{VarInfo, types::complete_type_from_initializer};
use crate::CompilerError;

pub fn generate_declaration(
    gen: &mut TypedStatementGenerator,
    name: &str,
    var_type: &Type,
    initializer: Option<&TypedExpr>,
    symbol_id: Option<rcc_common::SymbolId>,
    storage_class: crate::StorageClass,
) -> Result<(), CompilerError> {
    // A block-scope prototype or typedef declares a name, not a stack object.
    if storage_class == crate::StorageClass::Typedef || matches!(var_type, Type::Function { .. }) {
        return Ok(());
    }
    // Use the general helper to complete incomplete types
    let completed_type = complete_type_from_initializer(var_type, initializer);
    let ir_type = crate::codegen::codegen::global_gen::declaration_ir_type(&completed_type, storage_class)?;

    if matches!(storage_class, crate::StorageClass::Static | crate::StorageClass::Extern) {
        let global_name = if storage_class == crate::StorageClass::Static {
            let id = symbol_id.ok_or_else(|| crate::codegen::CodegenError::InternalError {
                message: format!("Static local '{name}' has no semantic symbol"),
                location: rcc_common::SourceLocation::new_simple(0, 0),
            })?;
            format!("__rcc_static_{id}_{name}")
        } else {
            name.to_string()
        };
        // Bind before evaluating the initializer: C permits self-referential
        // static objects, such as a circular list head containing &head.
        gen.variables.insert(name.to_string(), VarInfo {
            value: Value::Global(global_name.clone()),
            ir_type: ir_type.clone(),
            bank: Some(BankTag::Global),
        });
        if matches!(completed_type, Type::Array { .. }) {
            gen.array_variables.insert(name.to_string());
        } else {
            gen.array_variables.remove(name);
        }
        crate::codegen::codegen::global_gen::generate_global_variable(
            gen.builder, gen.module, gen.variables, gen.array_variables,
            gen.parameter_variables, gen.string_literals, gen.next_string_id,
            &global_name, &completed_type, initializer, storage_class,
        )?;
        return Ok(());
    }
    
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
    } else {
        gen.array_variables.remove(name);
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
