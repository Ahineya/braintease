//! Integer constant expressions (C99 6.6).
//!
//! Used for case labels, array sizes, enum values, and designator indices.
//! Identifiers (including enum constants) are not folded here; those need
//! semantic analysis.

use super::{BinaryOp, Expression, ExpressionKind, UnaryOp};

#[derive(Debug, Clone)]
pub struct ConstEvalError(pub String);

pub fn eval_integer_constant(expr: &Expression) -> Result<i64, ConstEvalError> {
    match &expr.kind {
        ExpressionKind::IntLiteral { value, .. } => Ok(*value),
        ExpressionKind::CharLiteral(v) => Ok(*v as i64),
        ExpressionKind::SizeofType(ty) => ty
            .size_in_bytes()
            .map(|s| s as i64)
            .ok_or_else(|| ConstEvalError("sizeof of incomplete type is not a constant".to_string())),
        ExpressionKind::Unary { op, operand } => {
            let v = eval_integer_constant(operand)?;
            match op {
                UnaryOp::Plus => Ok(v),
                UnaryOp::Minus => Ok(v.wrapping_neg()),
                UnaryOp::BitNot => Ok(!v),
                UnaryOp::LogicalNot => Ok(i64::from(v == 0)),
                _ => Err(ConstEvalError(
                    "Expression is not an integer constant expression".to_string(),
                )),
            }
        }
        ExpressionKind::Cast { operand, .. } => eval_integer_constant(operand),
        ExpressionKind::Conditional {
            condition,
            then_expr,
            else_expr,
        } => {
            // Both arms must be ICEs even if unused.
            let c = eval_integer_constant(condition)?;
            let t = eval_integer_constant(then_expr)?;
            let e = eval_integer_constant(else_expr)?;
            Ok(if c != 0 { t } else { e })
        }
        ExpressionKind::Binary { op, left, right } => {
            let l = eval_integer_constant(left)?;
            let r = eval_integer_constant(right)?;
            match op {
                BinaryOp::Add => Ok(l.wrapping_add(r)),
                BinaryOp::Sub => Ok(l.wrapping_sub(r)),
                BinaryOp::Mul => Ok(l.wrapping_mul(r)),
                BinaryOp::Div if r != 0 => Ok(l / r),
                BinaryOp::Mod if r != 0 => Ok(l % r),
                BinaryOp::BitAnd => Ok(l & r),
                BinaryOp::BitOr => Ok(l | r),
                BinaryOp::BitXor => Ok(l ^ r),
                BinaryOp::LeftShift => Ok(l.wrapping_shl(r as u32)),
                BinaryOp::RightShift => Ok(l.wrapping_shr(r as u32)),
                BinaryOp::Equal => Ok(i64::from(l == r)),
                BinaryOp::NotEqual => Ok(i64::from(l != r)),
                BinaryOp::Less => Ok(i64::from(l < r)),
                BinaryOp::Greater => Ok(i64::from(l > r)),
                BinaryOp::LessEqual => Ok(i64::from(l <= r)),
                BinaryOp::GreaterEqual => Ok(i64::from(l >= r)),
                BinaryOp::LogicalAnd => Ok(i64::from(l != 0 && r != 0)),
                BinaryOp::LogicalOr => Ok(i64::from(l != 0 || r != 0)),
                _ => Err(ConstEvalError(
                    "Expression is not an integer constant expression".to_string(),
                )),
            }
        }
        _ => Err(ConstEvalError(
            "Expression is not an integer constant expression".to_string(),
        )),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ast::NodeIdGenerator;
    use crate::types::Type;
    use rcc_common::{SourceLocation, SourceSpan};

    fn lit(v: i64) -> Expression {
        Expression {
            node_id: 0,
            kind: ExpressionKind::IntLiteral {
                value: v,
                suffix: crate::lexer::IntegerSuffix::None,
                hex: false,
            },
            span: SourceSpan::new(
                SourceLocation::new_simple(0, 0),
                SourceLocation::new_simple(0, 0),
            ),
            expr_type: None,
        }
    }

    fn bin(op: BinaryOp, l: i64, r: i64) -> Expression {
        let mut gen = NodeIdGenerator::new();
        Expression {
            node_id: gen.next(),
            kind: ExpressionKind::Binary {
                op,
                left: Box::new(lit(l)),
                right: Box::new(lit(r)),
            },
            span: SourceSpan::new(
                SourceLocation::new_simple(0, 0),
                SourceLocation::new_simple(0, 0),
            ),
            expr_type: None,
        }
    }

    #[test]
    fn folds_arithmetic() {
        assert_eq!(eval_integer_constant(&bin(BinaryOp::Add, 1, 2)).unwrap(), 3);
        assert_eq!(eval_integer_constant(&bin(BinaryOp::Mul, 2, 3)).unwrap(), 6);
        assert_eq!(eval_integer_constant(&bin(BinaryOp::LeftShift, 1, 2)).unwrap(), 4);
    }

    #[test]
    fn folds_sizeof_int() {
        let expr = Expression {
            node_id: 0,
            kind: ExpressionKind::SizeofType(Type::Int),
            span: SourceSpan::new(
                SourceLocation::new_simple(0, 0),
                SourceLocation::new_simple(0, 0),
            ),
            expr_type: None,
        };
        assert_eq!(eval_integer_constant(&expr).unwrap(), 2);
    }
}
