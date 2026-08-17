//! Function call code generation

use super::{TypedExpressionGenerator, convert_type_default};
use super::assignments::copy_struct;
use crate::ir::{Value, FatPointer};
use crate::typed_ast::TypedExpr;
use crate::types::{Type, BankTag};
use crate::codegen::CodegenError;
use crate::CompilerError;

pub fn generate_function_call(
    gen: &mut TypedExpressionGenerator,
    function: &TypedExpr,
    arguments: &[TypedExpr],
    return_type: &Type,
) -> Result<Value, CompilerError> {
    // For function calls, we need the function name directly, not its loaded value
    let func_val = match function {
        TypedExpr::Variable { name, .. } => {
            // Check if it's a known variable (function pointer) or a direct function name
            if gen.variables.contains_key(name) {
                // It's a function pointer variable, load it
                gen.generate(function)?
            } else {
                // It's a direct function name
                Value::Global(name.to_string())
            }
        }
        _ => {
            // For other expressions (like function pointers), generate normally
            gen.generate(function)?
        }
    };
    
    let mut arg_vals = Vec::new();
    for arg in arguments {
        let val = gen.generate(arg)?;
        let arg_type = arg.get_type();
        if matches!(arg_type, Type::Struct { .. } | Type::Union { .. }) {
            // Materialize into a dedicated object. Passing a GEP into a larger
            // aggregate (e.g. q_add(p.x, p.vx)) hangs in the backend.
            let ir_type = convert_type_default(&arg_type)?;
            let dest = gen.builder.build_alloca(ir_type, None)
                .map_err(|e| CodegenError::InternalError {
                    message: format!("Failed to allocate temp for aggregate argument: {e}"),
                    location: rcc_common::SourceLocation::new_simple(0, 0),
                })?;
            copy_struct(gen, val, dest.clone(), &arg_type)?;
            arg_vals.push(dest);
        } else {
            arg_vals.push(val);
        }
    }
    
    // Get the proper return type
    let ir_return_type = convert_type_default(return_type)?;
    let result = gen.builder.build_call(func_val, arg_vals, ir_return_type.clone())?;
    
    // Handle the return value based on type
    match result {
        Some(temp_id) => {
            // If the return type is a pointer, wrap it in a FatPointer with Mixed bank
            // Mixed is used for pointers that can come from different sources
            if matches!(return_type, Type::Pointer { .. }) {
                Ok(Value::FatPtr(FatPointer {
                    addr: Box::new(Value::Temp(temp_id)),
                    bank: BankTag::Mixed,
                }))
            } else if matches!(return_type, Type::Struct { .. } | Type::Union { .. }) {
                // Callee returns a pointer into its own frame. Copy into a
                // caller-owned temporary before that frame is reused
                // (nested calls like q_add(a, q_mul(b, c)) would otherwise
                // read freed stack).
                let src = Value::FatPtr(FatPointer {
                    addr: Box::new(Value::Temp(temp_id)),
                    bank: BankTag::Stack,
                });
                let dest = gen.builder.build_alloca(ir_return_type, None)
                    .map_err(|e| CodegenError::InternalError {
                        message: format!("Failed to allocate temp for aggregate return: {e}"),
                        location: rcc_common::SourceLocation::new_simple(0, 0),
                    })?;
                copy_struct(gen, src, dest.clone(), return_type)?;
                Ok(dest)
            } else {
                Ok(Value::Temp(temp_id))
            }
        }
        None => Ok(Value::Constant(0)), // void return
    }
}
