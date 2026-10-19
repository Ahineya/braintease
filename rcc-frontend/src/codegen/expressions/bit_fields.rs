//! Bit-fields share a declared integer storage unit, least significant bit first.
//! Keep the unit read separate so compound updates perform one volatile read.
use super::{TypedExpressionGenerator, convert_type_default};
use crate::{CompilerError, typed_ast::TypedExpr, types::Type};
use crate::ir::{IrBinaryOp as B, Value};

pub fn spec(expr: &TypedExpr) -> Option<(u8, u8)> {
    match expr { TypedExpr::MemberAccess { bit_field, .. } => *bit_field, _ => None }
}

pub fn mask(width: u8) -> i64 { if width == 64 { -1 } else { ((1u64 << width) - 1) as i64 } }

fn bin(gen: &mut TypedExpressionGenerator, op: B, a: Value, b: i64, ty: &Type) -> Result<Value, CompilerError> {
    Ok(Value::Temp(gen.builder.build_binary(op, a, Value::Constant(b), convert_type_default(ty)?)?))
}

pub fn extract(gen: &mut TypedExpressionGenerator, unit: Value, ty: &Type, width: u8, shift: u8) -> Result<Value, CompilerError> {
    let value = if shift != 0 { bin(gen, B::LShr, unit, shift as i64, ty)? } else { unit };
    let value = bin(gen, B::And, value, mask(width), ty)?;
    let bits = ty.size_in_words().unwrap() * 16;
    if !ty.is_unsigned_integer() && !matches!(ty.unqualified(), Type::Bool) && (width as u64) < bits {
        let value = bin(gen, B::Shl, value, (bits - width as u64) as i64, ty)?;
        bin(gen, B::AShr, value, (bits - width as u64) as i64, ty)
    } else { Ok(value) }
}

pub fn load(gen: &mut TypedExpressionGenerator, expr: &TypedExpr, addr: Value) -> Result<(Value, Option<Value>), CompilerError> {
    let ty = expr.get_declared_type();
    let unit = Value::Temp(gen.builder.build_load_volatile(addr, convert_type_default(ty)?, ty.is_volatile())?);
    if let Some((width, shift)) = spec(expr) {
        let value = extract(gen, unit.clone(), ty, width, shift)?;
        let value = super::conversions::convert_integer(gen, value, ty, expr.get_type())?;
        Ok((value, Some(unit)))
    } else { Ok((unit, None)) }
}

pub fn store_unit(gen: &mut TypedExpressionGenerator, addr: Value, ty: &Type, width: u8, shift: u8,
                  value: Value, old: Option<Value>) -> Result<Value, CompilerError> {
    let ir = convert_type_default(ty)?;
    let value = bin(gen, B::And, value, mask(width), ty)?;
    let positioned = if shift != 0 { bin(gen, B::Shl, value.clone(), shift as i64, ty)? } else { value.clone() };
    let old = match old { Some(v) => v, None => Value::Temp(gen.builder.build_load_volatile(addr.clone(), ir.clone(), ty.is_volatile())?) };
    let keep = bin(gen, B::And, old, !mask(width).wrapping_shl(shift as u32), ty)?;
    let merged = Value::Temp(gen.builder.build_binary(B::Or, keep, positioned, ir.clone())?);
    gen.builder.build_store_volatile(merged, addr, ir, ty.is_volatile())?;
    extract(gen, value, ty, width, 0)
}

pub fn store(gen: &mut TypedExpressionGenerator, expr: &TypedExpr, addr: Value, value: Value, old: Option<Value>) -> Result<Value, CompilerError> {
    if let Some((width, shift)) = spec(expr) {
        let value = store_unit(gen, addr, expr.get_declared_type(), width, shift, value, old)?;
        super::conversions::convert_integer(gen, value, expr.get_declared_type(), expr.get_type())
    } else {
        gen.builder.build_store_volatile(value.clone(), addr, convert_type_default(expr.get_type())?, expr.get_declared_type().is_volatile())?;
        Ok(value)
    }
}
