//! Numeric support for RFL lowering.

use super::*;

pub(super) fn widen_for_op(
    builder: &mut IrBuilder,
    value: Value,
    ty: Ty,
) -> Result<Value, CompileError> {
    match ty {
        Ty::I8 => sign_extend_i8(builder, value),
        Ty::U8 => Ok(Value::Temp(
            builder
                .build_unary(IrUnaryOp::ZExt, value, IrType::I16)
                .ir()?,
        )),
        _ => Ok(value),
    }
}

pub(super) fn cast_char(
    builder: &mut IrBuilder,
    value: Value,
    from: Ty,
    to: Ty,
) -> Result<Value, CompileError> {
    if from == Ty::Char && to == Ty::Char {
        return Ok(value);
    }
    if from == Ty::Char {
        return cast_value(builder, value, Ty::I32, to);
    }
    let bits = cast_value(builder, value, from, Ty::I32)?;
    let limit = i32_const(builder, 0x10FFFF)?;
    let surr_lo = i32_const(builder, 0xD800)?;
    let surr_hi = i32_const(builder, 0xDFFF)?;
    let bad = builder.new_label();
    let ok = builder.new_label();
    let above = builder
        .build_binary(IrBinaryOp::Ugt, bits.clone(), limit, IrType::I1)
        .ir()?;
    let below_surr = builder
        .build_binary(IrBinaryOp::Ult, bits.clone(), surr_lo, IrType::I1)
        .ir()?;
    let above_surr = builder
        .build_binary(IrBinaryOp::Ugt, bits.clone(), surr_hi, IrType::I1)
        .ir()?;
    let outside = builder
        .build_binary(
            IrBinaryOp::Or,
            Value::Temp(below_surr),
            Value::Temp(above_surr),
            IrType::I1,
        )
        .ir()?;
    let surrogate = builder
        .build_binary(
            IrBinaryOp::Eq,
            Value::Temp(outside),
            Value::Constant(0),
            IrType::I1,
        )
        .ir()?;
    let invalid = builder
        .build_binary(
            IrBinaryOp::Or,
            Value::Temp(above),
            Value::Temp(surrogate),
            IrType::I1,
        )
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(invalid), bad, ok)
        .ir()?;
    builder.create_block(bad).ir()?;
    fail(builder, FAIL_CHAR)?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(ok).ir()?;
    Ok(bits)
}

pub(super) fn cast_value(
    builder: &mut IrBuilder,
    value: Value,
    from: Ty,
    to: Ty,
) -> Result<Value, CompileError> {
    if from == Ty::Char || to == Ty::Char {
        return cast_char(builder, value, from, to);
    }
    if from.is_float() || to.is_float() {
        return cast_float(builder, value, from, to);
    }
    if from == to
        || (ir_ty(from) == ir_ty(to) && from.cells() == to.cells() && width(from) == width(to))
    {
        return Ok(value);
    }
    let mut value = value;
    if width(from) < 16 {
        value = if from.is_signed() {
            sign_extend_i8(builder, value)?
        } else {
            Value::Temp(
                builder
                    .build_unary(IrUnaryOp::ZExt, value, IrType::I16)
                    .ir()?,
            )
        };
    }
    if width(to) < width(from)
        || (width(from) < 16 && width(to) <= 16 && matches!(to, Ty::I8 | Ty::U8))
    {
        if matches!(to, Ty::I8 | Ty::U8) {
            let low = if width(from) > 16 {
                Value::Temp(
                    builder
                        .build_unary(IrUnaryOp::Trunc, value, IrType::I16)
                        .ir()?,
                )
            } else {
                value
            };
            return Ok(Value::Temp(
                builder
                    .build_unary(IrUnaryOp::Trunc, low, IrType::I8)
                    .ir()?,
            ));
        }
        return Ok(Value::Temp(
            builder
                .build_unary(IrUnaryOp::Trunc, value, ir_ty(to))
                .ir()?,
        ));
    }
    if width(to) > 16 && width(from) <= 16 {
        let op = if from.is_signed() {
            IrUnaryOp::SExt
        } else {
            IrUnaryOp::ZExt
        };
        let mid = if width(to) == 64 && width(from) == 16 {
            value
        } else if width(to) == 64 {
            Value::Temp(builder.build_unary(op, value, IrType::I32).ir()?)
        } else {
            value
        };
        let op = if from.is_signed() {
            IrUnaryOp::SExt
        } else {
            IrUnaryOp::ZExt
        };
        return Ok(Value::Temp(builder.build_unary(op, mid, ir_ty(to)).ir()?));
    }
    if width(to) > width(from) {
        let op = if from.is_signed() {
            IrUnaryOp::SExt
        } else {
            IrUnaryOp::ZExt
        };
        return Ok(Value::Temp(builder.build_unary(op, value, ir_ty(to)).ir()?));
    }
    Ok(Value::Temp(
        builder
            .build_unary(IrUnaryOp::Trunc, value, ir_ty(to))
            .ir()?,
    ))
}

