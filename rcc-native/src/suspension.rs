//! Conservative direct-call islands inside resumable programs. A bounded,
//! acyclic call/CFG tree cannot reach a suspension or require a loop checkpoint.
//! Public function tokens still name their resumable entry points.
use rcc_ir::ir::{Function, Instruction as I, IrType, Module, Value};
use std::collections::{BTreeMap, BTreeSet};

const MAX_COST: usize = 256;

/// Bound work along an executable path, including complete direct callees.
/// Mutually exclusive branches do not all execute during one invocation.
fn bounded_cost(function: &Function, callees: &BTreeMap<String, usize>) -> Option<usize> {
    if function.blocks.is_empty() || function.blocks.len() > MAX_COST {
        return None;
    }
    let mut blocks = BTreeMap::new();
    for block in &function.blocks {
        let successors = match block.instructions.last()? {
            I::Branch(to) => vec![*to],
            I::BranchCond {
                true_label,
                false_label,
                ..
            } => vec![*true_label, *false_label],
            I::Return(_) => vec![],
            _ => return None,
        };
        let mut cost = 0usize;
        for instruction in &block.instructions {
            cost = cost.checked_add(1)?;
            match instruction {
                I::Call {
                    function: Value::Global(name) | Value::Function(name),
                    ..
                } => cost = cost.checked_add(*callees.get(name)?)?,
                I::Call { .. } | I::InlineAsm { .. } => return None,
                _ => {}
            }
            if cost > MAX_COST {
                return None;
            }
        }
        blocks.insert(block.id, (cost, successors));
    }
    fn visit(
        id: u32,
        blocks: &BTreeMap<u32, (usize, Vec<u32>)>,
        active: &mut BTreeSet<u32>,
        done: &mut BTreeMap<u32, usize>,
    ) -> Option<usize> {
        if let Some(cost) = done.get(&id) {
            return Some(*cost);
        }
        if !active.insert(id) {
            return None;
        }
        let (own, successors) = blocks.get(&id)?;
        let mut tail = 0;
        for successor in successors {
            tail = tail.max(visit(*successor, blocks, active, done)?);
        }
        let cost = own.checked_add(tail)?;
        if cost > MAX_COST {
            return None;
        }
        active.remove(&id);
        done.insert(id, cost);
        Some(cost)
    }
    let mut done = BTreeMap::new();
    let mut maximum = 0;
    // Reject cycles/unknown targets even in unreachable source fragments.
    for block in &function.blocks {
        maximum = maximum.max(visit(block.id, &blocks, &mut BTreeSet::new(), &mut done)?);
    }
    Some(maximum)
}

pub(super) fn direct_functions(module: &Module) -> BTreeSet<String> {
    let mut costs = BTreeMap::<String, usize>::new();
    loop {
        let old = costs.len();
        for function in &module.functions {
            // Pointer returns can carry aggregate results in released logical
            // stack storage. Keep their existing return/checkpoint protocol.
            if function.is_external
                || costs.contains_key(&function.name)
                || matches!(
                    function.return_type,
                    IrType::FatPtr(_) | IrType::Struct { .. } | IrType::Array { .. }
                )
            {
                continue;
            }
            if let Some(cost) = bounded_cost(function, &costs) {
                costs.insert(function.name.clone(), cost);
            }
        }
        if costs.len() == old {
            return costs.into_keys().collect();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_ir::ir::BasicBlock;
    fn function(name: &str, instructions: Vec<I>) -> Function {
        let mut f = Function::new(name.into(), IrType::I16);
        let mut block = BasicBlock::new(0);
        block.instructions = instructions;
        f.blocks.push(block);
        f
    }
    fn call(name: &str) -> I {
        I::Call {
            result: None,
            function: Value::Function(name.into()),
            args: vec![],
            result_type: IrType::Void,
            stack_args_from: None,
        }
    }
    #[test]
    fn closure_excludes_cycles_unknown_calls_services_and_indirect_calls() {
        let mut module = Module::new("analysis".into());
        for f in [
            function("parent", vec![call("leaf"), I::Return(None)]),
            function("leaf", vec![I::Return(None)]),
            function("recursive", vec![call("recursive"), I::Return(None)]),
            function("service", vec![call("rcc_process_yield"), I::Return(None)]),
            function("unknown", vec![call("unknown_external"), I::Return(None)]),
            function("loop", vec![I::Branch(0)]),
        ] {
            module.add_function(f);
        }
        let mut indirect = function("indirect", vec![call("leaf"), I::Return(None)]);
        if let I::Call { function, .. } = &mut indirect.blocks[0].instructions[0] {
            *function = Value::Temp(0);
        }
        module.add_function(indirect);
        assert_eq!(
            direct_functions(&module),
            BTreeSet::from(["leaf".into(), "parent".into()])
        );
    }
    #[test]
    fn total_call_tree_cost_and_pointer_returns_are_conservative() {
        let mut module = Module::new("bounds".into());
        let mut leaf = function("leaf", vec![I::Comment("padding".into()); MAX_COST - 1]);
        leaf.blocks[0].instructions.push(I::Return(None));
        module.add_function(leaf);
        module.add_function(function("big", vec![call("leaf"), I::Return(None)]));
        let mut aggregate = function("aggregate", vec![I::Return(None)]);
        aggregate.return_type = IrType::FatPtr(Box::new(IrType::I16));
        module.add_function(aggregate);
        assert_eq!(direct_functions(&module), BTreeSet::from(["leaf".into()]));
    }

    #[test]
    fn exclusive_branches_use_longest_path_without_weakening_the_bound() {
        let mut diamond = function(
            "diamond",
            vec![I::BranchCond {
                condition: Value::Constant(1),
                true_label: 1,
                false_label: 2,
            }],
        );
        for id in [1, 2] {
            let mut block = BasicBlock::new(id);
            block.instructions = vec![I::Comment("work".into()); 180];
            block
                .instructions
                .push(I::Return(Some(Value::Constant(42))));
            diamond.blocks.push(block);
        }
        assert_eq!(bounded_cost(&diamond, &BTreeMap::new()), Some(182));
        let mut module = Module::new("branches".into());
        module.add_function(diamond.clone());
        module.add_function(function("parent", vec![call("diamond"), I::Return(None)]));
        // Two bounded calls on one path still exceed the original 256 limit.
        module.add_function(function(
            "twice",
            vec![call("diamond"), call("diamond"), I::Return(None)],
        ));
        assert_eq!(
            direct_functions(&module),
            BTreeSet::from(["diamond".into(), "parent".into()])
        );
        diamond.blocks[2].instructions = vec![I::Comment("work".into()); MAX_COST];
        diamond.blocks[2].instructions.push(I::Return(None));
        assert_eq!(bounded_cost(&diamond, &BTreeMap::new()), None);
    }
}
