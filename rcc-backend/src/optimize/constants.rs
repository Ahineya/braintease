use rcc_common::TempId;
use rcc_frontend::ir::{Function, Instruction, IrBinaryOp as B, IrType, IrUnaryOp as U, Value};
use std::collections::HashMap;

fn width(ty: &IrType) -> Option<u32> {
    match ty {
        IrType::I1 => Some(1),
        IrType::I8 => Some(8),
        IrType::I16 => Some(16),
        IrType::I32 => Some(32),
        IrType::I64 => Some(64),
        _ => None,
    }
}
fn mask(bits: u32) -> u64 {
    u64::MAX >> (64 - bits)
}
fn signed(value: u64, bits: u32) -> i64 {
    ((value << (64 - bits)) as i64) >> (64 - bits)
}
pub(super) fn comparison(op: B) -> bool {
    matches!(
        op,
        B::Eq | B::Ne | B::Slt | B::Sle | B::Sgt | B::Sge | B::Ult | B::Ule | B::Ugt | B::Uge
    )
}

/// Evaluate in the target's integer width, not the compiler host's width.
/// Undefined divisions and shifts are deliberately left for normal lowering.
pub fn fold_binary(op: B, lhs: i64, rhs: i64, ty: &IrType) -> Option<i64> {
    let bits = width(ty)?;
    let a = lhs as u64 & mask(bits);
    let b = rhs as u64 & mask(bits);
    let sa = signed(a, bits);
    let sb = signed(b, bits);
    let boolean = match op {
        B::Eq => Some(a == b),
        B::Ne => Some(a != b),
        B::Slt => Some(sa < sb),
        B::Sle => Some(sa <= sb),
        B::Sgt => Some(sa > sb),
        B::Sge => Some(sa >= sb),
        B::Ult => Some(a < b),
        B::Ule => Some(a <= b),
        B::Ugt => Some(a > b),
        B::Uge => Some(a >= b),
        _ => None,
    };
    if let Some(b) = boolean {
        return Some(i64::from(b));
    }
    let out = match op {
        B::Add => a.wrapping_add(b),
        B::Sub => a.wrapping_sub(b),
        B::Mul => a.wrapping_mul(b),
        B::And => a & b,
        B::Or => a | b,
        B::Xor => a ^ b,
        B::UDiv if b != 0 => a / b,
        B::URem if b != 0 => a % b,
        B::SDiv | B::SRem if sb != 0 => {
            if sa == signed(1u64 << (bits - 1), bits) && sb == -1 {
                return None;
            }
            if op == B::SDiv {
                (sa / sb) as u64
            } else {
                (sa % sb) as u64
            }
        }
        B::Shl if rhs >= 0 && (rhs as u64) < u64::from(bits) => a << rhs,
        B::LShr if rhs >= 0 && (rhs as u64) < u64::from(bits) => a >> rhs,
        B::AShr if rhs >= 0 && (rhs as u64) < u64::from(bits) => (sa >> rhs) as u64,
        _ => return None,
    };
    let out = out & mask(bits);
    Some(if bits < 16 {
        out as i64
    } else {
        signed(out, bits)
    })
}

fn constant(value: &Value, known: &HashMap<TempId, i64>) -> Option<i64> {
    match value {
        Value::Constant(c) => Some(*c),
        Value::Temp(t) => known.get(t).copied(),
        _ => None,
    }
}

fn value_width(value: &Value, types: &HashMap<TempId, IrType>) -> Option<u32> {
    match value {
        Value::Temp(t) => types.get(t).and_then(width),
        Value::Constant(c) if (-32768..=65535).contains(c) => Some(16),
        Value::Constant(c) if (i32::MIN as i64..=u32::MAX as i64).contains(c) => Some(32),
        Value::Constant(_) => Some(64),
        _ => None,
    }
}