pub(super) fn cast_float(
    builder: &mut IrBuilder,
    value: Value,
    from: Ty,
    to: Ty,
) -> Result<Value, CompileError> {
    if from == to {
        return Ok(value);
    }
    if from.is_int() && to.is_float() {
        let (src, unsigned, words) = float_int_width(from);
        let widened = if src == from {
            value
        } else {
            cast_value(builder, value, from, src)?
        };
        let ret = if to == Ty::F64 {
            IrType::I64
        } else {
            IrType::I32
        };
        return call_helper(
            builder,
            from_int_helper(to, unsigned, words),
            vec![widened],
            ret,
        );
    }
    if from.is_float() && to.is_int() {
        let (dest, unsigned, words) = float_int_width(to);
        let ret = if words == 4 { IrType::I64 } else { IrType::I32 };
        let raw = call_helper(
            builder,
            to_int_helper(from, unsigned, words),
            vec![value],
            ret,
        )?;
        if dest == to {
            return Ok(raw);
        }
        return cast_value(builder, raw, dest, to);
    }
    if matches!((from, to), (Ty::F32, Ty::F64)) {
        return call_helper(builder, "__rcc_f64_from_f32", vec![value], IrType::I64);
    }
    if matches!((from, to), (Ty::F64, Ty::F32)) {
        return call_helper(builder, "__rcc_f32_from_f64", vec![value], IrType::I32);
    }
    Err(CompileError::plain(format!("cannot cast {from} to {to}")))
}

/// Integer width the soft-float helpers actually accept or return.
/// `long` is 32 bits. `long long` is 64 bits.
pub(super) fn float_int_width(ty: Ty) -> (Ty, bool, u64) {
    match ty {
        Ty::I64 => (Ty::I64, false, 4),
        Ty::U64 => (Ty::U64, true, 4),
        Ty::I8 | Ty::I16 | Ty::I32 => (Ty::I32, false, 2),
        _ => (Ty::U32, true, 2),
    }
}

pub(super) fn from_int_helper(to: Ty, unsigned: bool, words: u64) -> &'static str {
    match (to == Ty::F64, unsigned, words == 4) {
        (false, false, true) => "__rcc_f32_from_i64",
        (false, true, true) => "__rcc_f32_from_u64",
        (false, true, false) => "__rcc_f32_from_ui",
        (false, false, false) => "__rcc_f32_from_si",
        (true, false, true) => "__rcc_f64_from_i64",
        (true, true, true) => "__rcc_f64_from_u64",
        (true, true, false) => "__rcc_f64_from_ui",
        (true, false, false) => "__rcc_f64_from_si",
    }
}

