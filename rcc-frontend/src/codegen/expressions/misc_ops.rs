//! Miscellaneous operation code generation (sizeof, array initializers, etc.)

use super::TypedExpressionGenerator;
use crate::ir::Value;
use crate::typed_ast::TypedExpr;
use crate::types::Type;
use crate::codegen::CodegenError;
use crate::CompilerError;

/// Produce a size_t value in C bytes (16-bit addressable cells).
/// Materialize the 32-bit result so calls and assignments retain both words.
fn generate_sizeof(gen: &mut TypedExpressionGenerator, ty: &Type) -> Result<Value, CompilerError> {
    let size = ty.size_in_bytes().ok_or_else(|| CodegenError::InvalidType {
        ast_type: ty.clone(),
        location: rcc_common::SourceLocation::new_simple(0, 0),
    })?;
    super::conversions::materialize_wide(gen, Value::Constant(size as i64), &Type::UnsignedLong)
}

pub fn generate_sizeof_expr(
    gen: &mut TypedExpressionGenerator,
    operand: &TypedExpr,
) -> Result<Value, CompilerError> {
    generate_sizeof(gen, operand.get_type())
}

pub fn generate_sizeof_type(
    gen: &mut TypedExpressionGenerator,
    target_type: &Type,
) -> Result<Value, CompilerError> {
    generate_sizeof(gen, target_type)
}

pub fn generate_array_initializer(
    gen: &mut TypedExpressionGenerator,
    elements: &[TypedExpr],
) -> Result<Value, CompilerError> {
    let mut values = Vec::new();
    for elem in elements {
        values.extend(flatten_constant_initializer(gen, elem)?);
    }
    Ok(Value::ConstantArray(values))
}

fn flatten_constant_initializer(
    gen: &mut TypedExpressionGenerator,
    expr: &TypedExpr,
) -> Result<Vec<i64>, CompilerError> {
    match expr {
        TypedExpr::IntLiteral { value, .. } => Ok(vec![*value]),
        TypedExpr::CharLiteral { value, .. } => Ok(vec![*value as i64]),
        TypedExpr::ArrayInitializer { elements, .. }
        | TypedExpr::CompoundLiteral { initializer: elements, .. } => {
            let mut values = Vec::new();
            for elem in elements {
                values.extend(flatten_constant_initializer(gen, elem)?);
            }
            Ok(values)
        }
        _ => match gen.generate(expr)? {
            Value::Constant(val) => Ok(vec![val]),
            Value::ConstantArray(vals) => Ok(vals),
            _ => Err(CodegenError::UnsupportedConstruct {
                construct: "non-constant array initializer".to_string(),
                location: rcc_common::SourceLocation::new_simple(0, 0),
            }
            .into()),
        },
    }
}