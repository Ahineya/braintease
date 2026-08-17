//! Assignment operation code generation

use super::TypedExpressionGenerator;
use super::conversions::convert_integer;
use super::unary_ops::generate_lvalue_address;
use crate::ir::{Value, IrType, IrBinaryOp};
use crate::typed_ast::TypedExpr;
use crate::types::Type;
use crate::CompilerError;
use crate::codegen::CodegenError;

fn to_bool_value(
    gen: &mut TypedExpressionGenerator,
    val: Value,
) -> Result<Value, CompilerError> {
    let scalar = match val {
        Value::FatPtr(fp) => *fp.addr,
        other => other,
    };
    let temp = gen.builder.build_binary(
        IrBinaryOp::Ne,
        scalar,
        Value::Constant(0),
        IrType::I16,
    )?;
    Ok(Value::Temp(temp))
}

/// Copy struct contents from source pointer to destination pointer
/// 
/// This performs a word-by-word copy of struct data, similar to memcpy.
/// Used for struct assignments and initializations.
pub fn copy_struct(
    gen: &mut TypedExpressionGenerator,
    src_ptr: Value,
    dst_ptr: Value,
    struct_type: &Type,
) -> Result<(), CompilerError> {
    // Calculate struct size
    let struct_size = struct_type.size_in_words()
        .ok_or_else(|| CodegenError::InternalError {
            message: "Cannot determine struct size".to_string(),
            location: rcc_common::SourceLocation::new_simple(0, 0),
        })?;
    
    // Copy each word from source to destination
    for offset in 0..struct_size {
        let offset_val = Value::Constant(offset as i64);
        
        // Calculate source address (source pointer + offset)
        let src_addr = gen.builder.build_pointer_offset(
            src_ptr.clone(),
            offset_val.clone(),
            IrType::I16, // Copy word by word
        )?;
        
        // Calculate destination address (destination pointer + offset)
        let dst_addr = gen.builder.build_pointer_offset(
            dst_ptr.clone(),
            offset_val,
            IrType::I16,
        )?;
        
        // Load from source
        let word = gen.builder.build_load(src_addr, IrType::I16)?;
        
        // Store to destination
        gen.builder.build_store(Value::Temp(word), dst_addr, IrType::I16)?;
    }
    
    Ok(())
}

pub fn generate_assignment(
    gen: &mut TypedExpressionGenerator,
    lhs: &TypedExpr,
    rhs: &TypedExpr,
) -> Result<Value, CompilerError> {
    let lhs_addr = generate_lvalue_address(gen, lhs)?;
    let lhs_type = lhs.get_type();
    let rhs_type = rhs.get_type();
    
    // Struct and union assignment copies the object word-by-word.
    // The RHS generates as a pointer to the source object.
    if lhs_type.is_aggregate() && rhs_type.is_aggregate() {
        let rhs_val = gen.generate(rhs)?;
        copy_struct(gen, rhs_val.clone(), lhs_addr, &lhs_type)?;
        Ok(rhs_val)
    } else {
        let rhs_val = gen.generate(rhs)?;
        let stored = if matches!(lhs_type, Type::Bool) {
            to_bool_value(gen, rhs_val)?
        } else if lhs_type.is_integer() && rhs_type.is_integer() {
            convert_integer(gen, rhs_val, rhs_type, lhs_type)?
        } else {
            rhs_val
        };
        let ir_type = super::convert_type_default(lhs_type)?;
        gen.builder.build_store(stored.clone(), lhs_addr, ir_type)?;
        Ok(stored)
    }
}