pub(super) fn to_int_helper(from: Ty, unsigned: bool, words: u64) -> &'static str {
    match (from == Ty::F64, unsigned, words == 4) {
        (false, false, true) => "__rcc_f32_to_i64",
        (false, true, true) => "__rcc_f32_to_u64",
        (false, true, false) => "__rcc_f32_to_ui",
        (false, false, false) => "__rcc_f32_to_si",
        (true, false, true) => "__rcc_f64_to_i64",
        (true, true, true) => "__rcc_f64_to_u64",
        (true, true, false) => "__rcc_f64_to_ui",
        (true, false, false) => "__rcc_f64_to_si",
    }
}

pub(super) fn sign_extend_i8(builder: &mut IrBuilder, value: Value) -> Result<Value, CompileError> {
    let wide = builder
        .build_unary(IrUnaryOp::ZExt, value, IrType::I16)
        .ir()?;
    let shifted = builder
        .build_binary(
            IrBinaryOp::Shl,
            Value::Temp(wide),
            Value::Constant(8),
            IrType::I16,
        )
        .ir()?;
    Ok(Value::Temp(
        builder
            .build_binary(
                IrBinaryOp::AShr,
                Value::Temp(shifted),
                Value::Constant(8),
                IrType::I16,
            )
            .ir()?,
    ))
}

pub(super) fn i64_const(builder: &mut IrBuilder, value: i64) -> Result<Value, CompileError> {
    let temp = builder
        .build_binary(
            IrBinaryOp::Add,
            Value::Constant(value),
            Value::Constant(0),
            IrType::I64,
        )
        .ir()?;
    Ok(Value::Temp(temp))
}

pub(super) fn i32_const(builder: &mut IrBuilder, value: i64) -> Result<Value, CompileError> {
    let temp = builder
        .build_binary(
            IrBinaryOp::Add,
            Value::Constant(value),
            Value::Constant(0),
            IrType::I32,
        )
        .ir()?;
    Ok(Value::Temp(temp))
}

pub(super) fn wide_zero(builder: &mut IrBuilder, ty: IrType) -> Result<Value, CompileError> {
    if matches!(ty, IrType::I32 | IrType::I64) {
        let temp = builder
            .build_binary(IrBinaryOp::Add, Value::Constant(0), Value::Constant(0), ty)
            .ir()?;
        Ok(Value::Temp(temp))
    } else {
        Ok(Value::Constant(0))
    }
}

pub(super) fn arith_ty(ty: Ty) -> IrType {
    match ty {
        Ty::I8 | Ty::U8 | Ty::I16 | Ty::U16 => IrType::I16,
        Ty::I32 | Ty::U32 | Ty::Usize | Ty::F32 | Ty::Char => IrType::I32,
        Ty::I64 | Ty::U64 | Ty::F64 => IrType::I64,
        Ty::Bool
        | Ty::Unit
        | Ty::String
        | Ty::Adt(_)
        | Ty::Func(_)
        | Ty::Vec(_)
        | Ty::Owned(_)
        | Ty::ErasedOwner
        | Ty::Ref(_)
        | Ty::Ptr(_)
        | Ty::CFn(_)
        | Ty::Array { .. }
        | Ty::Slice(_)
        | Ty::Struct { .. }
        | Ty::Tuple { .. } => IrType::I16,
    }
}

