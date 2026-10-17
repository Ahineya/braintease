//! Dominance-based scalar redundancy elimination. Only explicitly nonvolatile
//! global reads that the entire function leaves unchanged may be speculated.
//! Unknown stores/calls and assembly memory clobbers remain hard barriers.
use rcc_common::TempId;
use rcc_ir::ir::{Function, Instruction as I, IrBinaryOp as B, IrType, Value};
use std::collections::{BTreeMap, HashMap, HashSet};

fn global(value: &Value) -> Option<&str> {
    match value {
        Value::Global(name) => Some(name),
        Value::FatPtr(p) if p.bank == rcc_ir::BankTag::Global => global(&p.addr),
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
                        Value::FatPtr(p) if p.bank == rcc_ir::BankTag::Stack => {
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

/// Give repeated one-word arithmetic a real common definition before a
/// short-circuit diamond. This is speculation only of total register arithmetic:
/// loads, calls, divisions, shifts and pointer computations never move here.
/// Ordinary CSE and branch facts can then use the existing dominance rules.
fn hoist_common_arithmetic(function: &mut Function) {
    if super::function_has_opaque_asm(function) {
        return;
    }
    let Ok(cfg) = super::cfg::Cfg::new(function) else {
        return;
    };
    let parameters: HashSet<_> = function.parameters.iter().map(|(id, _)| *id).collect();
    if parameters.len() != function.parameters.len() {
        return;
    }
    let mut definitions = HashMap::new();
    let mut groups = BTreeMap::<String, Vec<(usize, usize, TempId)>>::new();
    for (bi, block) in function.blocks.iter().enumerate() {
        for (ii, inst) in block.instructions.iter().enumerate() {
            if let Some(id) = super::result(inst) {
                if parameters.contains(&id) || definitions.insert(id, (bi, ii)).is_some() {
                    return;
                }
            }
            if cfg.reachable.contains(&bi)
                && matches!(
                    inst,
                    I::Binary {
                        op: B::Add | B::Sub | B::And | B::Or | B::Xor,
                        result_type: IrType::I1 | IrType::I8 | IrType::I16,
                        ..
                    }
                )
            {
                let (key, id, _) = expression(inst).unwrap();
                groups.entry(key).or_default().push((bi, ii, id));
            }
        }
    }
    let insertion = |block: usize| {
        let code = &function.blocks[block].instructions;
        code.len()
            - usize::from(code.last().is_some_and(|inst| {
                matches!(inst, I::Branch(_) | I::BranchCond { .. } | I::Return(_))
            }))
    };
    let mut moved = HashSet::new();
    let mut hoisted = BTreeMap::<usize, Vec<I>>::new();
    for occurrences in groups.values().filter(|values| values.len() > 1) {
        let (first, index, id) = occurrences[0];
        let mut common = cfg.dominators[first].clone();
        for &(block, _, _) in &occurrences[1..] {
            common.retain(|d| cfg.dominators[block].contains(d));
        }
        let Some(target) = common
            .into_iter()
            .max_by_key(|&at| cfg.dominators[at].len())
        else {
            continue;
        };
        // An existing occurrence at the common dominator already serves CSE.
        if occurrences.iter().any(|&(block, _, _)| block == target) {
            continue;
        }
        let inst = &function.blocks[first].instructions[index];
        let I::Binary { lhs, rhs, .. } = inst else {
            unreachable!();
        };
        let available = [lhs, rhs].iter().all(|value| match value {
            Value::Constant(_) => true,
            Value::Temp(id) => {
                parameters.contains(id)
                    || definitions.get(id).is_some_and(|&(block, index)| {
                        cfg.dominators[target].contains(&block)
                            && (block != target || index < insertion(target))
                    })
            }
            _ => false,
        });
        if available {
            moved.insert(id);
            hoisted.entry(target).or_default().push(inst.clone());
        }
    }
    if moved.is_empty() {
        return;
    }
    for block in &mut function.blocks {
        block
            .instructions
            .retain(|inst| !super::result(inst).is_some_and(|id| moved.contains(&id)));
    }
    for (target, instructions) in hoisted {
        let code = &mut function.blocks[target].instructions;
        let insertion = code.len()
            - usize::from(code.last().is_some_and(|inst| {
                matches!(inst, I::Branch(_) | I::BranchCond { .. } | I::Return(_))
            }));
        code.splice(insertion..insertion, instructions);
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

struct PhiContext {
    ids: HashMap<u32, usize>,
    locations: HashMap<TempId, usize>,
    parameters: HashSet<TempId>,
    predicates: HashMap<String, TempId>,
    valid_ssa: bool,
}

impl PhiContext {
    fn new(function: &Function, defs: &HashMap<TempId, I>) -> Self {
        let ids = function
            .blocks
            .iter()
            .enumerate()
            .map(|(i, b)| (b.id, i))
            .collect();
        let parameters: HashSet<_> = function.parameters.iter().map(|(id, _)| *id).collect();
        let mut valid_ssa = parameters.len() == function.parameters.len();
        let mut locations = HashMap::new();
        for (bi, bb) in function.blocks.iter().enumerate() {
            for inst in &bb.instructions {
                if let Some(id) = super::result(inst) {
                    valid_ssa &= !parameters.contains(&id) && locations.insert(id, bi).is_none();
                }
            }
        }
        let predicates = defs
            .iter()
            .filter_map(|(id, inst)| predicate(inst).map(|(key, _)| (key, *id)))
            .collect();
        Self {
            ids,
            locations,
            parameters,
            predicates,
            valid_ssa,
        }
    }
}

/// A short-circuit join carries either its RHS comparison or the boolean that
/// skipped that comparison. Only the opposite outcome proves which edge ran.
/// Require every predicate operand to be available before the join, rather
/// than treating a value produced only on the selected edge as dominating it.
fn short_circuit_facts(
    condition: TempId,
    outcome: bool,
    block: usize,
    context: &PhiContext,
    cfg: &super::cfg::Cfg,
    defs: &HashMap<TempId, I>,
    constants: &HashMap<TempId, i64>,
    facts: &[HashMap<String, bool>],
) -> Option<HashMap<String, bool>> {
    let I::Phi {
        incoming,
        result_type,
        ..
    } = defs.get(&condition)?
    else {
        return None;
    };
    if !context.valid_ssa || !matches!(result_type, IrType::I1 | IrType::I16) || incoming.len() != 2
    {
        return None;
    }
    let PhiContext {
        ids,
        locations,
        parameters,
        ..
    } = context;
    if locations.get(&condition) != Some(&block) {
        return None;
    }
    let predecessors: HashSet<_> = incoming
        .iter()
        .filter_map(|(_, label)| ids.get(label).copied())
        .collect();
    if predecessors.len() != 2
        || predecessors != cfg.predecessors[block].iter().copied().collect()
        || predecessors
            .iter()
            .any(|p| !cfg.reachable.contains(p) || cfg.dominators[*p].contains(&block))
    {
        return None;
    }
    let constant = |value: &Value| match value {
        Value::Constant(c) => Some(*c),
        Value::Temp(id) => constants.get(id).copied(),
        _ => None,
    };
    let mut selected = None;
    let mut skipped = false;
    for (value, label) in incoming {
        match constant(value) {
            Some(c @ (0 | 1)) if (c != 0) != outcome => skipped = true,
            Some(_) => return None,
            None => {
                let Value::Temp(id) = value else {
                    return None;
                };
                if selected.replace((*id, ids[label])).is_some() {
                    return None;
                }
            }
        }
    }
    if !skipped {
        return None;
    }
    let (selected, predecessor) = selected?;
    if locations.get(&selected) != Some(&predecessor) {
        return None;
    }
    let available = |inst: &I| {
        let I::Binary {
            lhs,
            rhs,
            result_type,
            ..
        } = inst
        else {
            return false;
        };
        result_type.is_integer()
            && [lhs, rhs].iter().all(|value| match value {
                Value::Constant(_) => true,
                Value::Temp(id) => {
                    parameters.contains(id)
                        || locations
                            .get(id)
                            .is_some_and(|&at| at != block && cfg.dominators[block].contains(&at))
                }
                _ => false,
            })
    };
    let comparison = defs.get(&selected)?;
    if !available(comparison) {
        return None;
    }
    let (key, inverse) = predicate(comparison)?;
    let mut result: HashMap<_, _> = facts[predecessor]
        .iter()
        .filter(|(key, _)| {
            context
                .predicates
                .get(*key)
                .is_some_and(|id| available(&defs[id]))
        })
        .map(|(key, value)| (key.clone(), *value))
        .collect();
    let value = outcome != inverse;
    if result.get(&key).is_some_and(|prior| *prior != value) {
        return None;
    }
    result.insert(key, value);
    Some(result)
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
    let phi_context = PhiContext::new(function, &defs);
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
    // Direct branch facts above establish the guard on each RHS predecessor.
    // A bounded fixed point also covers nested &&/|| joins. Every addition is
    // independently proven; the bound can only leave an optimization undone.
    for _ in 0..4 {
        let mut changed = false;
        for (bi, block) in function.blocks.iter().enumerate() {
            let Some(I::BranchCond {
                condition: Value::Temp(id),
                true_label,
                false_label,
            }) = block.instructions.last()
            else {
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
                let Some(implied) = short_circuit_facts(
                    *id,
                    outcome,
                    bi,
                    &phi_context,
                    &cfg,
                    &defs,
                    &constants,
                    &facts,
                ) else {
                    continue;
                };
                for &at in &cfg.reachable {
                    if cfg.dominators[at].contains(&target) {
                        for (key, value) in &implied {
                            changed |= facts[at].insert(key.clone(), *value) != Some(*value);
                        }
                    }
                }
            }
        }
        if !changed {
            break;
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
                    Value::Temp(id) => local.get(id).copied().or_else(|| {
                        // CSE may make this branch use the original predicate
                        // directly, with no comparison left in this block.
                        let (key, inverse) = defs.get(id).and_then(predicate)?;
                        facts[bi]
                            .get(&key)
                            .map(|value| i64::from(*value != inverse))
                    }),
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
        let mut folded_phis = Vec::new();
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
                    folded_phis.push(copy(*result, incoming[0].0.clone(), result_type.clone()));
                    *inst = I::Comment("folded Phi".into());
                }
            }
        }
        // Phi definitions are simultaneous at entry. Their typed replacement
        // copies are ordinary instructions and must follow all remaining Phis.
        if !folded_phis.is_empty() {
            block.instructions.retain(|inst| !matches!(inst, I::Comment(text) if text == "folded Phi"));
            let insertion = block.instructions.iter().take_while(|inst|
                matches!(inst, I::Phi { .. } | I::Comment(_) | I::DebugLoc { .. })).count();
            block.instructions.splice(insertion..insertion, folded_phis);
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
        hoist_common_arithmetic(function);
        common_expressions(function);
        super::constants::simplify(function);
        simplify_control_flow(function);
        super::eliminate_dead_expressions(function);
    }
}

#[cfg(test)]
mod tests;
