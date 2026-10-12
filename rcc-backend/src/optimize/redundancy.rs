//! Dominance-based scalar redundancy elimination. Only explicitly nonvolatile
//! global reads that the entire function leaves unchanged may be speculated.
//! Unknown stores/calls and assembly memory clobbers remain hard barriers.
use rcc_common::TempId;
use rcc_frontend::ir::{Function, Instruction as I, IrBinaryOp as B, IrType, Value};
use std::collections::{HashMap, HashSet};

fn global(value: &Value) -> Option<&str> {
    match value {
        Value::Global(name) => Some(name),
        Value::FatPtr(p) if p.bank == rcc_frontend::BankTag::Global => global(&p.addr),
        _ => None,
    }
}

fn copy(result: TempId, value: Value, ty: IrType) -> I {
    I::Binary {
        result,
        op: B::Add,
        lhs: value,
        rhs: Value::Constant(0),
        result_type: ty,
    }
}

/// The inline-assembly operand/clobber contract permits ordinary C memory to
/// remain cached only for understood operand-only instructions without a memory
/// clobber. Unknown assembly remains a barrier even when its author omitted one.
fn preserves_memory(inst: &I) -> bool {
    matches!(inst, I::InlineAsm { assembly, clobbers, .. }
        if clobbers.is_empty() && super::inline::safe_asm(assembly, clobbers))
}

fn hoist_readonly_globals(function: &mut Function) {
    let mut names: HashSet<String> = function
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter_map(|i| match i {
            I::Load {
                ptr,
                result_type,
                volatile: false,
                ..
            } if result_type.is_integer() => global(ptr).map(str::to_owned),
            _ => None,
        })
        .collect();
    let allocas: HashSet<_> = function
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter_map(|i| match i {
            I::Alloca { result, .. } => Some(*result),
            _ => None,
        })
        .collect();
    for inst in function.blocks.iter().flat_map(|b| &b.instructions) {
        match inst {
            I::Store { ptr, .. } => {
                if let Some(name) = global(ptr) {
                    names.remove(name);
                } else {
                    let local = match ptr {
                        Value::Temp(id) => allocas.contains(id),
                        Value::FatPtr(p) if p.bank == rcc_frontend::BankTag::Stack => {
                            matches!(&*p.addr, Value::Temp(id) if allocas.contains(id))
                        }
                        _ => false,
                    };
                    if !local {
                        return;
                    }
                }
            }
            I::Load {
                ptr,
                volatile: true,
                ..
            } => {
                if let Some(name) = global(ptr) {
                    names.remove(name);
                }
            }
            I::Call { .. } | I::Intrinsic { .. } => return,
            I::InlineAsm { .. } if !preserves_memory(inst) => return,
            _ => {}
        }
    }
    let mut loads: Vec<(String, IrType, TempId)> = vec![];
    let mut hoisted = vec![];
    for block in &mut function.blocks {
        let mut code = vec![];
        for inst in std::mem::take(&mut block.instructions) {
            if let I::Load {
                result,
                ref ptr,
                ref result_type,
                volatile: false,
            } = inst
            {
                if let Some(name) = global(ptr).filter(|name| names.contains(*name)) {
                    if let Some((_, _, prior)) = loads
                        .iter()
                        .find(|(n, ty, _)| n == name && ty == result_type)
                    {
                        code.push(copy(result, Value::Temp(*prior), result_type.clone()));
                    } else {
                        loads.push((name.into(), result_type.clone(), result));
                        hoisted.push(inst);
                    }
                    continue;
                }
            }
            code.push(inst);
        }
        block.instructions = code;
    }
    if !hoisted.is_empty() {
        function.blocks[0].instructions.splice(0..0, hoisted);
    }
}

fn expression(inst: &I) -> Option<(String, TempId, IrType)> {
    match inst {
        I::Binary {
            result,
            op,
            lhs,
            rhs,
            result_type,
        } if result_type.is_integer() => {
            // Types and operand identities are part of the key, including the
            // source temporary identity for comparisons of multiword values.
            Some((
                format!("B:{op:?}:{result_type:?}:{lhs:?}:{rhs:?}"),
                *result,
                if super::constants::comparison(*op) {
                    IrType::I16
                } else {
                    result_type.clone()
                },
            ))
        }
        I::Unary {
            result,
            op,
            operand,
            result_type,
        } if result_type.is_integer() => Some((
            format!("U:{op:?}:{result_type:?}:{operand:?}"),
            *result,
            result_type.clone(),
        )),
        _ => None,
    }
}

fn common_expressions(function: &mut Function) {
    let Ok(cfg) = super::cfg::Cfg::new(function) else {
        return;
    };
    let mut order: Vec<_> = cfg.reachable.iter().copied().collect();
    order.sort_by_key(|&i| (cfg.dominators[i].len(), i));
    let mut available: HashMap<String, Vec<(usize, TempId)>> = HashMap::new();
    for bi in order {
        for inst in &mut function.blocks[bi].instructions {
            let Some((key, result, ty)) = expression(inst) else {
                continue;
            };
            let values = available.entry(key).or_default();
            if let Some(&(_, prior)) = values
                .iter()
                .rev()
                .find(|(at, _)| cfg.dominators[bi].contains(at))
            {
                *inst = copy(result, Value::Temp(prior), ty);
            } else {
                values.push((bi, result));
            }
        }
    }
}

