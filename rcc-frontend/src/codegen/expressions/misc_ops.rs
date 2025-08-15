//! Miscellaneous operation code generation (sizeof, array initializers, etc.)

use super::TypedExpressionGenerator;
use crate::ir::Value;
use crate::typed_ast::TypedExpr;
use crate::types::Type;
use crate::codegen::CodegenError;
use crate::CompilerError;

/// Get size in bytes for C sizeof operator
/// This is ONLY for C sizeof compatibility - Ripple VM is word-addressed
/// so all actual memory operations use size_in_words()
fn size_in_bytes(ty: &Type) -> u64 {
    match ty {
        Type::Void => 0,
        Type::Bool => 1,
        Type::Char | Type::SignedChar | Type::UnsignedChar => 1,
        Type::Short | Type::UnsignedShort => 2,
        Type::Int | Type::UnsignedInt => 2,  // 16-bit int
        Type::Long | Type::UnsignedLong => 4, // 32-bit long
        Type::Pointer { .. } => 4, // Fat pointer: 2 words = 4 bytes
        Type::Array { element_type, size } => {
            size.map(|s| s * size_in_bytes(element_type)).unwrap_or(0)
        }
        Type::Struct { .. } | Type::Union { .. } => {
            // Structs/unions: words * 2
            ty.size_in_words().unwrap_or(0) * 2
        }
        _ => 0,
    }
}

pub fn generate_sizeof_expr(
    _gen: &mut TypedExpressionGenerator,
    operand: &TypedExpr,
) -> Result<Value, CompilerError> {
    // sizeof returns size in bytes for C compatibility
    let size = size_in_bytes(&operand.get_type());
    Ok(Value::Constant(size as i64))
}

pub fn generate_sizeof_type(
    _gen: &mut TypedExpressionGenerator,
    target_type: &Type,
) -> Result<Value, CompilerError> {
    // sizeof returns size in bytes for C compatibility
    let size = size_in_bytes(target_type);
    Ok(Value::Constant(size as i64))
}

pub fn generate_array_initializer(
    gen: &mut TypedExpressionGenerator,
    elements: &[TypedExpr],
) -> Result<Value, CompilerError> {
    // For array initializers, we need to evaluate each element
    // and collect them into a ConstantArray value
    // This only works for constant expressions
    let mut values = Vec::new();
    for elem in elements {
        match gen.generate(elem)? {
            Value::Constant(val) => values.push(val),
            _ => {
                return Err(CodegenError::UnsupportedConstruct {
                    construct: "non-constant array initializer".to_string(),
                    location: rcc_common::SourceLocation::new_simple(0, 0),
                }
                .into())
            }
        }
    }
    Ok(Value::ConstantArray(values))
}