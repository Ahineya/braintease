//! Wasm locals start at zero. Verify SSA uses so malformed IR cannot silently
//! turn a missing/non-dominating definition into a valid zero-valued local.get.
use rcc_ir::ir::{Function, Instruction as I, Value};
use std::collections::BTreeMap;

pub(crate) fn ssa(
    function: &Function,
    edges: &[(usize, usize)],
    indices: &BTreeMap<u32, usize>,
) -> Result<(), String> {
    let count = function.blocks.len();
    let mut successors = vec![Vec::new(); count];
    let mut predecessors = vec![Vec::new(); count];
    for &(from, to) in edges {
        successors[from].push(to);
        predecessors[to].push(from);
    }
    let mut reachable = vec![false; count];
    let mut order = Vec::new();
    let mut pending = vec![(0, false)];
    while let Some((block, expanded)) = pending.pop() {
        if expanded {
            order.push(block);
        } else if !reachable[block] {
            reachable[block] = true;
            pending.push((block, true));
            pending.extend(successors[block].iter().rev().map(|&to| (to, false)));
        }
    }
    order.reverse();
    let mut ranks = vec![0; count];
    for (rank, &block) in order.iter().enumerate() {
        ranks[block] = rank;
    }
    // Immediate dominators in reverse postorder. Avoid a quadratic matrix of
    // dominator sets for large frontend-generated functions.
    let mut idom = vec![None; count];
    idom[0] = Some(0);
    loop {
        let mut changed = false;
        for &block in order.iter().skip(1) {
            let mut known = predecessors[block]
                .iter()
                .copied()
                .filter(|&p| idom[p].is_some());
            let mut parent = known
                .next()
                .ok_or("reachable block has no processed predecessor")?;
            for mut pred in known {
                while parent != pred {
                    while ranks[parent] > ranks[pred] {
                        parent = idom[parent].unwrap();
                    }
                    while ranks[pred] > ranks[parent] {
                        pred = idom[pred].unwrap();
                    }
                }
            }
            if idom[block] != Some(parent) {
                idom[block] = Some(parent);
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    let mut children = vec![Vec::new(); count];
    for &block in order.iter().skip(1) {
        children[idom[block].unwrap()].push(block);
    }
    let mut entry = vec![0; count];
    let mut exit = vec![0; count];
    let mut clock = 0;
    pending.push((0, false));
    while let Some((block, expanded)) = pending.pop() {
        clock += 1;
        if expanded {
            exit[block] = clock;
        } else {
            entry[block] = clock;
            pending.push((block, true));
            pending.extend(children[block].iter().map(|&child| (child, false)));
        }
    }
    // None means a parameter, available throughout the function.
    let mut definitions = BTreeMap::new();
    for (id, _) in &function.parameters {
        definitions.insert(*id, None);
    }
    for (block, b) in function.blocks.iter().enumerate() {
        for (position, inst) in b.instructions.iter().enumerate() {
            if let Some((id, _)) = crate::emit::result_type(inst) {
                definitions.insert(id, Some((block, position)));
            }
        }
    }
    let check = |value: &Value, block: usize, position: usize| -> Result<(), String> {
        let mut ids = Vec::new();
        value_temps(value, &mut ids);
        for id in &ids {
            let definition = definitions
                .get(id)
                .ok_or_else(|| format!("undefined IR temporary %{id}"))?;
            if let Some((origin, at)) = definition {
                if (*origin == block && *at >= position)
                    || (*origin != block
                        && reachable[block]
                        && !(entry[*origin] <= entry[block] && exit[block] <= exit[*origin]))
                {
                    return Err(format!(
                        "IR temporary %{id} does not dominate its use in block {}",
                        function.blocks[block].id
                    ));
                }
            }
        }
        Ok(())
    };
    for (block, b) in function.blocks.iter().enumerate() {
        for (position, inst) in b.instructions.iter().enumerate() {
            let values: Vec<&Value> = match inst {
                I::Binary { lhs, rhs, .. } => vec![lhs, rhs],
                I::Unary { operand, .. } => vec![operand],
                I::Cast { value, .. } => vec![value],
                I::Load { ptr, .. } => vec![ptr],
                I::Store { value, ptr, .. } => vec![value, ptr],
                I::GetElementPtr { ptr, indices, .. } => {
                    std::iter::once(ptr).chain(indices).collect()
                }
                I::Alloca { count, .. } => count.iter().collect(),
                I::Intrinsic { args, .. } => args.iter().collect(),
                I::Select {
                    condition,
                    true_value,
                    false_value,
                    ..
                } => vec![condition, true_value, false_value],
                I::Call { function, args, .. } => std::iter::once(function).chain(args).collect(),
                I::Return(value) => value.iter().collect(),
                I::BranchCond { condition, .. } => vec![condition],
                I::Phi { incoming, .. } => {
                    for (value, from) in incoming {
                        let pred = *indices
                            .get(from)
                            .ok_or_else(|| format!("unknown phi predecessor {from}"))?;
                        check(value, pred, function.blocks[pred].instructions.len())?;
                    }
                    Vec::new()
                }
                _ => Vec::new(), // Other instructions are rejected by instruction selection.
            };
            for value in values {
                check(value, block, position)?;
            }
        }
    }
    Ok(())
}

fn value_temps(value: &Value, ids: &mut Vec<u32>) {
    match value {
        Value::Temp(id) => ids.push(*id),
        Value::FatPtr(p) => value_temps(&p.addr, ids),
        Value::AddressOffset { base, .. } => value_temps(base, ids),
        Value::ConstantAggregate(values) => {
            for value in values {
                value_temps(value, ids);
            }
        }
        _ => {}
    }
}
