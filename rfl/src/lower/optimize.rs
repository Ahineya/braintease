//! RFL-specific preparation for the shared SSA optimizer.
//!
//! Heap frames persist across suspension; ordinary scalar bindings need memory
//! only at those boundaries. Give nonescaping word bindings private local homes,
//! reload them on entry, and publish them immediately before setting RESUME.
//! The backend can then promote their loops and arithmetic to SSA. Managed,
//! borrowed, and shared slots keep their original addresses and ownership rules.

use rcc_ir::ir::Instruction;
use std::collections::{BTreeMap, HashMap, HashSet};

use super::*;

pub(super) fn retain_reachable(procs: &mut Vec<Proc>) {
    let indices: HashMap<_, _> = procs
        .iter()
        .enumerate()
        .map(|(i, p)| (p.name.clone(), i))
        .collect();
    let mut work: Vec<_> = procs
        .iter()
        .enumerate()
        .filter(|(_, p)| p.name == "main" || p.c_export)
        .map(|(i, _)| i)
        .collect();
    let mut live = HashSet::new();
    while let Some(index) = work.pop() {
        if !live.insert(index) {
            continue;
        }
        for seg in &procs[index].segs {
            if let Some(Term::Call { callee, .. } | Term::Tail { callee, .. }) = &seg.term {
                if let Some(index) = indices.get(callee) {
                    work.push(*index);
                }
            }
            for insn in &seg.insns {
                if let Insn::Closure { func, .. } = insn {
                    if let Some(index) = indices.get(func) {
                        work.push(*index);
                    }
                }
            }
        }
    }
    let mut index = 0;
    procs.retain(|_| {
        let keep = live.contains(&index);
        index += 1;
        keep
    });
    for (index, proc) in procs.iter_mut().enumerate() {
        proc.func_id = index;
    }
}

pub(super) fn promote_frame_words(function: &mut Function, proc: &Proc) {
    let live_at_resume = super::liveness::live_at_resume(proc);
    let borrowed: HashSet<_> = proc
        .segs
        .iter()
        .flat_map(|s| &s.insns)
        .filter_map(|i| match i {
            Insn::Borrow { slot, .. } => Some(*slot),
            _ => None,
        })
        .collect();
    let mut candidates: BTreeMap<_, _> = proc
        .slots
        .iter()
        .enumerate()
        .filter_map(|(slot, info)| {
            (!info.shared
                && !borrowed.contains(&slot)
                && proc.result_slot != Some(slot)
                && matches!(
                    info.ty,
                    Ty::I16
                        | Ty::U16
                        | Ty::I32
                        | Ty::U32
                        | Ty::I64
                        | Ty::U64
                        | Ty::F32
                        | Ty::F64
                        | Ty::Usize
                        | Ty::Char
                        | Ty::Bool
                ))
            .then_some((info.offset, slot))
        })
        .collect();
    if candidates.is_empty() {
        return;
    }
    let frame = fat_temp(0, BankTag::Mixed);
    let offsets: HashMap<_, _> = function
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter_map(|i| match i {
            Instruction::GetElementPtr {
                result,
                ptr,
                indices,
                source_element_type: IrType::I16,
                ..
            } if *ptr == frame => match indices.as_slice() {
                [Value::Constant(offset)] => Some((*result, *offset)),
                _ => None,
            },
            _ => None,
        })
        .collect();
    let offset_of = |ptr: &Value| match ptr {
        Value::FatPtr(p) => match p.addr.as_ref() {
            Value::Temp(id) => offsets.get(id).copied(),
            _ => None,
        },
        _ => None,
    };
    // Inline aggregates and the tail-call protocol sometimes access words of a
    // scalar separately. Keep those slots in memory rather than split an alias.
    for inst in function.blocks.iter().flat_map(|b| &b.instructions) {
        let access = match inst {
            Instruction::Load {
                ptr,
                result_type,
                volatile,
                ..
            } => Some((ptr, result_type, *volatile)),
            Instruction::Store {
                ptr,
                value_type,
                volatile,
                ..
            } => Some((ptr, value_type, *volatile)),
            _ => None,
        };
        if let Some((ptr, ty, volatile)) = access {
            if let Some(at) = offset_of(ptr) {
                candidates.retain(|offset, slot| {
                    let info = &proc.slots[*slot];
                    !(at >= *offset
                        && at < *offset + info.ty.cells()
                        && (at != *offset || *ty != ir_ty(info.ty) || volatile))
                });
            }
        }
    }
    let mut next = function
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter_map(result_id)
        .chain(function.parameters.iter().map(|(id, _)| *id))
        .max()
        .unwrap_or(0)
        + 1;
    let mut fresh = || {
        let id = next;
        next += 1;
        id
    };
    let mut homes = BTreeMap::new();
    let mut entry = Vec::new();
    for (offset, slot) in &candidates {
        let ty = ir_ty(proc.slots[*slot].ty);
        let local = fresh();
        let addr = fresh();
        let value = fresh();
        let home = fat_temp(local, BankTag::Stack);
        entry.push(Instruction::Alloca {
            result: local,
            alloc_type: ty.clone(),
            count: None,
            result_type: ptr_ty(),
        });
        entry.push(frame_gep(addr, *offset));
        entry.push(Instruction::Load {
            result: value,
            ptr: fat_temp(addr, BankTag::Mixed),
            result_type: ty.clone(),
            volatile: false,
        });
        entry.push(Instruction::Store {
            value: Value::Temp(value),
            ptr: home.clone(),
            value_type: ty.clone(),
            volatile: false,
        });
        homes.insert(*offset, (home, ty));
    }
    for block in &mut function.blocks {
        let old = std::mem::take(&mut block.instructions);
        for mut inst in old {
            // This is the publication point used by direct and indirect RFL calls.
            if let Instruction::Store {
                ptr, value: resume, ..
            } = &inst
            {
                if offset_of(ptr) == Some(RESUME) {
                    for (offset, (home, ty)) in &homes {
                        let slot = candidates[offset];
                        let live = match resume {
                            Value::Constant(id) => usize::try_from(*id)
                                .ok()
                                .and_then(|id| live_at_resume.get(id)),
                            _ => None,
                        };
                        if live.is_some_and(|live| !live.contains(&slot)) {
                            continue;
                        }
                        let value = fresh();
                        let addr = fresh();
                        block.instructions.push(Instruction::Load {
                            result: value,
                            ptr: home.clone(),
                            result_type: ty.clone(),
                            volatile: false,
                        });
                        block.instructions.push(frame_gep(addr, *offset));
                        block.instructions.push(Instruction::Store {
                            value: Value::Temp(value),
                            ptr: fat_temp(addr, BankTag::Mixed),
                            value_type: ty.clone(),
                            volatile: false,
                        });
                    }
                }
            }
            match &mut inst {
                Instruction::Load {
                    ptr,
                    volatile: false,
                    ..
                }
                | Instruction::Store {
                    ptr,
                    volatile: false,
                    ..
                } => {
                    if let Some(home) = offset_of(ptr).and_then(|offset| homes.get(&offset)) {
                        *ptr = home.0.clone();
                    }
                }
                _ => {}
            }
            block.instructions.push(inst);
        }
    }
    entry.append(&mut function.blocks[0].instructions);
    function.blocks[0].instructions = entry;
}

