//! CFG facts shared by scalar promotion and Phi destruction. Actual terminators
//! are authoritative; stale frontend predecessor/successor caches are ignored.

use rcc_ir::ir::{Function, Instruction};
use std::collections::{BTreeSet, HashMap};

pub(crate) struct Cfg {
    pub predecessors: Vec<Vec<usize>>,
    pub successors: Vec<Vec<usize>>,
    pub reachable: BTreeSet<usize>,
    pub dominators: Vec<BTreeSet<usize>>,
    pub idom: Vec<Option<usize>>,
}

impl Cfg {
    pub fn new(function: &Function) -> Result<Self, String> {
        let n = function.blocks.len();
        let mut ids = HashMap::new();
        for (i, block) in function.blocks.iter().enumerate() {
            if ids.insert(block.id, i).is_some() {
                return Err(format!("duplicate block label {}", block.id));
            }
        }
        let mut successors = vec![vec![]; n];
        let mut predecessors = vec![vec![]; n];
        for (i, block) in function.blocks.iter().enumerate() {
            if block.instructions.iter().rev().skip(1).any(|inst| {
                matches!(
                    inst,
                    Instruction::Branch(_)
                        | Instruction::BranchCond { .. }
                        | Instruction::Return(_)
                )
            }) {
                return Err(format!(
                    "block {} contains an instruction after its terminator",
                    block.id
                ));
            }
            let targets = match block.instructions.last() {
                Some(Instruction::Branch(target)) => vec![*target],
                Some(Instruction::BranchCond {
                    true_label,
                    false_label,
                    ..
                }) => vec![*true_label, *false_label],
                Some(Instruction::Return(_)) => vec![],
                _ if i + 1 < n => vec![function.blocks[i + 1].id],
                _ => vec![],
            };
            for target in targets {
                let j = *ids
                    .get(&target)
                    .ok_or_else(|| format!("branch to missing block {target}"))?;
                if !successors[i].contains(&j) {
                    successors[i].push(j);
                    predecessors[j].push(i);
                }
            }
        }
        let mut reachable = BTreeSet::new();
        let mut work: Vec<_> = (n != 0).then_some(0).into_iter().collect();
        while let Some(i) = work.pop() {
            if reachable.insert(i) {
                work.extend(&successors[i]);
            }
        }
        // Dense block indices let the fixed point intersect machine words,
        // rather than clone and prune thousands of individually allocated tree
        // nodes on every predecessor/iteration. Keep the public sets unchanged.
        let words = n.div_ceil(64);
        let mut all = vec![0u64; words];
        for &i in &reachable { all[i / 64] |= 1 << (i % 64); }
        let mut bits = vec![all.clone(); n];
        if n != 0 {
            bits[0].fill(0);
            bits[0][0] = 1;
        }
        loop {
            let mut changed = false;
            for &i in reachable.iter().filter(|&&i| i != 0) {
                let mut set = all.clone();
                for &p in predecessors[i].iter().filter(|p| reachable.contains(p)) {
                    for (word, predecessor) in set.iter_mut().zip(&bits[p]) {
                        *word &= predecessor;
                    }
                }
                set[i / 64] |= 1 << (i % 64);
                if set != bits[i] {
                    bits[i] = set;
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
        let dominators: Vec<BTreeSet<usize>> = bits.iter().map(|set| {
            set.iter().enumerate().flat_map(|(word, &value)| {
                let mut value = value;
                std::iter::from_fn(move || {
                    if value == 0 { return None; }
                    let bit = value.trailing_zeros() as usize;
                    value &= value - 1;
                    Some(word * 64 + bit)
                })
            }).collect()
        }).collect();
        let mut idom = vec![None; n];
        for &i in reachable.iter().filter(|&&i| i != 0) {
            idom[i] = dominators[i]
                .iter()
                .copied()
                .filter(|&d| d != i)
                .max_by_key(|&d| dominators[d].len());
        }
        Ok(Self {
            predecessors,
            successors,
            reachable,
            dominators,
            idom,
        })
    }

    pub fn refresh_metadata(&self, function: &mut Function) {
        let ids: Vec<_> = function.blocks.iter().map(|b| b.id).collect();
        for (i, block) in function.blocks.iter_mut().enumerate() {
            block.predecessors = self.predecessors[i].iter().map(|&j| ids[j]).collect();
            block.successors = self.successors[i].iter().map(|&j| ids[j]).collect();
        }
    }
}

pub(super) fn next_temp(function: &Function) -> Option<u32> {
    function
        .parameters
        .iter()
        .map(|(id, _)| *id)
        .chain(
            function
                .blocks
                .iter()
                .flat_map(|b| &b.instructions)
                .flat_map(|i| super::used_temps(i).into_iter().chain(super::result(i))),
        )
        .max()
        .map_or(Some(0), |id| id.checked_add(1))
}

/// Emit reachable dominators before their uses, independently of frontend
/// layout. Phi values must already have been lowered to private stack homes.
pub(crate) fn order_for_lowering(function: &mut Function) {
    if function.blocks.is_empty() || function.blocks.iter().any(|b| !b.has_terminator()) {
        return; // Keep legacy implicit fallthrough order intact.
    }
    let Ok(cfg) = Cfg::new(function) else { return; };
    let mut seen = vec![false; function.blocks.len()];
    let mut postorder = Vec::new();
    let mut work = vec![(0, false)];
    while let Some((index, finished)) = work.pop() {
        if finished {
            postorder.push(index);
        } else if !seen[index] {
            seen[index] = true;
            work.push((index, true));
            for &successor in cfg.successors[index].iter().rev() {
                if !seen[successor] { work.push((successor, false)); }
            }
        }
    }
    postorder.reverse();
    postorder.extend((0..seen.len()).filter(|&i| !seen[i]));
    let mut blocks: Vec<_> = std::mem::take(&mut function.blocks).into_iter().map(Some).collect();
    function.blocks = postorder.into_iter().map(|i| blocks[i].take().unwrap()).collect();
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_ir::ir::{BasicBlock, IrType, Value};

    #[test]
    fn packed_dominators_match_set_solver_on_cycles_and_unreachable_blocks() {
        let mut seed = 123456789u64;
        for n in [0, 1, 2, 7, 63, 64, 65, 129] {
            for _ in 0..12 {
                let mut f = Function::new("cfg".into(), IrType::Void);
                for i in 0..n {
                    seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
                    let mut block = BasicBlock::new((i * 3 + 17) as u32);
                    block.instructions.push(if seed % 4 == 0 {
                        Instruction::Return(None)
                    } else {
                        Instruction::BranchCond {
                            condition: Value::Temp(0),
                            true_label: ((seed as usize % n) * 3 + 17) as u32,
                            false_label: (((seed >> 32) as usize % n) * 3 + 17) as u32,
                        }
                    });
                    f.blocks.push(block);
                }
                let cfg = Cfg::new(&f).unwrap();
                let mut expected = vec![cfg.reachable.clone(); n];
                if n != 0 { expected[0] = BTreeSet::from([0]); }
                loop {
                    let mut changed = false;
                    for &i in cfg.reachable.iter().filter(|&&i| i != 0) {
                        let mut set = cfg.reachable.clone();
                        for &p in &cfg.predecessors[i] {
                            if cfg.reachable.contains(&p) {
                                set.retain(|d| expected[p].contains(d));
                            }
                        }
                        set.insert(i);
                        if expected[i] != set { expected[i] = set; changed = true; }
                    }
                    if !changed { break; }
                }
                assert_eq!(cfg.dominators, expected);
                for &i in cfg.reachable.iter().filter(|&&i| i != 0) {
                    assert_eq!(cfg.idom[i], expected[i].iter().copied()
                        .filter(|&d| d != i).max_by_key(|&d| expected[d].len()));
                }
            }
        }
    }
}
