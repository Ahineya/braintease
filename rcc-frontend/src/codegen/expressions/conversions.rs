//! Integer and floating conversion helpers for usual arithmetic conversions
//! and assignment. Floating ops lower to runtime soft-float calls.

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

/// C99 6.3.1.8 usual arithmetic conversions.
pub fn usual_arithmetic_type(left: &Type, right: &Type) -> Type {
    match (left, right) {
        (Type::Double, _) | (_, Type::Double) => Type::Double,
        (Type::Float, _) | (_, Type::Float) => Type::Float,
        (Type::UnsignedLongLong, _) | (_, Type::UnsignedLongLong) => Type::UnsignedLongLong,
        (Type::LongLong, _) | (_, Type::LongLong) => Type::LongLong,
        (Type::UnsignedLong, _) | (_, Type::UnsignedLong) => Type::UnsignedLong,
        (Type::Long, _) | (_, Type::Long) => Type::Long,
        (Type::UnsignedInt, _) | (_, Type::UnsignedInt) => Type::UnsignedInt,
        _ => Type::Int,
    }
}

fn int_words(ty: &Type) -> u64 {
    ty.size_in_words().unwrap_or(1).max(1)
}

/// Force a constant into a typed I32/I64 temp so the ABI passes all words.
pub fn materialize_wide(
    gen: &mut TypedExpressionGenerator,
    val: Value,
    ty: &Type,
) -> Result<Value, CompilerError> {
    let ir_type = match ty {
        Type::Float => IrType::I32,
        Type::Double => IrType::I64,
        Type::Long | Type::UnsignedLong => IrType::I32,
        Type::LongLong | Type::UnsignedLongLong => IrType::I64,
        _ => return Ok(val),
    };
    if matches!(val, Value::Temp(_)) {
        return Ok(val);
    }
    let temp = intern_temp(gen.builder.build_binary(
        crate::ir::IrBinaryOp::Add,
        val,
        Value::Constant(0),
        ir_type,
    ))?;
    Ok(Value::Temp(temp))
}

fn emit_call(
    gen: &mut TypedExpressionGenerator,
    name: &str,
    args: Vec<Value>,
    ret_ty: IrType,
) -> Result<Value, CompilerError> {
    let result = gen.builder.build_call(Value::Global(name.to_string()), args, ret_ty)
        .map_err(|e| CodegenError::InternalError {
            message: e,
            location: rcc_common::SourceLocation::new_simple(0, 0),
        })?;
    match result {
        Some(temp) => Ok(Value::Temp(temp)),
        None => Ok(Value::Constant(0)),
    }
}

fn arith_helper(op: crate::ast::BinaryOp, is_f64: bool) -> Option<&'static str> {
    use crate::ast::BinaryOp;
    match (op, is_f64) {
        (BinaryOp::Add | BinaryOp::AddAssign, false) => Some("__rcc_f32_add"),
        (BinaryOp::Add | BinaryOp::AddAssign, true) => Some("__rcc_f64_add"),
        (BinaryOp::Sub | BinaryOp::SubAssign, false) => Some("__rcc_f32_sub"),
        (BinaryOp::Sub | BinaryOp::SubAssign, true) => Some("__rcc_f64_sub"),
        (BinaryOp::Mul | BinaryOp::MulAssign, false) => Some("__rcc_f32_mul"),
        (BinaryOp::Mul | BinaryOp::MulAssign, true) => Some("__rcc_f64_mul"),
        (BinaryOp::Div | BinaryOp::DivAssign, false) => Some("__rcc_f32_div"),
        (BinaryOp::Div | BinaryOp::DivAssign, true) => Some("__rcc_f64_div"),
        _ => None,
    }
}

fn cmp_helper(op: crate::ast::BinaryOp, is_f64: bool) -> Option<&'static str> {
    use crate::ast::BinaryOp;
    match (op, is_f64) {
        (BinaryOp::Equal, false) => Some("__rcc_f32_eq"),
        (BinaryOp::Equal, true) => Some("__rcc_f64_eq"),
        (BinaryOp::NotEqual, false) => Some("__rcc_f32_ne"),
        (BinaryOp::NotEqual, true) => Some("__rcc_f64_ne"),
        (BinaryOp::Less, false) => Some("__rcc_f32_lt"),
        (BinaryOp::Less, true) => Some("__rcc_f64_lt"),
        (BinaryOp::LessEqual, false) => Some("__rcc_f32_le"),
        (BinaryOp::LessEqual, true) => Some("__rcc_f64_le"),
        (BinaryOp::Greater, false) => Some("__rcc_f32_gt"),
        (BinaryOp::Greater, true) => Some("__rcc_f64_gt"),
        (BinaryOp::GreaterEqual, false) => Some("__rcc_f32_ge"),
        (BinaryOp::GreaterEqual, true) => Some("__rcc_f64_ge"),
        _ => None,
    }
}

pub fn emit_float_binop(
    gen: &mut TypedExpressionGenerator,
    op: crate::ast::BinaryOp,
    left: Value,
    right: Value,
    common: &Type,
) -> Result<Value, CompilerError> {
    let is_f64 = matches!(common, Type::Double);
    let left = materialize_wide(gen, left, common)?;
    let right = materialize_wide(gen, right, common)?;
    if let Some(name) = arith_helper(op, is_f64) {
        let ret = if is_f64 { IrType::F64 } else { IrType::F32 };
        return emit_call(gen, name, vec![left, right], ret);
    }
    if let Some(name) = cmp_helper(op, is_f64) {
        return emit_call(gen, name, vec![left, right], IrType::I16);
    }
    Err(CodegenError::UnsupportedConstruct {
        construct: format!("floating binary op: {op:?}"),
        location: rcc_common::SourceLocation::new_simple(0, 0),
    }
    .into())
}

