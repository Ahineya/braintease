use rcc_ir::ir::{Function, Instruction, IrBinaryOp as B, IrUnaryOp as U};
use std::collections::{HashMap, HashSet};

/// Natural-loop LICM. Only nontrapping, pure computations move; loads, stores,
/// calls, assembly, allocations and divisions remain at their original sites.
/// Existing dedicated preheaders suffice for frontend-generated C loops. Skip
/// irreducible loops and loops requiring edge splitting/phi repair.
pub(super) fn hoist_invariants(function: &mut Function) {
    if super::cfg::Cfg::new(function).is_err() { return; }
    let n = function.blocks.len();
    // Frontend C blocks have explicit terminators. Do not infer a natural
    // loop from incomplete CFG edges in hand-written/fallthrough IR.
    if function.blocks.iter().any(|b| !b.has_terminator()) {
        return;
    }
    // Phi results are ordinary definitions for invariance: loop-carried values
    // are defined inside the loop, so neither they nor their dependents move.
    // Phi instructions themselves are never speculatable.
    if n < 2 {
        return;
    }
    let ids: HashMap<_, _> = function
        .blocks
        .iter()
        .enumerate()
        .map(|(i, b)| (b.id, i))
        .collect();
    let mut successors = vec![Vec::new(); n];
    let mut predecessors = vec![Vec::new(); n];
    for (i, block) in function.blocks.iter().enumerate() {
        let labels = match block.instructions.last() {
            Some(Instruction::Branch(l)) => vec![*l],
            Some(Instruction::BranchCond {
                true_label,
                false_label,
                ..
            }) => vec![*true_label, *false_label],
            _ => vec![],
        };
        for l in labels {
            if let Some(&j) = ids.get(&l) {
                if !successors[i].contains(&j) {
                    successors[i].push(j);
                    predecessors[j].push(i);
                }
            }
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
    let mut loops = HashMap::<usize, HashSet<usize>>::new();
    for tail in 0..n {
        if !reachable.contains(&tail) {
            continue;
        }
        for &head in &successors[tail] {
            if !dom[tail].contains(&head) {
                continue;
            }
            let members = loops.entry(head).or_insert_with(|| HashSet::from([head]));
            let mut work = vec![tail];
            while let Some(i) = work.pop() {
                if members.insert(i) {
                    work.extend(
                        predecessors[i]
                            .iter()
                            .copied()
                            .filter(|p| reachable.contains(p)),
                    );
                }
            }
        }
    }
    let mut loops: Vec<_> = loops.into_iter().collect();
    loops.sort_by_key(|(head, members)| (members.len(), *head));
    let params: HashSet<_> = function.parameters.iter().map(|(id, _)| *id).collect();
    for (head, members) in loops {
        let outside: Vec<_> = predecessors[head]
            .iter()
            .copied()
            .filter(|p| !members.contains(p))
            .collect();
        if outside.len() != 1 {
            continue;
        }
        let preheader = outside[0];
        if successors[preheader] != [head] {
            continue;
        }
        let defs: HashMap<_, _> = function
            .blocks
            .iter()
            .enumerate()
            .flat_map(|(i, b)| {
                b.instructions
                    .iter()
                    .filter_map(move |inst| super::result(inst).map(|t| (t, i)))
            })
            .collect();
        let mut moved = HashSet::new();
        let mut hoisted = Vec::new();
        loop {
            let before = moved.len();
            for i in 0..n {
                if !members.contains(&i) {
                    continue;
                }
                for inst in &function.blocks[i].instructions {
                    let Some(t) = super::result(inst) else {
                        continue;
                    };
                    if moved.contains(&t) || !speculatable(inst) {
                        continue;
                    }
                    let invariant = super::used_temps(inst).iter().all(|v| {
                        moved.contains(v)
                            || params.contains(v)
                            || defs
                                .get(v)
                                .is_some_and(|d| !members.contains(d) && dom[preheader].contains(d))
                    });
                    if invariant {
                        moved.insert(t);
                        hoisted.push(inst.clone());
                    }
                }
            }
            if before == moved.len() {
                break;
            }
        }
        if hoisted.is_empty() {
            continue;
        }
        for &i in &members {
            function.blocks[i]
                .instructions
                .retain(|inst| !super::result(inst).is_some_and(|t| moved.contains(&t)));
        }
        let block = &mut function.blocks[preheader];
        let insertion = block.instructions.len() - 1;
        block.instructions.splice(insertion..insertion, hoisted);
    }
}

fn speculatable(inst: &Instruction) -> bool {
    match inst {
        Instruction::Binary {
            op, result_type, ..
        } => result_type.is_integer() && !matches!(op, B::SDiv | B::UDiv | B::SRem | B::URem),
        Instruction::Unary {
            op, result_type, ..
        } => {
            result_type.is_integer() && matches!(op, U::Not | U::Neg | U::ZExt | U::SExt | U::Trunc)
        }
        Instruction::GetElementPtr { .. } => true,
        _ => false,
    }
}
