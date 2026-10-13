//! Rotate simple counted loops without changing their zero-trip behavior.
//!
//! Once a native-word counter starts strictly below an invariant bound, adding
//! one exactly once per body reaches that bound before signed/unsigned wrap.
//! The latch can therefore test equality instead of repeating the ordering
//! comparison. This pass runs after SSA optimizations and lowers only the
//! counter Phi to the same private-home contract used by Phi destruction.

use super::{
    cfg::{next_temp, Cfg},
    result, used_temps,
};
use rcc_frontend::ir::{BasicBlock, Function, Instruction as I, IrBinaryOp as B, IrType, Value};
use std::collections::{HashMap, HashSet};

struct Loop {
    head: usize,
    pre: usize,
    tail: usize,
    exit: usize,
    body: u32,
    counter: u32,
    condition: u32,
    updated: u32,
    initial: Value,
    bound: Value,
    comparison: B,
}

pub(crate) fn rotate(function: &mut Function) {
    if function.is_external || super::function_has_opaque_asm(function) {
        return;
    }
    loop {
        let Ok(cfg) = Cfg::new(function) else {
            return;
        };
        if function.blocks.iter().any(|b| !b.has_terminator()) {
            return;
        }
        let Some(found) = cfg
            .reachable
            .iter()
            .find_map(|&head| candidate(function, &cfg, head))
        else {
            return;
        };
        let Some(home) = next_temp(function) else {
            return;
        };
        let Some(latch_condition) = home.checked_add(1) else {
            return;
        };
        let Some(entry_label) = function
            .blocks
            .iter()
            .map(|b| b.id)
            .max()
            .and_then(|x| x.checked_add(1))
        else {
            return;
        };
        let Some(exit_label) = entry_label.checked_add(1) else {
            return;
        };
        let head_label = function.blocks[found.head].id;
        let old_exit = function.blocks[found.exit].id;

        // The entry guard retains the original signedness and original initial
        // value. A start equal to or above the bound still makes zero trips.
        let pre = &mut function.blocks[found.pre].instructions;
        pre.pop();
        pre.extend([
            I::Binary {
                result: found.condition,
                op: found.comparison,
                lhs: found.initial.clone(),
                rhs: found.bound.clone(),
                result_type: IrType::I16,
            },
            I::BranchCond {
                condition: Value::Temp(found.condition),
                true_label: entry_label,
                false_label: old_exit,
            },
        ]);
        function.blocks[found.head].instructions = vec![
            I::Load {
                result: found.counter,
                ptr: Value::Temp(home),
                result_type: IrType::I16,
                volatile: false,
            },
            I::Branch(found.body),
        ];
        let tail = &mut function.blocks[found.tail].instructions;
        tail.pop();
        tail.extend([
            I::Store {
                value: Value::Temp(found.updated),
                ptr: Value::Temp(home),
                value_type: IrType::I16,
                volatile: false,
            },
            I::Binary {
                result: latch_condition,
                op: B::Eq,
                lhs: Value::Temp(found.updated),
                rhs: found.bound,
                result_type: IrType::I16,
            },
            I::BranchCond {
                condition: Value::Temp(latch_condition),
                true_label: exit_label,
                false_label: head_label,
            },
        ]);
        function.blocks[0].instructions.insert(
            0,
            I::Alloca {
                result: home,
                alloc_type: IrType::I16,
                count: None,
                result_type: IrType::FatPtr(Box::new(IrType::I16)),
            },
        );
        // Dedicated entry/exit edges let machine register promotion preserve
        // its existing contract. The zero-trip guard bypasses both blocks.
        let mut entry = BasicBlock::new(entry_label);
        entry.instructions = vec![
            I::Store {
                value: found.initial,
                ptr: Value::Temp(home),
                value_type: IrType::I16,
                volatile: false,
            },
            I::Branch(head_label),
        ];
        let mut exit = BasicBlock::new(exit_label);
        exit.instructions = vec![I::Branch(old_exit)];
        let mut blocks = Vec::with_capacity(function.blocks.len() + 2);
        for (index, block) in std::mem::take(&mut function.blocks).into_iter().enumerate() {
            if index == found.head {
                blocks.push(entry.clone());
            }
            if index == found.exit {
                blocks.push(exit.clone());
            }
            blocks.push(block);
        }
        function.blocks = blocks;
        if let Ok(cfg) = Cfg::new(function) {
            cfg.refresh_metadata(function);
        }
    }
}

