//! Lower Phi nodes only after SSA optimizations. Each Phi gets a private typed
//! home, written on its incoming edges and loaded at its block entry. Homes are
//! distinct from the incoming SSA temporaries, so even cyclic parallel copies
//! read the old values before any destination result is redefined.
//!
//! The current backend spills registers at every CFG edge. Using explicit homes
//! preserves its unique-definition and wide-value metadata invariants; assigning
//! a TempId repeatedly in predecessor blocks would violate both invariants.

use super::cfg::{next_temp, Cfg};
use rcc_frontend::ir::{BasicBlock, Function, Instruction, IrType, Value};
use std::collections::{BTreeMap, BTreeSet};

pub fn destroy_phis(function: &mut Function) -> Result<(), String> {
    if !function
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .any(|i| matches!(i, Instruction::Phi { .. }))
    {
        return Ok(());
    }
    let cfg = Cfg::new(function)?;
    let mut next = next_temp(function).ok_or("Phi temporary identifier overflow")?;
    let mut label = function
        .blocks
        .iter()
        .map(|b| b.id)
        .max()
        .unwrap_or(0)
        .checked_add(1)
        .ok_or("Phi edge label overflow")?;
    let mut homes = Vec::new();
    let mut edges = BTreeMap::<(usize, usize), Vec<Instruction>>::new();
    let mut replacements = Vec::new();
    // Validate the entire function before making any changes.
    for (bi, block) in function.blocks.iter().enumerate() {
        let mut body_started = false;
        for (ii, inst) in block.instructions.iter().enumerate() {
            let Instruction::Phi {
                result,
                incoming,
                result_type,
            } = inst
            else {
                body_started |=
                    !matches!(inst, Instruction::Comment(_) | Instruction::DebugLoc { .. });
                continue;
            };
            if body_started {
                return Err(format!(
                    "Phi in block {} follows an ordinary instruction",
                    block.id
                ));
            }
            if bi == 0 || cfg.predecessors[bi].is_empty() {
                return Err(format!("Phi in block {} has no valid entry edge", block.id));
            }
            if !(result_type.is_integer() || result_type.is_pointer() || result_type.is_fp()) {
                return Err(format!("unsupported Phi type {result_type}"));
            }
            let expected: BTreeSet<_> = cfg.predecessors[bi]
                .iter()
                .map(|&p| function.blocks[p].id)
                .collect();
            let actual: BTreeSet<_> = incoming.iter().map(|(_, p)| *p).collect();
            if actual != expected || actual.len() != incoming.len() {
                return Err(format!(
                    "Phi t{result} in block {} must have exactly one value per predecessor",
                    block.id
                ));
            }
            let home = next;
            next = next
                .checked_add(1)
                .ok_or("Phi temporary identifier overflow")?;
            homes.push(Instruction::Alloca {
                result: home,
                alloc_type: result_type.clone(),
                count: None,
                result_type: IrType::FatPtr(Box::new(result_type.clone())),
            });
            replacements.push((
                bi,
                ii,
                Instruction::Load {
                    result: *result,
                    ptr: Value::Temp(home),
                    result_type: result_type.clone(),
                    volatile: false,
                },
            ));
            for &pred in &cfg.predecessors[bi] {
                let value = incoming
                    .iter()
                    .find(|(_, p)| *p == function.blocks[pred].id)
                    .unwrap()
                    .0
                    .clone();
                edges
                    .entry((pred, bi))
                    .or_default()
                    .push(Instruction::Store {
                        value,
                        ptr: Value::Temp(home),
                        value_type: result_type.clone(),
                        volatile: false,
                    });
            }
        }
    }
    let split_count = edges
        .keys()
        .filter(|(pred, _)| cfg.successors[*pred].len() > 1)
        .count();
    label
        .checked_add(u32::try_from(split_count).map_err(|_| "Phi edge label overflow")?)
        .ok_or("Phi edge label overflow")?;
    // Preserve all original fallthroughs before appending edge blocks.
    for i in 0..function.blocks.len() {
        if !function.blocks[i].has_terminator() {
            if let Some(&successor) = cfg.successors[i].first() {
                let target = function.blocks[successor].id;
                function.blocks[i]
                    .instructions
                    .push(Instruction::Branch(target));
            } else {
                function.blocks[i]
                    .instructions
                    .push(Instruction::Return(None));
            }
        }
    }
    for (bi, ii, load) in replacements {
        function.blocks[bi].instructions[ii] = load;
    }
    function.blocks[0].instructions.splice(0..0, homes);
    for ((pred, target), mut stores) in edges {
        let target_label = function.blocks[target].id;
        if cfg.successors[pred].len() == 1 {
            let insertion = function.blocks[pred].instructions.len() - 1;
            function.blocks[pred]
                .instructions
                .splice(insertion..insertion, stores);
        } else {
            match function.blocks[pred].instructions.last_mut().unwrap() {
                Instruction::BranchCond {
                    true_label,
                    false_label,
                    ..
                } => {
                    if *true_label == target_label {
                        *true_label = label;
                    }
                    if *false_label == target_label {
                        *false_label = label;
                    }
                }
                _ => unreachable!("multiple successors require a conditional terminator"),
            }
            stores.push(Instruction::Branch(target_label));
            let mut block = BasicBlock::new(label);
            block.instructions = stores;
            function.blocks.push(block);
            label += 1;
        }
    }
    Cfg::new(function)?.refresh_metadata(function);
    Ok(())
}

#[cfg(test)]
mod tests;