fn from_int_helper(to_float: &Type, from_unsigned: bool, from_words: u64) -> &'static str {
    match (to_float, from_unsigned, from_words) {
        (Type::Float, false, 4) => "__rcc_f32_from_i64",
        (Type::Float, true, 4) => "__rcc_f32_from_u64",
        (Type::Float, true, _) => "__rcc_f32_from_ui",
        (Type::Float, false, _) => "__rcc_f32_from_si",
        (Type::Double, false, 4) => "__rcc_f64_from_i64",
        (Type::Double, true, 4) => "__rcc_f64_from_u64",
        (Type::Double, true, _) => "__rcc_f64_from_ui",
        (Type::Double, false, _) => "__rcc_f64_from_si",
        _ => "__rcc_f32_from_si",
    }
}

fn to_int_helper(from_float: &Type, to_unsigned: bool, to_words: u64) -> &'static str {
    match (from_float, to_unsigned, to_words) {
        (Type::Float, false, 4) => "__rcc_f32_to_i64",
        (Type::Float, true, 4) => "__rcc_f32_to_u64",
        (Type::Float, true, _) => "__rcc_f32_to_ui",
        (Type::Float, false, _) => "__rcc_f32_to_si",
        (Type::Double, false, 4) => "__rcc_f64_to_i64",
        (Type::Double, true, 4) => "__rcc_f64_to_u64",
        (Type::Double, true, _) => "__rcc_f64_to_ui",
        (Type::Double, false, _) => "__rcc_f64_to_si",
        _ => "__rcc_f32_to_si",
    }
}

/// Convert `val` from `from` to `to`, including integer and floating conversions.
pub fn convert_value(
    gen: &mut TypedExpressionGenerator,
    val: Value,
    from: &Type,
    to: &Type,
) -> Result<Value, CompilerError> {
    if from == to {
        return Ok(val);
    }
    if matches!(to, Type::Bool) {
        return convert_to_bool(gen, val, from);
    }

    if from.is_integer() && to.is_integer() {
        return convert_integer(gen, val, from, to);
    }

    if from.is_floating() && to.is_floating() {
        if matches!((from, to), (Type::Float, Type::Double)) {
            let val = materialize_wide(gen, val, from)?;
            return emit_call(gen, "__rcc_f64_from_f32", vec![val], IrType::F64);
        }
        if matches!((from, to), (Type::Double, Type::Float)) {
            let val = materialize_wide(gen, val, from)?;
            return emit_call(gen, "__rcc_f32_from_f64", vec![val], IrType::F32);
        }
        return Ok(val);
    }

    if from.is_integer() && to.is_floating() {
        let words = int_words(from);
        let helper_src = if words < 2 {
            convert_integer(
                gen,
                val,
                from,
                if from.is_unsigned_integer() { &Type::UnsignedLong } else { &Type::Long },
            )?
        } else {
            val
        };
        let src_ty = if words >= 4 {
            from
        } else if words < 2 {
            if from.is_unsigned_integer() { &Type::UnsignedLong } else { &Type::Long }
        } else {
            from
        };
        let helper_src = materialize_wide(gen, helper_src, src_ty)?;
        let name = from_int_helper(to, from.is_unsigned_integer(), int_words(src_ty));
        let ret = if matches!(to, Type::Double) { IrType::F64 } else { IrType::F32 };
        return emit_call(gen, name, vec![helper_src], ret);
    }

    if from.is_floating() && to.is_integer() {
        let val = materialize_wide(gen, val, from)?;
        let dest_words = int_words(to);
        let helper_dest_words = dest_words.max(2);
        let name = to_int_helper(from, to.is_unsigned_integer(), helper_dest_words);
        let ret_ir = if helper_dest_words >= 4 { IrType::I64 } else { IrType::I32 };
        let converted = emit_call(gen, name, vec![val], ret_ir)?;
        let helper_ty = if helper_dest_words >= 4 {
            if to.is_unsigned_integer() { Type::UnsignedLongLong } else { Type::LongLong }
        } else if to.is_unsigned_integer() {
            Type::UnsignedLong
        } else {
            Type::Long
        };
        if dest_words < helper_dest_words {
            return convert_integer(gen, converted, &helper_ty, to);
        }
        return Ok(converted);
    }

    Ok(val)
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
        return convert_value(gen, val, from, to);
    }
    if matches!(to, Type::Bool) {
        return convert_to_bool(gen, val, from);
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

/// Convert a scalar to 0/1 int (C boolean context, including `if (f)`).
pub fn convert_to_bool(
    gen: &mut TypedExpressionGenerator,
    val: Value,
    from: &Type,
) -> Result<Value, CompilerError> {
    if from.is_floating() {
        let zero = Value::Constant(0);
        return emit_float_binop(gen, crate::ast::BinaryOp::NotEqual, val, zero, from);
    }
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
    Ok(Value::Temp(temp))
}

pub fn one_as_float(ty: &Type) -> Value {
    match ty {
        Type::Float => Value::Constant(1.0f32.to_bits() as i64),
        Type::Double => Value::Constant(1.0f64.to_bits() as i64),
        _ => Value::Constant(1),
    }
}
