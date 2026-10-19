//! End single-use operand lifetimes immediately after their consumer.
//!
//! Expression snapshots preserve left-to-right evaluation, but keeping their
//! managed shares until frame exit makes an ordinary indexed write detach the
//! entire vector. Only compiler temporaries with one static consumer qualify.
//! Bindings, borrowed addresses, results and suspension operands remain owned
//! by the frame. Clearing the released pointers keeps exit cleanup idempotent.
use super::*;
use std::collections::HashSet;

pub(super) fn consumed_temporaries(proc: &Proc) -> HashSet<usize> {
    let mut uses = vec![0usize; proc.slots.len()];
    let mut excluded: HashSet<_> = proc.binding_slots.iter().copied().collect();
    excluded.extend(proc.result_slot);
    for seg in &proc.segs {
        for insn in &seg.insns {
            if let Insn::Borrow { place, .. } = insn {
                excluded.insert(place.root);
            }
            // A cleanup is a lifetime boundary, not an additional value use.
            if !matches!(insn, Insn::Drop { .. }) {
                for slot in operands(insn) {
                    uses[slot] += 1;
                }
            }
        }
        match seg.term.as_ref().unwrap() {
            Term::Call { args, .. } | Term::Tail { args, .. } => excluded.extend(args),
            Term::Apply { slot, args, .. } => {
                excluded.insert(*slot);
                excluded.extend(args);
            }
            Term::If { cond, .. } => {
                excluded.insert(*cond);
            }
            Term::AsyncPoll {
                future, context, ..
            } => {
                excluded.extend([*future, *context]);
            }
            Term::Tag { slot, .. } | Term::Lit { slot, .. } => {
                excluded.insert(*slot);
            }
            _ => {}
        }
    }
    uses.into_iter()
        .enumerate()
        .filter_map(|(i, n)| {
            (n == 1 && !excluded.contains(&i) && keeps(proc, proc.slots[i].ty)).then_some(i)
        })
        .collect()
}

pub(super) fn operands(insn: &Insn) -> Vec<usize> {
    match insn {
        Insn::Expect { src, message, .. } => {
            let mut result = vec![*src];
            if let ExpectMessage::Slot(slot) = message {
                result.push(*slot);
            }
            result
        }
        Insn::AsyncNew { args, .. } => args.clone(),
        Insn::Closure { captures, .. } => captures.clone(),
        Insn::AsyncCancel { future } => vec![*future],
        Insn::Replace { place, value, .. } => vec![*place, *value],
        Insn::IntoInner { src, .. } | Insn::EraseOwner { src, .. } => vec![*src],
        Insn::Move { src, .. } => vec![*src],
        Insn::Drop { slot } => vec![*slot],
        Insn::Owned { value, cleanup, .. } => vec![*value, *cleanup],
        Insn::Copy { src, .. }
        | Insn::Neg { src, .. }
        | Insn::Not { src, .. }
        | Insn::BitNot { src, .. }
        | Insn::Cast { src, .. }
        | Insn::Read { src, .. }
        | Insn::ToVec { src, .. }
        | Insn::Take { src, .. }
        | Insn::StrFromChar { src, .. }
        | Insn::StrFromByte { src, .. } => vec![*src],
        Insn::VecStore { src, .. } | Insn::FieldStore { src, .. } => vec![*src],
        Insn::Bin { lhs, rhs, .. } | Insn::StrConcat { lhs, rhs, .. } => vec![*lhs, *rhs],
        Insn::CApply { callee, args, .. } => {
            let mut v = args.clone();
            v.push(*callee);
            v
        }
        Insn::Extern { args, .. } => args.clone(),
        Insn::Make { fields, .. } => fields.iter().map(|(s, _)| *s).collect(),
        Insn::Record { fields, .. } => fields.clone(),
        Insn::VecLit { elems, .. } | Insn::ArrayLit { elems, .. } => elems.clone(),
        Insn::Print { slot, .. } | Insn::Putc { slot } => vec![*slot],
        Insn::SliceNew {
            base, start, end, ..
        }
        | Insn::StrSlice {
            base, start, end, ..
        } => vec![*base, *start, *end],
        Insn::Index { vec, index, .. } => vec![*vec, *index],
        Insn::Len { vec, .. } | Insn::Empty { vec, .. } | Insn::Clear { vec, .. } => vec![*vec],
        Insn::Pop { vec, value, .. } => vec![*vec, *value],
        Insn::Get {
            vec, index, value, ..
        } => vec![*vec, *index, *value],
        Insn::Push { vec, value, .. } => vec![*vec, *value],
        Insn::IndexStore {
            vec, index, value, ..
        } => vec![*vec, *index, *value],
        Insn::Try { src, fail_slot, .. } => {
            let mut v = vec![*src];
            v.extend(fail_slot);
            v
        }
        Insn::Borrow { place, .. } => {
            let mut slots = vec![place.root];
            slots.extend(place.projections.iter().filter_map(|step| match step {
                PlaceStep::Index { index, .. } => Some(*index),
                _ => None,
            }));
            slots
        }
        Insn::PtrLoad { ptr, index, .. } => {
            let mut v = vec![*ptr];
            v.extend(index);
            v
        }
        Insn::PtrStore {
            ptr, index, src, ..
        } => {
            let mut v = vec![*ptr, *src];
            v.extend(index);
            v
        }
        Insn::PtrOff { ptr, index, .. } => vec![*ptr, *index],
        Insn::PtrWord { ptr, .. } | Insn::PtrNull { ptr, .. } => vec![*ptr],
        Insn::VecFill { .. }
        | Insn::Const { .. }
        | Insn::BoxNew { .. }
        | Insn::CFn { .. }
        | Insn::PrintLit { .. }
        | Insn::CStr { .. }
        | Insn::Str { .. } => vec![],
    }
}

