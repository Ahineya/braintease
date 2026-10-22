//! DFS back edges form a feedback set: every directed cycle crosses one.
//! Block-vector order alone also marks acyclic joins/inlined returns as loops.
use rcc_ir::ir::{Function, Instruction as I};
use std::collections::{BTreeMap, BTreeSet};

pub(super) fn checkpoint_edges(function: &Function) -> BTreeSet<(u32, u32)> {
    let successors: BTreeMap<_, _> = function
        .blocks
        .iter()
        .map(|b| {
            let targets = match b.instructions.last() {
                Some(I::Branch(to)) => vec![*to],
                Some(I::BranchCond {
                    true_label,
                    false_label,
                    ..
                }) => vec![*true_label, *false_label],
                _ => vec![],
            };
            (b.id, targets)
        })
        .collect();
    let mut colors = BTreeMap::new();
    let mut feedback = BTreeSet::new();
    for block in &function.blocks {
        if colors.contains_key(&block.id) {
            continue;
        }
        colors.insert(block.id, 1);
        let mut stack = vec![(block.id, 0)];
        while let Some((id, next)) = stack.last_mut() {
            let Some(target) = successors.get(id).and_then(|s| s.get(*next)).copied() else {
                colors.insert(*id, 2);
                stack.pop();
                continue;
            };
            *next += 1;
            match colors.get(&target) {
                Some(1) => {
                    feedback.insert((*id, target));
                }
                Some(_) => {}
                None => {
                    colors.insert(target, 1);
                    stack.push((target, 0));
                }
            }
        }
    }
    feedback
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_ir::ir::{BasicBlock, IrType, Value};

    #[test]
    fn removing_checkpoint_edges_breaks_every_small_cfg_cycle() {
        // Independent topological-sort oracle over every three-node CFG with
        // at most two outgoing edges per node, including irreducible loops.
        for mask in 0..512u32 {
            let targets: Vec<Vec<u32>> = (0..3)
                .map(|from| {
                    (0..3)
                        .filter(|to| mask & (1 << (from * 3 + to)) != 0)
                        .collect()
                })
                .collect();
            if targets.iter().any(|row| row.len() > 2) {
                continue;
            }
            let mut f = Function::new("graph".into(), IrType::I16);
            for (id, row) in targets.iter().enumerate() {
                let mut b = BasicBlock::new(id as u32);
                b.instructions.push(match row.as_slice() {
                    [] => I::Return(None),
                    [to] => I::Branch(*to),
                    [yes, no] => I::BranchCond {
                        condition: Value::Constant(1),
                        true_label: *yes,
                        false_label: *no,
                    },
                    _ => unreachable!(),
                });
                f.blocks.push(b);
            }
            let feedback = checkpoint_edges(&f);
            let mut degrees = [0; 3];
            for (from, row) in targets.iter().enumerate() {
                for to in row {
                    if !feedback.contains(&(from as u32, *to)) {
                        degrees[*to as usize] += 1;
                    }
                }
            }
            let mut done = BTreeSet::new();
            loop {
                let Some(from) = (0..3).find(|i| degrees[*i] == 0 && !done.contains(i)) else {
                    break;
                };
                done.insert(from);
                for to in &targets[from] {
                    if !feedback.contains(&(from as u32, *to)) {
                        degrees[*to as usize] -= 1;
                    }
                }
            }
            assert_eq!(done.len(), 3, "uncheckpointed cycle in graph {mask:09b}");
        }
    }

    #[test]
    fn backward_acyclic_joins_need_no_checkpoint_but_irreducible_cycles_do() {
        let mut f = Function::new("cfg".into(), IrType::I16);
        for (id, instruction) in [
            (
                0,
                I::BranchCond {
                    condition: Value::Constant(1),
                    true_label: 2,
                    false_label: 1,
                },
            ),
            (1, I::Return(None)),
            (2, I::Branch(1)),
        ] {
            let mut b = BasicBlock::new(id);
            b.instructions.push(instruction);
            f.blocks.push(b);
        }
        assert!(checkpoint_edges(&f).is_empty());
        f.blocks[1].instructions = vec![I::Branch(2)];
        assert_eq!(checkpoint_edges(&f), BTreeSet::from([(1, 2)]));
        // Include unreachable/self loops too, and avoid recursive analysis on
        // very deep straight-line CFGs produced by large generated programs.
        for id in 3..10003 {
            let mut b = BasicBlock::new(id);
            b.instructions.push(I::Branch(id + 1));
            f.blocks.push(b);
        }
        let mut last = BasicBlock::new(10003);
        last.instructions.push(I::Branch(10003));
        f.blocks.push(last);
        assert_eq!(
            checkpoint_edges(&f),
            BTreeSet::from([(1, 2), (10003, 10003)])
        );
    }
}