fn evaluate(
    inst: &Instruction,
    known: &HashMap<TempId, i64>,
    types: &HashMap<TempId, IrType>,
) -> Option<(i64, IrType)> {
    match inst {
        Instruction::Binary {
            op,
            lhs,
            rhs,
            result_type,
            ..
        } => {
            let (a, b) = (constant(lhs, known)?, constant(rhs, known)?);
            // Boolean conversion declares an I16 result even when comparing
            // an I32/I64 operand. Keep the operand width until after comparing.
            let compare_type = match value_width(lhs, types)
                .unwrap_or(16)
                .max(value_width(rhs, types).unwrap_or(16))
                .max(width(result_type)?)
            {
                64 => IrType::I64,
                32 => IrType::I32,
                _ => IrType::I16,
            };
            let value = fold_binary(
                *op,
                a,
                b,
                if comparison(*op) {
                    &compare_type
                } else {
                    result_type
                },
            )?;
            Some((
                value,
                if comparison(*op) {
                    IrType::I16
                } else {
                    result_type.clone()
                },
            ))
        }
        Instruction::Unary {
            op,
            operand,
            result_type,
            ..
        } => {
            let bits = width(result_type)?;
            let value = constant(operand, known)?;
            let out = match op {
                U::Not => !value,
                U::Neg => value.wrapping_neg(),
                U::Trunc => value,
                // Untyped literals do not record their source width. For
                // example 65535L is positive I32, not negative I16. Retain
                // normal lowering unless a typed SSA operand proves width.
                U::ZExt if matches!(operand, Value::Temp(_)) => {
                    (value as u64 & mask(value_width(operand, types)?)) as i64
                }
                U::SExt if matches!(operand, Value::Temp(_)) => {
                    signed(value as u64, value_width(operand, types)?)
                }
                _ => return None,
            };
            let out = out as u64 & mask(bits);
            Some((
                if bits < 16 {
                    out as i64
                } else {
                    signed(out, bits)
                },
                result_type.clone(),
            ))
        }
        _ => None,
    }
}

pub(super) fn simplify(function: &mut Function) {
    let mut types: HashMap<_, _> = function.parameters.iter().cloned().collect();
    for inst in function.blocks.iter().flat_map(|b| &b.instructions) {
        let ty = match inst {
            Instruction::Binary {
                op, result_type, ..
            } => Some(if comparison(*op) {
                IrType::I16
            } else {
                result_type.clone()
            }),
            Instruction::Unary { result_type, .. }
            | Instruction::Load { result_type, .. }
            | Instruction::Call { result_type, .. }
            | Instruction::Phi { result_type, .. }
            | Instruction::Select { result_type, .. }
            | Instruction::Intrinsic { result_type, .. } => Some(result_type.clone()),
            Instruction::Cast { target_type, .. } => Some(target_type.clone()),
            _ => None,
        };
        if let (Some(id), Some(ty)) = (super::result(inst), ty) {
            types.insert(id, ty);
        }
    }
    // IR temporaries are single-assignment. A bounded fixed point also handles
    // block layouts where a dominating definition is emitted after its use.
    let mut known = HashMap::new();
    loop {
        let before = known.len();
        for inst in function.blocks.iter().flat_map(|b| &b.instructions) {
            if let Some(id) = super::result(inst) {
                if let Some((c, _)) = evaluate(inst, &known, &types) {
                    known.insert(id, c);
                }
            }
        }
        if before == known.len() {
            break;
        }
    }
    for inst in function.blocks.iter_mut().flat_map(|b| &mut b.instructions) {
        if let Some((c, ty)) = evaluate(inst, &known, &types) {
            // Keep the typed temporary: replacing a long call argument with a
            // bare Constant would silently change its ABI to a single word.
            *inst = Instruction::Binary {
                result: super::result(inst).unwrap(),
                op: B::Add,
                lhs: Value::Constant(c),
                rhs: Value::Constant(0),
                result_type: ty,
            };
            continue;
        }
        match inst {
            Instruction::GetElementPtr { indices, .. } => {
                for index in indices {
                    if let Some(c) = constant(index, &known) {
                        *index = Value::Constant(c);
                    }
                }
            }
            Instruction::Binary {
                op,
                lhs,
                rhs,
                result_type,
                ..
            } if result_type.is_integer() => {
                // Powers of two use native shifts/masks even for multiword C
                // integers. Signed division is intentionally not rewritten.
                if *op == B::Mul
                    && constant(lhs, &known).is_some()
                    && constant(rhs, &known).is_none()
                {
                    std::mem::swap(lhs, rhs);
                }
                if let Some(c) = constant(rhs, &known) {
                    let bits = width(result_type).unwrap();
                    let unsigned = c as u64 & mask(bits);
                    if unsigned.is_power_of_two() {
                        match op {
                            B::Mul => {
                                *op = B::Shl;
                                *rhs = Value::Constant(unsigned.trailing_zeros() as i64);
                            }
                            B::UDiv => {
                                *op = B::LShr;
                                *rhs = Value::Constant(unsigned.trailing_zeros() as i64);
                            }
                            B::URem => {
                                *op = B::And;
                                *rhs = Value::Constant((unsigned - 1) as i64);
                            }
                            _ => {}
                        }
                    }
                }
            }
            _ => {}
        }
    }
}
