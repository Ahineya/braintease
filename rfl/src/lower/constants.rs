//! Propagate immutable scalar values before software-float calls hide them.
//!
//! Do not reassociate arithmetic or approximate division with reciprocals. Host
//! binary32/binary64 folds are restricted to finite normal inputs/results (and
//! zero). Both runtime formats use round-to-nearest-even, as the host does.
use super::*;

pub(super) fn fold_scalar_constants(proc: &mut Proc) {
    let mut writes = vec![0usize; proc.slots.len()];
    for &slot in &proc.param_slots {
        writes[slot] += 1;
    }
    for cap in &proc.captures {
        writes[cap.slot] += 1;
    }
    for seg in &proc.segs {
        for insn in &seg.insns {
            for slot in destinations(insn) {
                writes[slot] += 1;
            }
            if let Insn::Borrow { slot, .. } = insn {
                writes[*slot] += 2;
            }
        }
        if let Some(Term::Call { dest, .. } | Term::Apply { dest, .. }) = &seg.term {
            writes[*dest] += 1;
        }
    }
    let mut known = vec![None; proc.slots.len()];
    loop {
        let mut changed = false;
        for seg in &proc.segs {
            for insn in &seg.insns {
                let Some((slot, value)) = evaluate(&proc.slots, insn, &known) else {
                    continue;
                };
                if writes[slot] == 1 && !proc.slots[slot].shared && known[slot].is_none() {
                    known[slot] = Some(value);
                    changed = true;
                }
            }
        }
        if !changed {
            break;
        }
    }
    // Slot types and constant facts stay immutable during rewriting.
    let slots = &proc.slots;
    for seg in &mut proc.segs {
        for insn in &mut seg.insns {
            if let Some((slot, value)) = evaluate(slots, insn, &known) {
                *insn = Insn::Const { slot, value };
            }
        }
    }
}

fn evaluate(slots: &[Slot], insn: &Insn, known: &[Option<i64>]) -> Option<(usize, i64)> {
    let (dest, value) = match insn {
        Insn::Const { slot, value } => (*slot, *value),
        Insn::Copy { dest, src } if !slots[*src].shared => (*dest, known[*src]?),
        Insn::Neg { dest, src } if slots[*src].ty == Ty::F64 => (*dest, known[*src]? ^ i64::MIN),
        Insn::Neg { dest, src } if slots[*src].ty == Ty::F32 => {
            (*dest, known[*src]? ^ (1i64 << 31))
        }
        Insn::Bin { dest, op, lhs, rhs } if slots[*lhs].ty.is_float() => {
            let ty = slots[*lhs].ty;
            let a = float_value(ty, known[*lhs]?);
            let b = float_value(ty, known[*rhs]?);
            if !normal_or_zero(ty, a) || !normal_or_zero(ty, b) {
                return None;
            }
            let value = match op {
                BinOp::Eq => return Some((*dest, i64::from(a == b))),
                BinOp::Ne => return Some((*dest, i64::from(a != b))),
                BinOp::Lt => return Some((*dest, i64::from(a < b))),
                BinOp::Le => return Some((*dest, i64::from(a <= b))),
                BinOp::Gt => return Some((*dest, i64::from(a > b))),
                BinOp::Ge => return Some((*dest, i64::from(a >= b))),
                BinOp::Add | BinOp::Sub | BinOp::Mul | BinOp::Div
                    if *op != BinOp::Div || b != 0.0 =>
                {
                    if ty == Ty::F32 {
                        // Operate at binary32 precision, including the division.
                        let (a, b) = (a as f32, b as f32);
                        f64::from(match op {
                            BinOp::Add => a + b,
                            BinOp::Sub => a - b,
                            BinOp::Mul => a * b,
                            BinOp::Div => a / b,
                            _ => unreachable!(),
                        })
                    } else {
                        match op {
                            BinOp::Add => a + b,
                            BinOp::Sub => a - b,
                            BinOp::Mul => a * b,
                            BinOp::Div => a / b,
                            _ => unreachable!(),
                        }
                    }
                }
                _ => return None,
            };
            if !normal_or_zero(ty, value) {
                return None;
            }
            (*dest, float_bits(ty, value))
        }
        Insn::Cast {
            dest,
            src,
            from,
            to,
        } if from.is_int() && to.is_float() => {
            let limit = 1u64 << if *to == Ty::F32 { 24 } else { 53 };
            let raw = from.bits(known[*src]?);
            let value = if from.is_signed() {
                if raw.unsigned_abs() > limit {
                    return None;
                }
                raw as f64
            } else {
                let unsigned = bits(*from, raw);
                if unsigned > limit {
                    return None;
                }
                unsigned as f64
            };
            (*dest, float_bits(*to, value))
        }
        Insn::Cast {
            dest,
            src,
            from,
            to,
        } if from.is_float() && to.is_float() => {
            let value = float_value(*from, known[*src]?);
            if !normal_or_zero(*from, value) || !normal_or_zero(*to, value) {
                return None;
            }
            (*dest, float_bits(*to, value))
        }
        _ => return None,
    };
    if slots[dest].shared || cell_wise(slots[dest].ty) {
        return None;
    }
    Some((dest, value))
}

fn normal_or_zero(ty: Ty, value: f64) -> bool {
    if ty == Ty::F32 {
        let value = value as f32;
        value.is_normal() || value == 0.0
    } else {
        value.is_normal() || value == 0.0
    }
}

fn float_value(ty: Ty, bits: i64) -> f64 {
    if ty == Ty::F32 {
        f64::from(f32::from_bits(bits as u32))
    } else {
        f64::from_bits(bits as u64)
    }
}

fn float_bits(ty: Ty, value: f64) -> i64 {
    if ty == Ty::F32 {
        i64::from((value as f32).to_bits())
    } else {
        value.to_bits() as i64
    }
}
