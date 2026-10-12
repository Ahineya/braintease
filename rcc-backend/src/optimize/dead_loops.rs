//! Remove empty counter loops only after proving termination at target width.
//! In particular, a backedge alone is never evidence that a loop can disappear.
use super::{cfg::Cfg, result, used_temps};
use rcc_frontend::ir::{Function, Instruction as I, IrBinaryOp as B, IrType, Value};
use std::collections::{HashMap, HashSet};

pub(super) fn eliminate(function: &mut Function) {
    loop {
        let Ok(cfg) = Cfg::new(function) else {
            return;
        };
        let Some((head, members, exit)) = cfg
            .reachable
            .iter()
            .find_map(|&head| candidate(function, &cfg, head))
        else {
            return;
        };
        // Keep the header as the predecessor of the exit, so exit Phis do not
        // need edge rewriting. No value defined in the loop escapes its body.
        let exit_id = function.blocks[exit].id;
        function.blocks[head].instructions = vec![I::Branch(exit_id)];
        let mut index = 0;
        function.blocks.retain(|_| {
            let keep = index == head || !members.contains(&index);
            index += 1;
            keep
        });
        if let Ok(cfg) = Cfg::new(function) {
            cfg.refresh_metadata(function);
        }
    }
}

fn candidate(
    function: &Function,
    cfg: &Cfg,
    head: usize,
) -> Option<(usize, HashSet<usize>, usize)> {
    let block = &function.blocks[head];
    let I::BranchCond {
        condition: Value::Temp(condition),
        true_label,
        false_label,
    } = block.instructions.last()?
    else {
        return None;
    };
    let ids: HashMap<_, _> = function
        .blocks
        .iter()
        .enumerate()
        .map(|(i, b)| (b.id, i))
        .collect();
    let exit = ids[false_label];
    let mut next = ids[true_label];
    if exit == head || next == head || next == exit {
        return None;
    }
    // The body has one unconditional path back to the header. This excludes
    // secondary cycles, alternate latches, early exits and skipped increments.
    let mut members = HashSet::from([head]);
    let mut tail = head;
    while next != head {
        if next == exit || !members.insert(next) || cfg.predecessors[next] != [tail] {
            return None;
        }
        let Some(I::Branch(target)) = function.blocks[next].instructions.last() else {
            return None;
        };
        tail = next;
        next = ids[target];
    }
    if cfg.predecessors[head].len() != 2 || !cfg.predecessors[head].contains(&tail) {
        return None;
    }
    let entry = *cfg.predecessors[head].iter().find(|&&p| p != tail)?;
    if members.contains(&entry) || !cfg.reachable.contains(&entry) {
        return None;
    }
    let mut definitions = HashMap::new();
    for (id, _) in &function.parameters {
        definitions.insert(*id, None);
    }
    for (bi, b) in function.blocks.iter().enumerate() {
        for inst in &b.instructions {
            if let Some(id) = result(inst) {
                if definitions.insert(id, Some((bi, inst))).is_some() {
                    return None;
                }
            }
        }
    }
    let &(compare_block, compare) = definitions.get(condition)?.as_ref()?;
    let I::Binary {
        op: B::Slt | B::Ult,
        lhs: Value::Temp(counter),
        rhs: Value::Constant(bound),
        result_type: IrType::I16 | IrType::I1,
        ..
    } = compare
    else {
        return None;
    };
    if compare_block != head || !(-32768..=65535).contains(bound) {
        return None;
    }
    let &(phi_block, phi) = definitions.get(counter)?.as_ref()?;
    let I::Phi {
        incoming,
        result_type: IrType::I16,
        ..
    } = phi
    else {
        return None;
    };
    if phi_block != head || incoming.len() != 2 {
        return None;
    }
    let initial = incoming
        .iter()
        .find(|(_, label)| *label == function.blocks[entry].id)?;
    let updated = incoming
        .iter()
        .find(|(_, label)| *label == function.blocks[tail].id)?;
    let (Value::Constant(initial), Value::Temp(updated)) = (&initial.0, &updated.0) else {
        return None;
    };
    if !(-32768..=65535).contains(initial) {
        return None;
    }
    let &(update_block, update) = definitions.get(updated)?.as_ref()?;
    let I::Binary {
        op: B::Add,
        lhs: Value::Temp(input),
        rhs: Value::Constant(1),
        result_type: IrType::I16,
        ..
    } = update
    else {
        return None;
    };
    if input != counter || update_block == head || !members.contains(&update_block) {
        return None;
    }
    // A signed or unsigned 16-bit counter incremented by one while strictly
    // below a same-width bound reaches that bound before overflow (or makes
    // zero trips). Unlike <=, this holds even for the greatest value. Constants
    // are interpreted as target bit patterns, including signed negative starts.
    let allowed = HashSet::from([*counter, *condition, *updated]);
    for (bi, b) in function.blocks.iter().enumerate() {
        for inst in &b.instructions {
            if members.contains(&bi) {
                if !matches!(
                    inst,
                    I::Branch(_) | I::BranchCond { .. } | I::Comment(_) | I::DebugLoc { .. }
                ) && result(inst).is_none_or(|id| !allowed.contains(&id))
                {
                    return None;
                }
            } else if !used_temps(inst).is_disjoint(&allowed) {
                return None;
            }
        }
    }
    Some((head, members, exit))
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_frontend::ir::BasicBlock;

    fn counted(initial: i64, bound: i64, signed: bool) -> Function {
        let mut f = Function::new("empty".into(), IrType::I16);
        let blocks = [
            vec![I::Branch(1)],
            vec![
                I::Phi {
                    result: 0,
                    incoming: vec![(Value::Constant(initial), 0), (Value::Temp(2), 3)],
                    result_type: IrType::I16,
                },
                I::Binary {
                    result: 1,
                    op: if signed { B::Slt } else { B::Ult },
                    lhs: Value::Temp(0),
                    rhs: Value::Constant(bound),
                    result_type: IrType::I16,
                },
                I::BranchCond {
                    condition: Value::Temp(1),
                    true_label: 2,
                    false_label: 4,
                },
            ],
            vec![I::Branch(3)],
            vec![
                I::Binary {
                    result: 2,
                    op: B::Add,
                    lhs: Value::Temp(0),
                    rhs: Value::Constant(1),
                    result_type: IrType::I16,
                },
                I::Branch(1),
            ],
            vec![I::Return(Some(Value::Constant(7)))],
        ];
        f.blocks = blocks
            .into_iter()
            .enumerate()
            .map(|(id, instructions)| {
                let mut b = BasicBlock::new(id as u32);
                b.instructions = instructions;
                b
            })
            .collect();
        f
    }

    #[test]
    fn finite_counters_and_zero_trips_preserve_exit_predecessor() {
        for (start, end, signed) in [
            (0, 5000, true),
            (7, 0, true),
            (-32768, 32767, true),
            (65534, 65535, false),
            (0, -1, false),
        ] {
            let mut f = counted(start, end, signed);
            eliminate(&mut f);
            assert_eq!(f.blocks.len(), 3);
            assert_eq!(f.blocks[1].instructions, vec![I::Branch(4)]);
            assert_eq!(f.blocks[2].predecessors, vec![1]);
        }
    }

    #[test]
    fn wrapping_infinite_counters_unknown_bounds_and_skipped_steps_remain() {
        let mut cases = vec![];
        let mut f = counted(0, 65535, false);
        if let I::Binary { op, .. } = &mut f.blocks[1].instructions[1] {
            *op = B::Ule;
        }
        cases.push(f);
        let mut f = counted(0, 5000, true);
        if let I::Binary { rhs, .. } = &mut f.blocks[3].instructions[0] {
            *rhs = Value::Constant(2);
        }
        cases.push(f);
        let mut f = counted(0, 5000, true);
        f.parameters.push((9, IrType::I16));
        if let I::Binary { rhs, .. } = &mut f.blocks[1].instructions[1] {
            *rhs = Value::Temp(9);
        }
        cases.push(f);
        let mut f = counted(0, 5000, true);
        f.blocks[2].instructions = vec![I::BranchCond {
            condition: Value::Constant(1),
            true_label: 1,
            false_label: 3,
        }];
        cases.push(f);
        for mut f in cases {
            let before = f.clone();
            eliminate(&mut f);
            assert_eq!(f, before);
        }
    }

    #[test]
    fn effects_and_counter_liveouts_are_never_removed() {
        let mut cases = vec![];
        for effect in [
            I::Load {
                result: 8,
                ptr: Value::Constant(20),
                result_type: IrType::I16,
                volatile: true,
            },
            I::Store {
                value: Value::Constant(1),
                ptr: Value::Constant(20),
                value_type: IrType::I16,
                volatile: false,
            },
            I::Call {
                result: None,
                function: Value::Function("effect".into()),
                args: vec![],
                result_type: IrType::Void,
                stack_args_from: None,
            },
            I::Binary {
                result: 8,
                op: B::UDiv,
                lhs: Value::Constant(7),
                rhs: Value::Temp(0),
                result_type: IrType::I16,
            },
        ] {
            let mut f = counted(0, 5000, true);
            f.blocks[2].instructions.insert(0, effect);
            cases.push(f);
        }
        let mut f = counted(0, 5000, true);
        f.blocks[4].instructions = vec![I::Return(Some(Value::Temp(0)))];
        cases.push(f);
        for mut f in cases {
            let before = f.clone();
            eliminate(&mut f);
            assert_eq!(f, before);
        }
    }
}