/// Slots assigned by an instruction, including mutations and scratch results.
pub(super) fn destinations(insn: &Insn) -> Vec<usize> {
    match insn {
        Insn::Expect { dest, .. } => vec![*dest],
        Insn::AsyncNew { dest, .. } => vec![*dest],
        Insn::AsyncCancel { .. } => vec![],
        Insn::Replace { dest, value, .. } => vec![*dest, *value],
        Insn::IntoInner { dest, src } | Insn::EraseOwner { dest, src, .. } => vec![*dest, *src],
        Insn::Move { dest, src } => vec![*dest, *src],
        Insn::Drop { slot } => vec![*slot],
        Insn::Owned { dest, .. } => vec![*dest],
        Insn::Const { slot, .. } | Insn::BoxNew { slot } => vec![*slot],
        Insn::Copy { dest, .. }
        | Insn::Bin { dest, .. }
        | Insn::Neg { dest, .. }
        | Insn::Not { dest, .. }
        | Insn::BitNot { dest, .. }
        | Insn::CFn { dest, .. }
        | Insn::CApply { dest, .. }
        | Insn::Cast { dest, .. }
        | Insn::Make { dest, .. }
        | Insn::Read { dest, .. }
        | Insn::Closure { dest, .. }
        | Insn::VecLit { dest, .. }
        | Insn::VecFill { dest, .. }
        | Insn::ArrayLit { dest, .. }
        | Insn::SliceNew { dest, .. }
        | Insn::ToVec { dest, .. }
        | Insn::Index { dest, .. }
        | Insn::Len { dest, .. }
        | Insn::Empty { dest, .. }
        | Insn::Borrow { dest, .. }
        | Insn::VecStore { dest, .. }
        | Insn::Record { dest, .. }
        | Insn::Take { dest, .. }
        | Insn::FieldStore { dest, .. }
        | Insn::Extern { dest, .. }
        | Insn::StrConcat { dest, .. }
        | Insn::StrSlice { dest, .. }
        | Insn::StrFromChar { dest, .. }
        | Insn::StrFromByte { dest, .. }
        | Insn::CStr { dest, .. }
        | Insn::Str { dest, .. }
        | Insn::PtrLoad { dest, .. }
        | Insn::PtrOff { dest, .. }
        | Insn::PtrWord { dest, .. }
        | Insn::PtrNull { dest, .. } => vec![*dest],
        Insn::Clear { vec, .. } | Insn::Push { vec, .. } | Insn::IndexStore { vec, .. } => {
            vec![*vec]
        }
        Insn::Pop {
            vec, value, dest, ..
        } => vec![*vec, *value, *dest],
        Insn::Get { value, dest, .. } => vec![*value, *dest],
        Insn::Try {
            dest,
            result,
            fail_slot,
            ..
        } => {
            let mut slots = vec![*dest, *result];
            slots.extend(fail_slot);
            slots
        }
        Insn::Print { .. } | Insn::PrintLit { .. } | Insn::Putc { .. } | Insn::PtrStore { .. } => {
            vec![]
        }
    }
}
