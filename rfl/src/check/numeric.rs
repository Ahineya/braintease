//! Numeric helpers for RFL checking.

use super::*;

pub(super) fn literal_int(expr: &Expr) -> Option<i64> {
    match &expr.kind {
        ExprKind::Int(value) => Some(*value),
        ExprKind::Unary {
            op: UnaryOp::Neg,
            expr,
        } => literal_int(expr).map(|value| value.wrapping_neg()),
        _ => None,
    }
}

/// Small ranges default to i16; their bounds may require a wider integer.
pub(super) fn signed_fit(start: i64, end: i64) -> Ty {
    for ty in [Ty::I16, Ty::I32, Ty::I64] {
        if ty.in_range(start) && ty.in_range(end) {
            return ty;
        }
    }
    Ty::I64
}

pub(super) fn number_default(lhs: &Expr, rhs: &Expr) -> Ty {
    if has_float_lit(lhs) || has_float_lit(rhs) {
        Ty::F64
    } else {
        Ty::I16
    }
}

pub(super) fn has_float_lit(expr: &Expr) -> bool {
    match &expr.kind {
        ExprKind::Float(_) => true,
        ExprKind::Unary { expr, .. } => has_float_lit(expr),
        ExprKind::Binary { lhs, rhs, .. } => has_float_lit(lhs) || has_float_lit(rhs),
        _ => false,
    }
}

pub(super) fn parse_float_literal(
    source: &str,
    span: usize,
    text: &str,
) -> Result<f64, CompileError> {
    let value: f64 = text
        .parse()
        .map_err(|_| CompileError::at(source, span, format!("invalid float literal {text}")))?;
    if !value.is_finite() {
        return Err(CompileError::at(
            source,
            span,
            format!("{text} does not fit in f64"),
        ));
    }
    Ok(value)
}

pub(super) fn float_literal_bits(ty: Ty, value: f64) -> Result<u64, String> {
    match ty {
        Ty::F32 => {
            let narrow = value as f32;
            if !narrow.is_finite() {
                return Err(format!("{value} does not fit in f32"));
            }
            Ok(u64::from(narrow.to_bits()))
        }
        Ty::F64 => Ok(value.to_bits()),
        _ => Err("not a float".to_string()),
    }
}

pub(super) fn float_value(ty: Ty, bits: u64) -> f64 {
    if ty == Ty::F32 {
        f64::from(f32::from_bits(bits as u32))
    } else {
        f64::from_bits(bits)
    }
}

pub(super) fn float_arith(ty: Ty, op: BinOp, left: u64, right: u64) -> u64 {
    let value = if ty == Ty::F32 {
        let left = f32::from_bits(left as u32);
        let right = f32::from_bits(right as u32);
        let value = match op {
            BinOp::Add => left + right,
            BinOp::Sub => left - right,
            BinOp::Mul => left * right,
            BinOp::Div => left / right,
            _ => 0.0,
        };
        return u64::from(value.to_bits());
    } else {
        let left = f64::from_bits(left);
        let right = f64::from_bits(right);
        match op {
            BinOp::Add => left + right,
            BinOp::Sub => left - right,
            BinOp::Mul => left * right,
            BinOp::Div => left / right,
            _ => 0.0,
        }
    };
    value.to_bits()
}

pub(super) fn float_compare(ty: Ty, op: BinOp, left: u64, right: u64) -> bool {
    let left = float_value(ty, left);
    let right = float_value(ty, right);
    match op {
        BinOp::Eq => left == right,
        BinOp::Ne => left != right,
        BinOp::Lt => left < right,
        BinOp::Le => left <= right,
        BinOp::Gt => left > right,
        BinOp::Ge => left >= right,
        _ => false,
    }
}

pub(super) fn can_cast(from: Ty, to: Ty) -> bool {
    if from == Ty::Char {
        return to == Ty::Char || to.is_int();
    }
    if to == Ty::Char {
        return from.is_int();
    }
    (from.is_int() || from.is_float()) && (to.is_int() || to.is_float())
}

pub(super) fn unicode_scalar(value: i64) -> bool {
    (0..=0x10FFFF).contains(&value) && !(0xD800..=0xDFFF).contains(&value)
}

pub(super) fn utf8_bytes(chars: &[u16]) -> Result<Vec<u8>, String> {
    let mut bytes = Vec::new();
    for scalar in char::decode_utf16(chars.iter().copied()) {
        let ch = scalar.map_err(|_| "string contains an unpaired surrogate".to_string())?;
        let mut buf = [0u8; 4];
        bytes.extend(ch.encode_utf8(&mut buf).as_bytes());
    }
    Ok(bytes)
}

