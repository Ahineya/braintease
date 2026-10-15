//! Propagate immutable scalar values before software-float calls hide them.
//!
//! Do not reassociate arithmetic or approximate division with reciprocals. Host
//! binary64 folds are restricted to finite normal inputs/results (and zero).
//! The target binary32 helpers have different rounding, so leave them alone.
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
        Insn::Bin { dest, op, lhs, rhs } if slots[*lhs].ty == Ty::F64 => {
            let a = f64::from_bits(known[*lhs]? as u64);
            let b = f64::from_bits(known[*rhs]? as u64);
            if !normal_or_zero(a) || !normal_or_zero(b) {
                return None;
            }
            let value = match op {
                BinOp::Eq => return Some((*dest, i64::from(a == b))),
                BinOp::Ne => return Some((*dest, i64::from(a != b))),
                BinOp::Lt => return Some((*dest, i64::from(a < b))),
                BinOp::Le => return Some((*dest, i64::from(a <= b))),
                BinOp::Gt => return Some((*dest, i64::from(a > b))),
                BinOp::Ge => return Some((*dest, i64::from(a >= b))),
                BinOp::Add => a + b,
                BinOp::Sub => a - b,
                BinOp::Mul => a * b,
                BinOp::Div if b != 0.0 => a / b,
                _ => return None,
            };
            if !normal_or_zero(value) {
                return None;
            }
            (*dest, value.to_bits() as i64)
        }
        Insn::Cast {
            dest,
            src,
            from,
            to: Ty::F64,
        } if from.is_int() => {
            let raw = from.bits(known[*src]?);
            let value = if from.is_signed() {
                if raw.unsigned_abs() > (1u64 << 53) {
                    return None;
                }
                raw as f64
            } else {
                let unsigned = bits(*from, raw);
                if unsigned > (1u64 << 53) {
                    return None;
                }
                unsigned as f64
            };
            (*dest, value.to_bits() as i64)
        }
        _ => return None,
    };
    if slots[dest].shared || cell_wise(slots[dest].ty) {
        return None;
    }
    Some((dest, value))
}

fn normal_or_zero(value: f64) -> bool {
    value.is_normal() || value == 0.0
}