pub(super) fn ir_op(op: BinOp, ty: Ty) -> IrBinaryOp {
    let unsigned = !ty.is_signed();
    match op {
        BinOp::Add => IrBinaryOp::Add,
        BinOp::Sub => IrBinaryOp::Sub,
        BinOp::Mul => IrBinaryOp::Mul,
        BinOp::Div => {
            if unsigned {
                IrBinaryOp::UDiv
            } else {
                IrBinaryOp::SDiv
            }
        }
        BinOp::Rem => {
            if unsigned {
                IrBinaryOp::URem
            } else {
                IrBinaryOp::SRem
            }
        }
        BinOp::Eq => IrBinaryOp::Eq,
        BinOp::Ne => IrBinaryOp::Ne,
        BinOp::Lt => {
            if unsigned {
                IrBinaryOp::Ult
            } else {
                IrBinaryOp::Slt
            }
        }
        BinOp::Le => {
            if unsigned {
                IrBinaryOp::Ule
            } else {
                IrBinaryOp::Sle
            }
        }
        BinOp::Gt => {
            if unsigned {
                IrBinaryOp::Ugt
            } else {
                IrBinaryOp::Sgt
            }
        }
        BinOp::Ge => {
            if unsigned {
                IrBinaryOp::Uge
            } else {
                IrBinaryOp::Sge
            }
        }
        BinOp::And | BinOp::Or => IrBinaryOp::And,
        BinOp::BitAnd => IrBinaryOp::And,
        BinOp::BitOr => IrBinaryOp::Or,
        BinOp::BitXor => IrBinaryOp::Xor,
        BinOp::Shl => IrBinaryOp::Shl,
        BinOp::Shr => {
            if unsigned {
                IrBinaryOp::LShr
            } else {
                IrBinaryOp::AShr
            }
        }
    }
}

pub(super) fn fold_bitwise(ty: Ty, op: BinOp, left: i64, right: i64) -> i64 {
    let width = match ty {
        Ty::I8 | Ty::U8 => 8,
        Ty::I16 | Ty::U16 => 16,
        Ty::I32 | Ty::U32 | Ty::Usize => 32,
        _ => 64,
    };
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
    bits as i64
}

pub(super) fn ir_ty(ty: Ty) -> IrType {
    match ty {
        Ty::I8 | Ty::U8 => IrType::I8,
        Ty::I16 | Ty::U16 => IrType::I16,
        Ty::I32 | Ty::U32 | Ty::Usize | Ty::F32 | Ty::Char => IrType::I32,
        Ty::I64 | Ty::U64 | Ty::F64 => IrType::I64,
        Ty::Bool => IrType::I1,
        Ty::Unit => IrType::Void,
        Ty::String
        | Ty::Adt(_)
        | Ty::Func(_)
        | Ty::Vec(_)
        | Ty::Owned(_)
        | Ty::ErasedOwner
        | Ty::Ref(_)
        | Ty::Ptr(_)
        | Ty::CFn(_)
        | Ty::Array { .. }
        | Ty::Slice(_)
        | Ty::Struct { .. }
        | Ty::Tuple { .. } => IrType::I16,
    }
}

pub(super) fn width(ty: Ty) -> u32 {
    match ty {
        Ty::I8 | Ty::U8 => 8,
        Ty::I16 | Ty::U16 | Ty::Bool => 16,
        Ty::I32 | Ty::U32 | Ty::Usize | Ty::F32 | Ty::Char => 32,
        Ty::I64 | Ty::U64 | Ty::F64 => 64,
        Ty::Unit
        | Ty::String
        | Ty::Adt(_)
        | Ty::Func(_)
        | Ty::Vec(_)
        | Ty::Owned(_)
        | Ty::ErasedOwner
        | Ty::Ref(_)
        | Ty::Ptr(_)
        | Ty::CFn(_)
        | Ty::Array { .. }
        | Ty::Slice(_)
        | Ty::Struct { .. }
        | Ty::Tuple { .. } => 0,
    }
}

pub(super) fn cell_wise(ty: Ty) -> bool {
    matches!(
        ty,
        Ty::String
            | Ty::Adt(_)
            | Ty::Func(_)
            | Ty::Vec(_)
            | Ty::Owned(_)
            | Ty::ErasedOwner
            | Ty::Ref(_)
            | Ty::Ptr(_)
            | Ty::CFn(_)
            | Ty::Array { .. }
            | Ty::Slice(_)
            | Ty::Struct { .. }
            | Ty::Tuple { .. }
    )
}
