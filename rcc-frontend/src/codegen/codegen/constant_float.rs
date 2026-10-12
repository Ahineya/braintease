//! Floating constant expressions used by integer static initializers.

use crate::ast::{BinaryOp, UnaryOp};
use crate::typed_ast::TypedExpr;
use crate::types::Type;

/// A floating static initializer is stored in its IEEE representation.
pub(super) fn initializer_bits(expr: &TypedExpr) -> Option<i64> {
    match expr.get_type() {
        Type::Float => Some((evaluate(expr)? as f32).to_bits() as i64),
        Type::Double => Some(evaluate(expr)?.to_bits() as i64),
        _ => None,
    }
}

/// C truncates toward zero when a finite floating value converts to integer.
/// Reject out-of-range conversions rather than applying Rust's saturation.
pub(super) fn cast_integer(expr: &TypedExpr, target: &Type) -> Option<i64> {
    let value = evaluate(expr)?;
    if matches!(target, Type::Bool) {
        return Some(i64::from(value != 0.0));
    }
    if !value.is_finite() { return None; }
    let value = value.trunc();
    let bits = (target.size_in_words()? * 16) as i32;
    if target.is_unsigned_integer() {
        if value < 0.0 || value >= 2.0f64.powi(bits) { return None; }
        Some(value as u64 as i64)
    } else {
        let limit = 2.0f64.powi(bits - 1);
        if value < -limit || value >= limit { return None; }
        Some(value as i64)
    }
}

fn evaluate(expr: &TypedExpr) -> Option<f64> {
    if expr.get_type().is_integer() {
        let value = super::global_gen::scalar_initializer_constant(expr)?;
        return Some(if expr.get_type().is_unsigned_integer() {
            value as u64 as f64
        } else {
            value as f64
        });
    }
    let value = match expr {
        TypedExpr::FloatLiteral { bits, expr_type: Type::Float } => f32::from_bits(*bits as u32) as f64,
        TypedExpr::FloatLiteral { bits, expr_type: Type::Double } => f64::from_bits(*bits),
        TypedExpr::Unary { op: UnaryOp::Plus, operand, .. } => evaluate(operand)?,
        TypedExpr::Unary { op: UnaryOp::Minus, operand, .. } => -evaluate(operand)?,
        TypedExpr::Binary { op, left, right, .. } => {
            let left = evaluate(left)?;
            let right = evaluate(right)?;
            let (left, right) = if matches!(expr.get_type(), Type::Float) {
                ((left as f32) as f64, (right as f32) as f64)
            } else { (left, right) };
            match op {
                BinaryOp::Add => left + right,
                BinaryOp::Sub => left - right,
                BinaryOp::Mul => left * right,
                BinaryOp::Div => left / right,
                _ => return None,
            }
        }
        TypedExpr::Cast { operand, target_type, .. } if target_type.is_floating() => evaluate(operand)?,
        TypedExpr::Conditional { condition, then_expr, else_expr, .. } => {
            evaluate(if evaluate(condition)? != 0.0 { then_expr } else { else_expr })?
        }
        _ => return None,
    };
    // Every float operation rounds to binary32, including explicit casts.
    Some(if matches!(expr.get_type(), Type::Float) { (value as f32) as f64 } else { value })
}
