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
        let mut dominators = vec![reachable.clone(); n];
        if n != 0 {
            dominators[0] = BTreeSet::from([0]);
        }
        loop {
            let mut changed = false;
            for &i in reachable.iter().filter(|&&i| i != 0) {
                let mut set = reachable.clone();
                for &p in predecessors[i].iter().filter(|p| reachable.contains(p)) {
                    set.retain(|j| dominators[p].contains(j));
                }
                set.insert(i);
                if set != dominators[i] {
                    dominators[i] = set;
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
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