fn candidate(function: &Function, cfg: &Cfg, head: usize) -> Option<Loop> {
    // The implicit function-entry edge cannot initialize a counter home.
    if head == 0 {
        return None;
    }
    let meaningful: Vec<_> = function.blocks[head]
        .instructions
        .iter()
        .filter(|i| !matches!(i, I::Comment(_) | I::DebugLoc { .. }))
        .collect();
    let [I::Phi {
        result: counter,
        incoming,
        result_type: IrType::I16,
    }, I::Binary {
        result: condition,
        op: comparison @ (B::Slt | B::Ult),
        lhs: Value::Temp(input),
        rhs: bound,
        result_type: IrType::I16 | IrType::I1,
    }, I::BranchCond {
        condition: Value::Temp(test),
        true_label,
        false_label,
    }] = meaningful.as_slice()
    else {
        return None;
    };
    if counter != input || condition != test || incoming.len() != 2 {
        return None;
    }
    let ids: HashMap<_, _> = function
        .blocks
        .iter()
        .enumerate()
        .map(|(i, b)| (b.id, i))
        .collect();
    let exit = ids[false_label];
    let mut next = ids[true_label];
    if exit == head
        || next == head
        || exit == next
        || cfg.predecessors[exit] != [head]
        || function.blocks[exit]
            .instructions
            .iter()
            .any(|i| matches!(i, I::Phi { .. }))
    {
        return None;
    }
    let mut members = HashSet::from([head]);
    let mut tail = head;
    // A single unconditional body chain excludes alternate latches, internal
    // cycles, early exits and conditional/skipped counter increments.
    while next != head {
        // Calls can prevent the backend from caching the private counter
        // home. Operand-only asm already passed function_has_opaque_asm's
        // whitelist and retains its memory effects in the loop body.
        if next == exit
            || !members.insert(next)
            || cfg.predecessors[next] != [tail]
            || function.blocks[next]
                .instructions
                .iter()
                .any(|i| matches!(i, I::Phi { .. } | I::Call { .. } | I::Intrinsic { .. }))
        {
            return None;
        }
        let I::Branch(target) = function.blocks[next].instructions.last()? else {
            return None;
        };
        tail = next;
        next = ids[target];
    }
    if cfg.predecessors[head].len() != 2 || !cfg.predecessors[head].contains(&tail) {
        return None;
    }
    let pre = *cfg.predecessors[head].iter().find(|&&p| p != tail)?;
    if members.contains(&pre) || !cfg.reachable.contains(&pre) || cfg.successors[pre] != [head] {
        return None;
    }
    let initial = incoming
        .iter()
        .find(|(_, p)| *p == function.blocks[pre].id)?
        .0
        .clone();
    let Value::Temp(updated) = incoming
        .iter()
        .find(|(_, p)| *p == function.blocks[tail].id)?
        .0
    else {
        return None;
    };
    let mut definitions = HashMap::new();
    for &(id, _) in &function.parameters {
        if definitions.insert(id, None).is_some() {
            return None;
        }
    }
    for (bi, block) in function.blocks.iter().enumerate() {
        for (ii, inst) in block.instructions.iter().enumerate() {
            let mut outputs = result(inst).into_iter().collect::<Vec<_>>();
            if let I::InlineAsm {
                outputs: asm_outputs,
                ..
            } = inst
            {
                outputs.extend(asm_outputs.iter().filter_map(|o| match o.value {
                    Value::Temp(t) => Some(t),
                    _ => None,
                }));
            }
            for id in outputs {
                if definitions.insert(id, Some((bi, ii))).is_some() {
                    return None;
                }
            }
        }
    }
    let &(update_block, update_index) = definitions.get(&updated)?.as_ref()?;
    if update_block == head || !members.contains(&update_block) {
        return None;
    }
    if !matches!(&function.blocks[update_block].instructions[update_index], I::Binary {
        op: B::Add, lhs: Value::Temp(source), rhs: Value::Constant(1), result_type: IrType::I16, ..
    } if source == counter)
    {
        return None;
    }
    let (_, types) = super::constants::integer_facts(function);
    let invariant_word = |value: &Value| match value {
        Value::Constant(c) => (-32768..=65535).contains(c),
        Value::Temp(id) => {
            matches!(types.get(id), Some(IrType::I1 | IrType::I8 | IrType::I16))
                && definitions.get(id).is_some_and(|definition| {
                    definition.is_none_or(|(bi, _)| {
                        !members.contains(&bi) && cfg.dominators[pre].contains(&bi)
                    })
                })
        }
        _ => false,
    };
    if !invariant_word(&initial) || !invariant_word(bound) {
        return None;
    }
    let locals = HashSet::from([*counter, *condition, updated]);
    for (bi, block) in function.blocks.iter().enumerate() {
        for inst in &block.instructions {
            let uses = used_temps(inst);
            if !members.contains(&bi) && !uses.is_disjoint(&locals) {
                return None;
            }
            if uses.contains(condition) && !(bi == head && matches!(inst, I::BranchCond { .. })) {
                return None;
            }
        }
    }
    Some(Loop {
        head,
        pre,
        tail,
        exit,
        body: *true_label,
        counter: *counter,
        condition: *condition,
        updated,
        initial,
        bound: (*bound).clone(),
        comparison: *comparison,
    })
}

#[cfg(test)]
mod tests;
