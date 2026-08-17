//! Integer conversion helpers for usual arithmetic conversions and assignment.

use super::{TypedExpressionGenerator, convert_type_default};
use crate::ir::{IrType, IrUnaryOp, Value};
use crate::types::Type;
use crate::codegen::CodegenError;
use crate::CompilerError;

fn intern_temp(result: Result<rcc_common::TempId, String>) -> Result<rcc_common::TempId, CompilerError> {
    result.map_err(|e| CodegenError::InternalError {
        message: e,
        location: rcc_common::SourceLocation::new_simple(0, 0),
    }.into())
}

/// C99 6.3.1.8 usual arithmetic conversions on this ILP16 / 32-bit-long target.
pub fn usual_arithmetic_type(left: &Type, right: &Type) -> Type {
    match (left, right) {
        (Type::UnsignedLong, _) | (_, Type::UnsignedLong) => Type::UnsignedLong,
        (Type::Long, _) | (_, Type::Long) => Type::Long,
        (Type::UnsignedInt, _) | (_, Type::UnsignedInt) => Type::UnsignedInt,
        _ => Type::Int,
    }
}

fn int_words(ty: &Type) -> u64 {
    ty.size_in_words().unwrap_or(1).max(1)
}

/// Convert an integer value from `from` to `to`.
///
/// Widening a constant keeps the full i64 (backend I32 materialization uses it).
/// Widening a temp emits SExt (signed source) or ZExt (unsigned source).
pub fn convert_integer(
    gen: &mut TypedExpressionGenerator,
    val: Value,
    from: &Type,
    to: &Type,
) -> Result<Value, CompilerError> {
    if !from.is_integer() || !to.is_integer() {
        return Ok(val);
    }
    if matches!(to, Type::Bool) {
        let scalar = match val {
            Value::FatPtr(fp) => *fp.addr,
            other => other,
        };
        let temp = intern_temp(gen.builder.build_binary(
            crate::ir::IrBinaryOp::Ne,
            scalar,
            Value::Constant(0),
            IrType::I16,
        ))?;
        return Ok(Value::Temp(temp));
    }

    let from_w = int_words(from);
    let to_w = int_words(to);
    if from_w == to_w {
        return Ok(val);
    }

    let to_ir = convert_type_default(to)?;
    if to_w > from_w {
        let op = if from.is_unsigned_integer() {
            IrUnaryOp::ZExt
        } else {
            IrUnaryOp::SExt
        };
        let temp = intern_temp(gen.builder.build_unary(op, val, to_ir))?;
        Ok(Value::Temp(temp))
    } else {
        let temp = intern_temp(gen.builder.build_unary(IrUnaryOp::Trunc, val, to_ir))?;
        Ok(Value::Temp(temp))
    }
}
