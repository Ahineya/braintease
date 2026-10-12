//! Promote scalar locals whose address never escapes and whose sole store
//! dominates every read. This exposes ordinary C parameters and invariant local
//! expressions to LICM without speculating or reordering any memory accesses.

use rcc_common::TempId;
use rcc_frontend::{
    ir::{Function, Instruction, IrBinaryOp, IrType, Value},
    BankTag,
};
use std::collections::{HashMap, HashSet};

fn address(value: &Value) -> Option<TempId> {
    match value {
        Value::Temp(t) => Some(*t),
        Value::FatPtr(p) if p.bank == BankTag::Stack => match p.addr.as_ref() {
            Value::Temp(t) => Some(*t),
            _ => None,
        },
        _ => None,
    }
}

struct Candidate {
    ty: IrType,
    allocation: (usize, usize),
    store: Option<(usize, usize, Value)>,
    loads: Vec<(usize, usize)>,
    escaped: bool,
}

pub(super) fn promote_single_store_locals(function: &mut Function) {
    let mut candidates = HashMap::new();
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
                            allocation: (bi, ii),
                            store: None,
                            loads: vec![],
                            escaped: false,
                        },
                    );
                }
            }
        }
    }
    if candidates.is_empty() {
        return;
    }
    for (bi, block) in function.blocks.iter().enumerate() {
        for (ii, inst) in block.instructions.iter().enumerate() {
            let mut uses = super::used_temps(inst);
            match inst {
                Instruction::Load {
                    ptr,
                    result_type,
                    volatile: false,
                    ..
                } => {
                    if let Some(id) = address(ptr) {
                        if let Some(c) = candidates.get_mut(&id) {
                            if *result_type == c.ty {
                                c.loads.push((bi, ii));
                                uses.remove(&id);
                            }
                        }
                    }
                }
                Instruction::Store {
                    ptr,
                    value,
                    value_type,
                    volatile: false,
                } => {
                    if let Some(id) = address(ptr) {
                        if let Some(c) = candidates.get_mut(&id) {
                            if *value_type == c.ty {
                                if c.store.is_some() {
                                    c.escaped = true;
                                }
                                c.store = Some((bi, ii, value.clone()));
                                uses.remove(&id);
                                // Storing an address as the value still escapes
                                // even if this instruction also writes to it.
                                super::value_temps(value, &mut uses);
                            }
                        }
                    }
                }
                _ => {}
            }
            for id in uses {
                if let Some(c) = candidates.get_mut(&id) {
                    c.escaped = true;
                }
            }
        }
    }
    candidates.retain(|_, c| !c.escaped && c.store.is_some());
    if candidates.is_empty() {
        return;
    }
    let n = function.blocks.len();
    let ids: HashMap<_, _> = function
        .blocks
        .iter()
        .enumerate()
        .map(|(i, b)| (b.id, i))
        .collect();
    let mut predecessors = vec![vec![]; n];
    let mut successors = vec![vec![]; n];
    for (i, block) in function.blocks.iter().enumerate() {
        let labels = match block.instructions.last() {
            Some(Instruction::Branch(l)) => vec![*l],
            Some(Instruction::BranchCond {
                true_label,
                false_label,
                ..
            }) => vec![*true_label, *false_label],
            Some(Instruction::Return(_)) => vec![],
            // Respect implicit fallthrough in manually constructed IR too.
            _ if i + 1 < n => vec![function.blocks[i + 1].id],
            _ => vec![],
        };
        for label in labels {
            let Some(&j) = ids.get(&label) else {
                return;
            };
            predecessors[j].push(i);
            successors[i].push(j);
        }
    }
    let mut reachable = HashSet::new();
    let mut work = vec![0];
    while let Some(i) = work.pop() {
        if reachable.insert(i) {
            work.extend(&successors[i]);
        }
    }
    let mut dom = vec![reachable.clone(); n];
    dom[0] = HashSet::from([0]);
    loop {
        let mut changed = false;
        for i in 1..n {
            if !reachable.contains(&i) {
                continue;
            }
            let mut set = reachable.clone();
            for &p in &predecessors[i] {
                if reachable.contains(&p) {
                    set.retain(|j| dom[p].contains(j));
                }
            }
            set.insert(i);
            if set != dom[i] {
                dom[i] = set;
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    let dominates = |a: (usize, usize), b: (usize, usize)| {
        reachable.contains(&a.0)
            && reachable.contains(&b.0)
            && if a.0 == b.0 {
                a.1 < b.1
            } else {
                dom[b.0].contains(&a.0)
            }
    };
    let mut remove = HashSet::new();
    for candidate in candidates.into_values() {
        let (bi, ii, value) = candidate.store.unwrap();
        if !dominates(candidate.allocation, (bi, ii))
            || !candidate.loads.iter().all(|&loc| dominates((bi, ii), loc))
        {
            continue;
        }
        for (lb, li) in candidate.loads {
            let result = super::result(&function.blocks[lb].instructions[li]).unwrap();
            // A typed copy preserves multiword call arguments and conversions.
            function.blocks[lb].instructions[li] = Instruction::Binary {
                result,
                op: IrBinaryOp::Add,
                lhs: value.clone(),
                rhs: Value::Constant(0),
                result_type: candidate.ty.clone(),
            };
        }
        remove.insert(candidate.allocation);
        remove.insert((bi, ii));
    }
    for (bi, block) in function.blocks.iter_mut().enumerate() {
        let mut ii = 0;
        block.instructions.retain(|_| {
            let keep = !remove.contains(&(bi, ii));
            ii += 1;
            keep
        });
    }
}