pub(super) fn fold_cast(from: Ty, to: Ty, kind: &TExprKind) -> Option<Result<TExprKind, String>> {
    match kind {
        TExprKind::Char(scalar) if to == Ty::Char => Some(Ok(TExprKind::Char(*scalar))),
        TExprKind::Char(scalar) if to.is_int() => {
            let value = i64::from(*scalar);
            if !to.in_range(value) {
                return Some(Err(format!("{value} does not fit in {to}")));
            }
            Some(Ok(TExprKind::Int(value)))
        }
        TExprKind::Int(value) if to == Ty::Char => {
            if !unicode_scalar(*value) {
                return Some(Err(format!("{value} is not a Unicode scalar")));
            }
            Some(Ok(TExprKind::Char(*value as u32)))
        }
        TExprKind::Int(value) if from == to && to.is_int() => Some(Ok(TExprKind::Int(*value))),
        TExprKind::Int(value) if to.is_int() => {
            if !to.in_range(*value) {
                return Some(Err(format!("{value} does not fit in {to}")));
            }
            Some(Ok(TExprKind::Int(*value)))
        }
        TExprKind::Int(value) if to.is_float() => {
            Some(Ok(TExprKind::Float(int_to_float_bits(from, to, *value))))
        }
        TExprKind::Float(bits) if from == to => Some(Ok(TExprKind::Float(*bits))),
        TExprKind::Float(bits) if to.is_float() => {
            match float_literal_bits(to, float_value(from, *bits)) {
                Ok(bits) => Some(Ok(TExprKind::Float(bits))),
                Err(msg) => Some(Err(msg)),
            }
        }
        TExprKind::Float(bits) if to.is_int() => match float_to_int(to, from, *bits) {
            Ok(value) => Some(Ok(TExprKind::Int(value))),
            Err(msg) => Some(Err(msg)),
        },
        _ => None,
    }
}

pub(super) fn int_to_float_bits(from: Ty, to: Ty, value: i64) -> u64 {
    if to == Ty::F32 {
        let narrow = if from.is_signed() {
            value as f32
        } else {
            value as u64 as f32
        };
        u64::from(narrow.to_bits())
    } else {
        let wide = if from.is_signed() {
            value as f64
        } else {
            value as u64 as f64
        };
        wide.to_bits()
    }
}

pub(super) fn float_to_int(to: Ty, from: Ty, bits: u64) -> Result<i64, String> {
    let value = float_value(from, bits);
    if !value.is_finite() {
        return Err(format!("cannot cast {from} to {to}"));
    }
    let trunc = value.trunc();
    if to == Ty::U64 {
        if !(0.0..18446744073709551616.0).contains(&trunc) {
            return Err(format!("float value does not fit in {to}"));
        }
        return Ok(trunc as u64 as i64);
    }
    if trunc.abs() >= 9223372036854775808.0 && !(to == Ty::I64 && trunc == -9223372036854775808.0) {
        return Err(format!("float value does not fit in {to}"));
    }
    let value = trunc as i64;
    if !to.in_range(value) {
        return Err(format!("{value} does not fit in {to}"));
    }
    Ok(value)
}

pub(super) fn const_less(ty: Ty, a: i64, b: i64) -> bool {
    if ty.is_signed() {
        a < b
    } else {
        (ty.bits(a) as u64) < (ty.bits(b) as u64)
    }
}

pub(super) fn shareable(ty: Ty) -> bool {
    ty.is_int() || ty.is_float() || ty == Ty::Bool || ty == Ty::Char
}

pub(super) fn bit_width(ty: Ty) -> u32 {
    match ty {
        Ty::I8 | Ty::U8 => 8,
        Ty::I16 | Ty::U16 => 16,
        Ty::I32 | Ty::U32 | Ty::Usize => 32,
        _ => 64,
    }
}

pub(super) fn fold_bitnot(ty: Ty, value: i64) -> i64 {
    let width = bit_width(ty);
    let mask = if width == 64 {
        u64::MAX
    } else {
        (1u64 << width) - 1
    };
    ty.bits((!(value as u64) & mask) as i64)
}

pub(super) fn fold_bitwise(ty: Ty, op: BinOp, left: i64, right: i64) -> i64 {
    let width = bit_width(ty);
    let mask = if width == 64 {
        u64::MAX
    } else {
        (1u64 << width) - 1
    };
    let a = (left as u64) & mask;
    let b = (right as u64) & mask;
    let count = (b as u32) & (width - 1);
    let bits = match op {
        BinOp::BitAnd => a & b,
        BinOp::BitOr => a | b,
        BinOp::BitXor => a ^ b,
        BinOp::Shl => a.wrapping_shl(count) & mask,
        BinOp::Shr if ty.is_signed() => {
            let shifted = match width {
                8 => ((a as u8) as i8 >> count) as u64,
                16 => ((a as u16) as i16 >> count) as u64,
                32 => ((a as u32) as i32 >> count) as u64,
                _ => ((a as i64) >> count) as u64,
            };
            shifted & mask
        }
        BinOp::Shr => a >> count,
        _ => 0,
    };
    ty.bits(bits as i64)
}
