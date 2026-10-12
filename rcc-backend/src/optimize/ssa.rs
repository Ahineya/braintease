//! Pruned SSA promotion for nonescaping integer locals. A local is promoted only
//! when every read is definitely initialized; volatile/address-taken storage is
//! never rewritten. Phi nodes remain until all SSA optimizations have finished.

use super::cfg::{next_temp, Cfg};
use rcc_common::TempId;
use rcc_frontend::{
    ir::{Function, Instruction, IrBinaryOp, IrType, Value},
    BankTag,
};
use std::collections::{BTreeMap, BTreeSet};

fn address(value: &Value) -> Option<TempId> {
    match value {
        Value::Temp(id) => Some(*id),
        Value::FatPtr(pointer) if pointer.bank == BankTag::Stack => match pointer.addr.as_ref() {
            Value::Temp(id) => Some(*id),
            _ => None,
        },
        _ => None,
    }
}

#[cfg(test)]
mod tests;

struct Candidate {
    ty: IrType,
    block: usize,
    position: usize,
    escaped: bool,
}

pub(super) fn promote_integer_locals(function: &mut Function) {
    let Ok(cfg) = Cfg::new(function) else {
        return;
    };
    if function.blocks.is_empty() || !cfg.predecessors[0].is_empty() {
        return;
    }
    let Some(mut next) = next_temp(function) else {
        return;
    };
    let n = function.blocks.len();
    let mut candidates = BTreeMap::new();
    for (bi, block) in function.blocks.iter().enumerate() {
        for (ii, inst) in block.instructions.iter().enumerate() {
            if let Instruction::Alloca {
                result,
                alloc_type,
                count,
                ..
            } = inst
            {
                if alloc_type.is_integer() && matches!(count, None | Some(Value::Constant(1))) {
                    candidates.insert(
                        *result,
                        Candidate {
                            ty: alloc_type.clone(),
                            block: bi,
                            position: ii,
                            escaped: false,
                        },
                    );
                }
            }
        }
    }
    for (bi, block) in function.blocks.iter().enumerate() {
        for (ii, inst) in block.instructions.iter().enumerate() {
            let mut uses = super::used_temps(inst);
            let access = match inst {
                Instruction::Load {
                    ptr,
                    result_type,
                    volatile: false,
                    ..
                } => Some((ptr, result_type)),
                Instruction::Store {
                    ptr,
                    value_type,
                    volatile: false,
                    ..
                } => Some((ptr, value_type)),
                _ => None,
            };
            if let Some((ptr, ty)) = access {
                if let Some(id) = address(ptr) {
                    if let Some(candidate) = candidates.get_mut(&id) {
                        if ty == &candidate.ty
                            && cfg.reachable.contains(&bi)
                            && cfg.dominators[bi].contains(&candidate.block)
                            && (bi != candidate.block || ii > candidate.position)
                        {
                            uses.remove(&id);
                            // The value of a store can still leak this address.
                            if let Instruction::Store { value, .. } = inst {
                                super::value_temps(value, &mut uses);
                            }
                        }
                    }
                }
            }
            for id in uses {
                if let Some(candidate) = candidates.get_mut(&id) {
                    candidate.escaped = true;
                }
            }
        }
    }
    candidates
        .retain(|_, candidate| !candidate.escaped && cfg.reachable.contains(&candidate.block));
    let mut frontier = vec![BTreeSet::new(); n];
    for &b in &cfg.reachable {
        let preds: Vec<_> = cfg.predecessors[b]
            .iter()
            .copied()
            .filter(|p| cfg.reachable.contains(p))
            .collect();
        if preds.len() < 2 {
            continue;
        }
        for p in preds {
            let mut runner = Some(p);
            while runner != cfg.idom[b] {
                let Some(r) = runner else {
                    break;
                };
                frontier[r].insert(b);
                runner = cfg.idom[r];
            }
        }
    }
    let mut children = vec![vec![]; n];
    for &b in &cfg.reachable {
        if let Some(parent) = cfg.idom[b] {
            children[parent].push(b);
        }
    }

    for (id, candidate) in candidates {
        let mut defs = vec![false; n];
        let mut reads_before_def = vec![false; n];
        for (bi, block) in function.blocks.iter().enumerate() {
            for inst in &block.instructions {
                match inst {
                    Instruction::Store { ptr, .. } if address(ptr) == Some(id) => defs[bi] = true,
                    Instruction::Load { ptr, .. } if address(ptr) == Some(id) && !defs[bi] => {
                        reads_before_def[bi] = true
                    }
                    _ => {}
                }
            }
        }
        // Greatest fixed point for must-definition, with entry uninitialized.
        let mut assigned_in = vec![true; n];
        assigned_in[0] = false;
        loop {
            let mut changed = false;
            for &b in &cfg.reachable {
                let assigned = b != 0
                    && cfg.predecessors[b]
                        .iter()
                        .filter(|p| cfg.reachable.contains(p))
                        .all(|&p| assigned_in[p] || defs[p]);
                if assigned_in[b] != assigned {
                    assigned_in[b] = assigned;
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
        if cfg
            .reachable
            .iter()
            .any(|&b| reads_before_def[b] && !assigned_in[b])
        {
            continue;
        }
        let mut live_in = reads_before_def.clone();
        loop {
            let mut changed = false;
            for &b in cfg.reachable.iter().rev() {
                let live = reads_before_def[b]
                    || (!defs[b] && cfg.successors[b].iter().any(|&s| live_in[s]));
                if live != live_in[b] {
                    live_in[b] = live;
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
        let mut phi_blocks = BTreeSet::new();
        let mut work: Vec<_> = cfg.reachable.iter().copied().filter(|&b| defs[b]).collect();
        while let Some(b) = work.pop() {
            for &join in &frontier[b] {
                if live_in[join] && phi_blocks.insert(join) {
                    work.push(join);
                }
            }
        }
        // Do not invent values on unreachable incoming edges. Keeping memory
        // for this uncommon malformed/dead-edge shape is safe and predictable.
        if phi_blocks.iter().any(|&b| {
            cfg.predecessors[b]
                .iter()
                .any(|p| !cfg.reachable.contains(p))
        }) {
            continue;
        }
        let Some(end) = next.checked_add(phi_blocks.len() as u32) else {
            return;
        };
        let mut phis = BTreeMap::new();
        for b in phi_blocks {
            phis.insert(b, (next, vec![]));
            next += 1;
        }
        debug_assert_eq!(next, end);
        // Explicit traversal avoids recursion for large generated functions.
        let mut work = vec![(0, None)];
        while let Some((b, mut current)) = work.pop() {
            if let Some((temp, _)) = phis.get(&b) {
                current = Some(Value::Temp(*temp));
            }
            for inst in &mut function.blocks[b].instructions {
                match inst {
                    Instruction::Store { ptr, value, .. } if address(ptr) == Some(id) => {
                        current = Some(value.clone());
                        *inst = Instruction::Comment("promoted local store".into());
                    }
                    Instruction::Load { result, ptr, .. } if address(ptr) == Some(id) => {
                        *inst = Instruction::Binary {
                            result: *result,
                            op: IrBinaryOp::Add,
                            lhs: current
                                .clone()
                                .expect("definite assignment and Phi placement"),
                            rhs: Value::Constant(0),
                            result_type: candidate.ty.clone(),
                        };
                    }
                    Instruction::Alloca { result, .. } if *result == id => {
                        *inst = Instruction::Comment("promoted local allocation".into());
                    }
                    _ => {}
                }
            }
            for &s in &cfg.successors[b] {
                if let Some((_, incoming)) = phis.get_mut(&s) {
                    incoming.push((
                        current.clone().expect("live Phi incoming must be assigned"),
                        function.blocks[b].id,
                    ));
                }
            }
            work.extend(
                children[b]
                    .iter()
                    .rev()
                    .map(|&child| (child, current.clone())),
            );
        }
        for (b, (result, incoming)) in phis {
            function.blocks[b].instructions.insert(
                0,
                Instruction::Phi {
                    result,
                    incoming,
                    result_type: candidate.ty.clone(),
                },
            );
        }
        for block in &mut function.blocks {
            block.instructions.retain(|inst| {
                !matches!(inst, Instruction::Comment(text)
                if text == "promoted local store" || text == "promoted local allocation")
            });
        }
    }
}