/// Normalize complementary comparisons without changing signedness or operand
/// width. `a < b` and `a >= b` share a predicate with opposite polarity.
fn predicate(inst: &I) -> Option<(String, bool)> {
    let I::Binary {
        op,
        lhs,
        rhs,
        result_type,
        ..
    } = inst
    else {
        return None;
    };
    let (family, inverse, swapped) = match op {
        B::Eq => ("eq", false, false),
        B::Ne => ("eq", true, false),
        B::Slt => ("slt", false, false),
        B::Sge => ("slt", true, false),
        B::Sgt => ("slt", false, true),
        B::Sle => ("slt", true, true),
        B::Ult => ("ult", false, false),
        B::Uge => ("ult", true, false),
        B::Ugt => ("ult", false, true),
        B::Ule => ("ult", true, true),
        _ => return None,
    };
    let (a, b) = if swapped { (rhs, lhs) } else { (lhs, rhs) };
    Some((format!("{family}:{result_type:?}:{a:?}:{b:?}"), inverse))
}

fn simplify_control_flow(function: &mut Function) {
    let Ok(cfg) = super::cfg::Cfg::new(function) else {
        return;
    };
    let (constants, _) = super::constants::integer_facts(function);
    let defs: HashMap<_, _> = function
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter_map(|i| super::result(i).map(|id| (id, i.clone())))
        .collect();
    let ids: HashMap<_, _> = function
        .blocks
        .iter()
        .enumerate()
        .map(|(i, b)| (b.id, i))
        .collect();
    let mut facts = vec![HashMap::<String, bool>::new(); function.blocks.len()];
    for (bi, block) in function.blocks.iter().enumerate() {
        let Some(I::BranchCond {
            condition: Value::Temp(id),
            true_label,
            false_label,
        }) = block.instructions.last()
        else {
            continue;
        };
        let Some((key, inverse)) = defs.get(id).and_then(predicate) else {
            continue;
        };
        if true_label == false_label {
            continue;
        }
        for (label, outcome) in [(true_label, true), (false_label, false)] {
            let target = ids[label];
            if target == 0 || cfg.predecessors[target] != [bi] {
                continue;
            }
            for &at in &cfg.reachable {
                if cfg.dominators[at].contains(&target) {
                    facts[at].insert(key.clone(), outcome != inverse);
                }
            }
        }
    }
    for (bi, block) in function.blocks.iter_mut().enumerate() {
        let mut local = constants.clone();
        for inst in &mut block.instructions {
            if let Some((key, inverse)) = predicate(inst) {
                if let Some(value) = facts[bi].get(&key) {
                    let id = super::result(inst).unwrap();
                    let value = i64::from(*value != inverse);
                    *inst = copy(id, Value::Constant(value), IrType::I16);
                    local.insert(id, value);
                }
            }
            if let I::BranchCond {
                condition,
                true_label,
                false_label,
            } = inst
            {
                let value = match condition {
                    Value::Constant(c) => Some(*c),
                    Value::Temp(id) => local.get(id).copied(),
                    _ => None,
                };
                if let Some(value) = value {
                    *inst = I::Branch(if value != 0 {
                        *true_label
                    } else {
                        *false_label
                    });
                }
            }
        }
    }
    let Ok(cfg) = super::cfg::Cfg::new(function) else {
        return;
    };
    let labels: Vec<_> = function.blocks.iter().map(|b| b.id).collect();
    for (bi, block) in function.blocks.iter_mut().enumerate() {
        let predecessors: HashSet<_> = cfg.predecessors[bi]
            .iter()
            .filter(|p| cfg.reachable.contains(p))
            .map(|&p| labels[p])
            .collect();
        for inst in &mut block.instructions {
            if let I::Phi {
                result,
                incoming,
                result_type,
            } = inst
            {
                incoming.retain(|(_, pred)| predecessors.contains(pred));
                if result_type.is_integer()
                    && !incoming.is_empty()
                    && incoming[0].0 != Value::Temp(*result)
                    && incoming.iter().all(|(v, _)| v == &incoming[0].0)
                {
                    *inst = copy(*result, incoming[0].0.clone(), result_type.clone());
                }
            }
        }
    }
    let mut i = 0;
    function.blocks.retain(|_| {
        let keep = cfg.reachable.contains(&i);
        i += 1;
        keep
    });
    super::inline::merge_linear_blocks(function);
    if let Ok(cfg) = super::cfg::Cfg::new(function) {
        cfg.refresh_metadata(function);
    }
}

pub(super) fn eliminate_redundancy(function: &mut Function) {
    if function.is_external || function.blocks.is_empty() || super::cfg::Cfg::new(function).is_err()
    {
        return;
    }
    hoist_readonly_globals(function);
    for _ in 0..6 {
        super::constants::simplify(function);
        common_expressions(function);
        super::constants::simplify(function);
        simplify_control_flow(function);
        super::eliminate_dead_expressions(function);
    }
}

#[cfg(test)]
mod tests;