fn frame_gep(result: u32, offset: i64) -> Instruction {
    Instruction::GetElementPtr {
        result,
        ptr: fat_temp(0, BankTag::Mixed),
        indices: vec![Value::Constant(offset)],
        result_type: ptr_ty(),
        source_element_type: IrType::I16,
        is_struct_field: false,
    }
}

fn result_id(inst: &Instruction) -> Option<u32> {
    match inst {
        Instruction::Binary { result, .. }
        | Instruction::Unary { result, .. }
        | Instruction::Load { result, .. }
        | Instruction::GetElementPtr { result, .. }
        | Instruction::Alloca { result, .. }
        | Instruction::Phi { result, .. }
        | Instruction::Cast { result, .. }
        | Instruction::Select { result, .. } => Some(*result),
        Instruction::Call { result, .. } | Instruction::Intrinsic { result, .. } => *result,
        _ => None,
    }
}

/// Emit definitions before blocks they dominate. The backend carries pointer
/// bank facts while emitting blocks, so preallocated join labels must not force
/// joins to appear before their incoming arms. Call continuations are edges in
/// this graph even though the driver, rather than a native branch, reaches them.
pub(super) fn segment_order(proc: &Proc) -> Vec<usize> {
    let mut seen = HashSet::new();
    let mut postorder = Vec::new();
    let mut work = vec![(0, false)];
    while let Some((index, leave)) = work.pop() {
        if leave {
            postorder.push(index);
            continue;
        }
        if !seen.insert(index) {
            continue;
        }
        work.push((index, true));
        match proc.segs[index].term.as_ref().unwrap() {
            Term::Jump(next) | Term::Call { next, .. } | Term::Apply { next, .. } => {
                work.push((*next, false))
            }
            Term::If {
                then_seg, else_seg, ..
            }
            | Term::Tag {
                then_seg, else_seg, ..
            }
            | Term::Lit {
                then_seg, else_seg, ..
            } => {
                work.push((*else_seg, false));
                work.push((*then_seg, false));
            }
            _ => {}
        }
    }
    postorder.reverse();
    postorder
}
